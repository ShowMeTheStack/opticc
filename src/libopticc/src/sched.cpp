// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/sched.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace opticc {

static constexpr double kInf = std::numeric_limits<double>::infinity();

Scheduler::Scheduler(const Topology& t, const Workload& w, std::shared_ptr<CostPredictor> pred)
    : topo_(t), wl_(w), pred_(std::move(pred)) {
  t.check_ids();  // per-node arrays are indexed by id == position+1
  mean_mem_ = w.mean_mem();
  int n = t.num_nodes();
  cap_.resize(static_cast<size_t>(n));
  for (int k = 0; k < n; ++k) cap_[static_cast<size_t>(k)] = t.capacity(k, mean_mem_);
  window_ = cap_;
  if (t.params.local_slots >= 0) window_[0] = t.params.local_slots;
  if (t.nodes.empty()) window_[0] = std::max(window_[0], std::max(1, cap_[0]));   // liveness: no worker at all
  inflight_.assign(static_cast<size_t>(n), 0);
  hits_inflight_.assign(static_cast<size_t>(n), 0);
  disabled_.assign(static_cast<size_t>(n), 0);
  core_busy_until_.resize(static_cast<size_t>(n));
  uplink_free_at_.assign(static_cast<size_t>(n), 0.0);
  what_cache_.assign(w.jobs.size(), 0.0);
  what_ver_.assign(w.jobs.size(), 0);
  miss_ema_.assign(static_cast<size_t>(n), -1.0);  // -1: nothing observed, use the job's closure
  miss_n_.assign(static_cast<size_t>(n), 0);
  priced_ = t.params.priced;
  nu_ = t.params.nu >= 0 ? t.params.nu : 0.0;
  compute_priorities();
}

// ---- modes and shipped bytes -------------------------------------------------
std::vector<Mode> Scheduler::modes_for(const Job& j, int k) const {
  if (k == 0) return {Mode::Classic};
  bool hp_ok = j.src_size_bytes > 0;
  switch (topo_.params.mode_policy) {
    case ModePolicy::Classic: return {Mode::Classic};
    case ModePolicy::HeaderPush: return hp_ok ? std::vector<Mode>{Mode::HeaderPush} : std::vector<Mode>{Mode::Classic};
    case ModePolicy::Auto: return hp_ok ? std::vector<Mode>{Mode::Classic, Mode::HeaderPush} : std::vector<Mode>{Mode::Classic};
  }
  return {Mode::Classic};
}

double Scheduler::ship_bytes(const Job& j, Mode mode, int k) const {
  if (mode != Mode::HeaderPush || k <= 0) return shipped_bytes(j, mode, topo_.params, rho());
  double miss = miss_ema_[static_cast<size_t>(k)];
  return shipped_bytes(j, mode, topo_.params, rho(), miss);  // miss < 0 -> default (closure / prior)
}

// Observed misses: bytes/rho - s_src. EMA (0.2) per node; the first observation replaces the prior.
void Scheduler::on_shipped(int job, int node, Mode mode, double bytes_shipped) {
  if (mode != Mode::HeaderPush || node <= 0) return;
  const Job& j = wl_.jobs.at(static_cast<size_t>(job));
  double r = std::max(rho(), 1e-9);
  double miss = std::max(0.0, bytes_shipped / r - j.src_size_bytes);
  size_t k = static_cast<size_t>(node);
  if (miss_n_[k] == 0) miss_ema_[k] = miss; else miss_ema_[k] = 0.8 * miss_ema_[k] + 0.2 * miss;
  miss_n_[k]++;
}

double Scheduler::client_cpu_cost(const Job& j, int k, Mode mode) const {
  if (k == 0) return w_hat(j) / topo_.client.speed;
  return ship_client_cpu(j, mode) / topo_.client.speed;
}

Mode Scheduler::running_mode(int job) const {
  auto it = running_.find(job);
  if (it == running_.end() || it->second.empty()) return Mode::Classic;
  return it->second.front().mode;
}

// ---- shadow price of client CPU --------------------------------------------
// The client's cores are a shared resource between local compiles (w/f0) and
// shipping work (p/f0 classic, q/f0 header-push). Myopic ECT spends them on
// local compiles that could have fed ~w/p remote jobs. nu is the dual of
// the client-CPU row of the makespan LP bound, estimated online: raise it while the client
// is saturated and remote capacity idles, decay it otherwise.
void Scheduler::disable_node(int k) {
  disabled_.at(static_cast<size_t>(k)) = 1;
  bool any = false;
  for (int m = 1; m < topo_.num_nodes(); ++m) if (!disabled_[static_cast<size_t>(m)]) { any = true; break; }
  if (!any) window_[0] = std::max(window_[0], std::max(1, cap_[0]));   // liveness: last worker gone
}

void Scheduler::enable_node(int k) {
  size_t kk = static_cast<size_t>(k);
  if (k <= 0 || kk >= disabled_.size()) return;
  disabled_[kk] = 0;
  cap_[kk] = topo_.capacity(k, mean_mem_);
  window_[kk] = std::max(1, cap_[kk]);
  if (topo_.params.local_slots >= 0) window_[0] = topo_.params.local_slots;   // undo the no-worker liveness raise
}

bool Scheduler::remote_saturated() const {
  bool any = false;
  for (int k = 1; k < topo_.num_nodes(); ++k) {
    if (disabled_[static_cast<size_t>(k)]) continue;
    any = true;
    if (window_open(k)) return false;
  }
  return any;   // no enabled remote node at all: "not saturated" would strand every job, so the client may compile
}

int Scheduler::idle_remote_slots() const {
  int idle = 0;
  for (int k = 1; k < topo_.num_nodes(); ++k) {
    size_t kk = static_cast<size_t>(k);
    if (disabled_[kk] || !window_open(k)) continue;
    idle += std::max(0, cap_[kk] - inflight_[kk]);
  }
  return idle;
}

int Scheduler::best_idle_remote(const Job& j, double now, Mode* mode, bool pp_done) const {
  int best = -1; double bc = kInf; Mode bm = mode ? *mode : Mode::Classic;
  for (int k = 1; k < topo_.num_nodes(); ++k) {
    size_t kk = static_cast<size_t>(k);
    if (disabled_[kk] || !eligible(j, k) || !window_open(k) || inflight_[kk] >= cap_[kk]) continue;
    std::vector<Mode> ms = modes_for(j, k);
    if (pp_done && mode) { if (std::find(ms.begin(), ms.end(), *mode) == ms.end()) continue; ms = {*mode}; }
    for (Mode m : ms) { double c = predicted_completion(j, k, now, m, pp_done).done; if (c < bc) { bc = c; best = k; bm = m; } }
  }
  if (mode && best > 0) *mode = bm;
  return best;
}

// Client work conservation for header-push/auto. When the client is not the bottleneck (it does no preprocessing in
// header-push), its free cores are capacity like any worker slot: a job that would otherwise WAIT for a full remote
// node runs locally now. ECT alone uses the client only when its predictions say local finishes first, which needs
// near-exact job sizes; this rule needs none. Never in classic mode (every shipped job costs the client p_j there).
bool Scheduler::client_slot_free_for_waiting(const Job& j) const {
  if (!topo_.params.work_conserving || topo_.params.mode_policy == ModePolicy::Classic) return false;
  if (non_distributable(j) || !eligible(j, 0) || !window_open(0)) return false;
  size_t occ = core_busy_until_[0].size() + pp_busy_until_.size();
  return occ < static_cast<size_t>(std::max(1, cap_[0]));
}

bool Scheduler::local_gate(const Job& j) const {
  if (!topo_.params.work_conserving) return true;      // plain ECT (tests of the earliest-completion rule in isolation)
  if (non_distributable(j)) return true;
  bool any_remote = false;
  for (int k = 1; k < topo_.num_nodes(); ++k) if (!disabled_[static_cast<size_t>(k)] && eligible(j, k)) { any_remote = true; break; }
  if (!any_remote) return true;            // nothing else can run it
  if (remote_saturated()) return true;     // every remote slot busy: the client is spare capacity
  // localhost-last: the client is a stage. While a remote slot idles, a local compile is allowed only
  // if it takes no client capacity that preprocessing needs: a core is free AND (classic) no other job is waiting
  // for client-side work. ECT still decides among the allowed options (a lone small job may compile locally).
  size_t occ = core_busy_until_[0].size() + pp_busy_until_.size();
  if (occ >= static_cast<size_t>(std::max(1, cap_[0]))) return false;
  if (topo_.params.mode_policy == ModePolicy::Classic && (ready_.size() > 1 || !unbound_.empty())) return false;
  return true;
}

void Scheduler::sample_load(double now) {
  double dt = now - load_last_sample_;
  if (dt <= 0) return;
  size_t cap0 = static_cast<size_t>(std::max(1, cap_[0]));
  double occ = static_cast<double>(core_busy_until_[0].size() + pp_busy_until_.size());
  double util = std::min(1.0, occ / static_cast<double>(cap0));
  // Remote idleness from OBSERVED occupancy: a core is busy from an attempt's predicted compile start (jobs still in
  // transit, or queued behind the client's preprocessing, hold a window slot but no core) until the attempt actually
  // completes, not until its predicted finish. Otherwise an attempt that overran its prediction would count as an idle
  // core, so with an under-predicting w_hat "some node idles" would almost always hold and nu would sit at nu_max
  // while the workers are fully busy.
  // Certain cache hits occupy no core.
  std::vector<int> busy(static_cast<size_t>(topo_.num_nodes()), 0);
  for (auto& [jid, atts] : running_)
    for (auto& a : atts) if (a.node != 0 && !a.cache_hit && a.predicted_start <= now) busy[static_cast<size_t>(a.node)]++;
  bool idle = false;
  for (int k = 1; k < topo_.num_nodes() && !idle; ++k) {
    size_t kk = static_cast<size_t>(k);
    if (!disabled_[kk] && busy[kk] < cap_[kk]) idle = true;
  }
  acc_client_busy_ += util * dt; acc_remote_idle_ += (idle ? 1.0 : 0.0) * dt; acc_dt_ += dt;
  load_last_sample_ = now;
}

// nu_max = max_k (w/f_k) / (c/f0), c the mean client CPU a
// shipped job costs in the policy's current mode (p classic, q header-push), floored at the measured
// hashing/scan cost floor Params::min_client_cpu_s (never a fraction of p). Zero when nothing can be shipped.
double Scheduler::nu_max() const {
  double mean_w = 0, mean_c = 0; int n = 0;
  Mode m = topo_.params.mode_policy == ModePolicy::Classic ? Mode::Classic : Mode::HeaderPush;
  for (auto& j : wl_.jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) { mean_w += w_hat(j); mean_c += ship_client_cpu(j, m); ++n; }
  if (!n || mean_c <= 0) return 0.0;   // shipping costs no client CPU at all: client cores are just more compute, pricing is moot
  mean_w /= n; mean_c = std::max(mean_c / n, topo_.params.min_client_cpu_s);
  double fmin = kInf; for (auto& nd : topo_.nodes) if (!disabled_[static_cast<size_t>(nd.id)]) fmin = std::min(fmin, nd.speed);
  if (fmin == kInf || mean_c <= 0) return 0.0;
  return (mean_w / fmin) / (mean_c / topo_.client.speed);
}

void Scheduler::update_nu(double now) {
  if (!priced_ || topo_.params.nu >= 0) return;
  sample_load(now);
  if (now - nu_last_update_ < 2.0 && completions_ - nu_completions_at_ < 25) return;
  if (acc_dt_ > 0) {
    double util_w = acc_client_busy_ / acc_dt_, idle_w = acc_remote_idle_ / acc_dt_;
    // Smooth the window measurements (time constant 8 s): synchronised completion waves make a single window's
    // utilisation swing between 0 and 1, and a controller acting on raw windows tracks that wave.
    constexpr double kNuTau = 8.0;
    double wgt = std::min(1.0, acc_dt_ / kNuTau);
    if (util_ema_ < 0) { util_ema_ = util_w; idle_ema_ = idle_w; }
    else { util_ema_ += wgt * (util_w - util_ema_); idle_ema_ += wgt * (idle_w - idle_ema_); }
    double util = util_ema_, idle = idle_ema_;
    double nmax = nu_max();
    double before = nu_;
    // Multiplicative update: x1.5 (+0.01) while remote capacity idles
    // (the price is too low: local compiles are stealing client CPU the feed needs), x0.7 while the workers are
    // saturated and the client has slack (the price is too high), and HOLD when both are busy -- the balanced
    // state the LP dual describes. A plain "raise iff client saturated AND remote idle, else decay" rule
    // flip-flops every window in header-push (decay -> local compiles -> saturation -> raise -> ...).
    // Capped at nu_max; the warm-up jump to 10% of nu_max happens exactly once.
    // Raise while remote capacity idles (shipping is under-priced relative to local compiles); lower only while
    // the workers are saturated and the client has slack (the client could usefully compile); hold otherwise.
    if (idle >= 0.5) {
      nu_ = std::min(nu_ * 1.5 + 0.01, nmax);
      if (!nu_warmed_) { nu_ = std::min(std::max(nu_, 0.1 * nmax), nmax); nu_warmed_ = true; }
    } else if (util < 0.9) nu_ *= 0.7;
    if (nu_ < 1e-4) nu_ = 0;
    int dir = nu_ > before ? 1 : nu_ < before ? -1 : 0;
    if (dir != 0 && nu_last_dir_ != 0 && dir != nu_last_dir_) ++nu_dir_changes_;
    if (dir != 0) nu_last_dir_ = dir;
    ++nu_updates_; nu_peak_ = std::max(nu_peak_, nu_);
    if (std::getenv("OPTICC_NU_DEBUG")) std::fprintf(stderr, "nu-update t=%.1f util=%.2f(%.2f) idle=%.2f(%.2f) nu=%.4f nu_max=%.2f\n", now, util, util_w, idle, idle_w, nu_, nmax);
  }
  acc_client_busy_ = acc_remote_idle_ = acc_dt_ = 0;
  nu_last_update_ = now; nu_completions_at_ = completions_;
}

// Prediction cache keyed by the predictor's version: a learning
// predictor invalidates every cached value the moment it observes a job.
double Scheduler::w_hat(const Job& j) const {
  auto i = static_cast<size_t>(j.id);
  uint64_t v = pred_->version() + 1;
  if (what_ver_[i] != v) { what_cache_[i] = pred_->predict(j); what_ver_[i] = v; }
  return what_cache_[i];
}

void Scheduler::compute_priorities() {
  // longest path to a sink, job cost = w_hat / f0 (local time), reverse topological order
  size_t n = wl_.jobs.size();
  prio_.assign(n, 0.0);
  auto succ = wl_.successors();
  std::vector<int> indeg(n, 0), order, stack;
  for (auto& j : wl_.jobs) indeg[static_cast<size_t>(j.id)] = static_cast<int>(j.deps.size());
  for (auto& j : wl_.jobs) if (j.deps.empty()) stack.push_back(j.id);
  while (!stack.empty()) {
    int u = stack.back(); stack.pop_back(); order.push_back(u);
    for (int v : succ[static_cast<size_t>(u)]) if (--indeg[static_cast<size_t>(v)] == 0) stack.push_back(v);
  }
  if (order.size() != n) throw std::runtime_error("workload DAG has a cycle");
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    size_t u = static_cast<size_t>(*it); double best = 0;
    for (int v : succ[u]) best = std::max(best, prio_[static_cast<size_t>(v)]);
    prio_[u] = w_hat(wl_.jobs[u]) / topo_.client.speed + best;
  }
  prio_version_ = pred_->version();
}

void Scheduler::on_jobs_added() {
  size_t n = wl_.jobs.size();
  if (n <= what_cache_.size()) return;
  size_t old = what_cache_.size();
  what_cache_.resize(n, 0.0);
  what_ver_.resize(n, 0);
  prio_.resize(n, 0.0);
  for (size_t i = old; i < n; ++i) prio_[i] = w_hat(wl_.jobs[i]) / topo_.client.speed;
  dyn_.resize(n, 1);
  ready_t_.resize(n, 0.0);
}

// Critical-path (LPT for jobs without successors) order; run-time jobs go by arrival bucket first while
// jobs keep arriving. The key is lexicographic, so the order is a strict weak order also for mixed job sets.
void Scheduler::sort_by_priority(std::vector<int>& v, double now, bool cp, bool lpt_tiebreak) const {
  const bool aged = now - last_arrival_ < kTailQuiet;
  auto bucket = [&](int j) -> double {
    size_t u = static_cast<size_t>(j);
    if (!aged || u >= dyn_.size() || !dyn_[u]) return -1e300;
    return std::floor(ready_t_[u] / kAgeBucket);
  };
  std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
    double ba = bucket(a), bb = bucket(b);
    if (ba != bb) return ba < bb;
    double pa = cp ? prio_[static_cast<size_t>(a)] : 0.0, pb = cp ? prio_[static_cast<size_t>(b)] : 0.0;
    if (pa != pb) return pa > pb;
    return lpt_tiebreak && w_hat(wl_.jobs[static_cast<size_t>(a)]) > w_hat(wl_.jobs[static_cast<size_t>(b)]);
  });
}

void Scheduler::on_job_ready(int job, double now) {
  ready_.push_back(job);
  if (static_cast<size_t>(job) < ready_t_.size()) ready_t_[static_cast<size_t>(job)] = now;
  last_arrival_ = std::max(last_arrival_, now);
  // recent arrival times (5 s window) for the arrival-aware hedging guard
  arrivals_.push_back(now); last_now_ = std::max(last_now_, now);
  size_t drop = 0; while (drop < arrivals_.size() && arrivals_[drop] < now - 5.0) ++drop;
  if (drop) arrivals_.erase(arrivals_.begin(), arrivals_.begin() + static_cast<long>(drop));
}

// Late binding: the admitted job's client-side work is done; it now waits for a node.
void Scheduler::on_preprocessed(int job, double /*now*/) {
  auto it = unbound_.find(job);
  Mode m = it != unbound_.end() ? it->second.mode : Mode::Classic;
  if (it != unbound_.end()) unbound_.erase(it);
  auto pk = parked_.find(job);
  if (pk != parked_.end()) { m = pk->second; parked_.erase(pk); }
  staged_.push_back(job);
  staged_mode_[job] = m;
}

void Scheduler::park(int job) {
  auto it = unbound_.find(job);
  if (it == unbound_.end()) return;   // not admitted (early binding, or already staged): nothing to give back
  parked_[job] = it->second.mode;
  unbound_.erase(it);
}

void Scheduler::drop_attempts(int job) {
  auto it = running_.find(job);
  if (it == running_.end()) return;
  for (auto& a : it->second) {
    if (!a.cache_hit) inflight_[static_cast<size_t>(a.node)]--;
    else if (hits_inflight_[static_cast<size_t>(a.node)] > 0) hits_inflight_[static_cast<size_t>(a.node)]--;
    auto& ms = core_busy_until_[static_cast<size_t>(a.node)];
    auto f = a.cache_hit ? ms.end() : ms.find(a.predicted_done);
    if (f != ms.end()) ms.erase(f);
  }
  running_.erase(it);
}

bool Scheduler::requeue_lost(int job, Mode mode) {
  if (!late_binding()) return false;
  drop_attempts(job);
  unbound_.erase(job); parked_.erase(job);
  staged_.erase(std::remove(staged_.begin(), staged_.end(), job), staged_.end());
  staged_.push_back(job);
  staged_mode_[job] = mode;
  return true;
}

void Scheduler::rebind_local(int job, double now) {
  drop_attempts(job);
  unbound_.erase(job); parked_.erase(job); staged_mode_.erase(job);
  staged_.erase(std::remove(staged_.begin(), staged_.end(), job), staged_.end());
  record_dispatch(wl_.jobs[static_cast<size_t>(job)], 0, now, false, Mode::Classic);
}

void Scheduler::on_job_done(int job, int /*node*/, double now, double measured_w) {
  drop_attempts(job);
  affinity_.erase(job);   // job ids are unique: drop the entry so it does not leak
  // defensive: a job finished through a path we did not bind (coordinator fallback) must not linger
  unbound_.erase(job); staged_mode_.erase(job); parked_.erase(job);
  staged_.erase(std::remove(staged_.begin(), staged_.end(), job), staged_.end());
  pred_->observe(wl_.jobs[static_cast<size_t>(job)], measured_w);
  ++completions_;
  // Priorities and admission windows depend on predictions: refresh periodically once the predictor has moved.
  if (pred_->version() != prio_version_ && completions_ % 25 == 0) { compute_priorities(); refresh(now); }
}

double Scheduler::next_wakeup(double /*now*/) const { return kInf; }

void Scheduler::prune(double now) {
  // preprocessing pseudo-occupancy is not tracked by completion; age it out
  while (!pp_busy_until_.empty() && *pp_busy_until_.begin() <= now) pp_busy_until_.erase(pp_busy_until_.begin());
}

double Scheduler::predicted_core_start(int k, double arrival, double now) const {
  auto& ms = core_busy_until_[static_cast<size_t>(k)];
  size_t occupied = ms.size() + (k == 0 ? pp_busy_until_.size() : 0);
  size_t cap = static_cast<size_t>(std::max(0, cap_[static_cast<size_t>(k)]));
  if (cap == 0) return kInf;  // a node without usable cores never starts anything
  if (occupied < cap) return arrival;
  // the job gets a core when (occupied - cap + 1) current occupants have finished
  size_t need = occupied - cap + 1;
  std::vector<double> all(ms.begin(), ms.end());
  if (k == 0) all.insert(all.end(), pp_busy_until_.begin(), pp_busy_until_.end());
  std::nth_element(all.begin(), all.begin() + static_cast<long>(need - 1), all.end());
  double t = all[need - 1];
  return std::max({arrival, t, now});
}

// Predicted time at which the (q+1)-th job queued for node k gets a window slot: immediately while free
// slots remain, otherwise when the required number of current in-flight jobs have completed, and beyond
// the in-flight set by extrapolating the slot recycle rate (mean remaining duration / in-flight count).
double Scheduler::predicted_slot_free(int k, double now, int q) const {
  size_t kk = static_cast<size_t>(k);
  int free_slots = window_[kk] - inflight_[kk];
  if (q < free_slots) return now;
  auto& ms = core_busy_until_[kk];
  if (ms.empty()) return kInf;  // nothing in flight and no free slot (window zero): never
  size_t n = ms.size();
  size_t need = static_cast<size_t>(q - std::max(free_slots, 0));   // completions required before our turn
  if (need < n) {
    auto it = ms.begin(); std::advance(it, static_cast<long>(need));
    return std::max(now, *it);
  }
  double sum = 0; for (double d : ms) sum += std::max(d - now, 0.0);
  double mean_dur = std::max(sum / static_cast<double>(n), 1e-6);
  double last = std::max(now, *ms.rbegin());
  return last + static_cast<double>(need - n + 1) * mean_dur / static_cast<double>(n);
}

Scheduler::Prediction Scheduler::predicted_completion(const Job& j, int k, double t0, Mode mode, bool pp_done) const {
  Prediction r;
  double what = w_hat(j);
  const Client& c = topo_.client;
  double ovh = overhead() / c.speed;   // make/fixdep/objtool/zstd per job, on the client, every policy
  if (k == 0) {
    double start = predicted_core_start(0, t0, t0);
    r.pp_done = r.up_done = t0; r.start = start;
    r.remote = (what + overhead()) / c.speed;
    r.done = start + (what + overhead()) / c.speed;
    return r;
  }
  const Node& nd = topo_.node(k);
  Prediction hitp; double p_hit = 0;
  if (cache_hit_expected(j.id, k)) {   // object cached on k — no worker core, only source up + object down
    const Affinity& af = affinity_.at(j.id);
    p_hit = hit_p(j.id, k);
    if (p_hit >= kCertainHit && !cache_hit_certain(j.id, k)) p_hit = std::min(p_hit, 0.9799);   // capped: reserve
    double client_cpu = pp_done ? 0.0 : (ship_client_cpu(j, mode) / c.speed + ovh);
    double pp_start = client_cpu > 0 ? predicted_core_start(0, t0, t0) : t0;
    r.pp_done = pp_start + client_cpu;
    double bytes = mode == Mode::Classic ? ship_bytes(j, mode) : rho() * j.src_size_bytes;
    double up_start = std::max(r.pp_done + alpha() * nd.rtt_s, uplink_free_at_[static_cast<size_t>(k)]);
    r.up_done = up_start + xfer_s(bytes, nd.bw_up_bps);
    r.start = r.up_done;
    r.done = r.up_done + af.hit_cost_s + xfer_s(j.obj_size_bytes, nd.bw_down_bps);
    r.remote = client_cpu + alpha() * nd.rtt_s + xfer_s(bytes, nd.bw_up_bps) + af.hit_cost_s + xfer_s(j.obj_size_bytes, nd.bw_down_bps);
    if (p_hit >= kCertainHit) return r;
    hitp = r;   // uncertain: blended with the miss prediction below (expected completion time)
  }
  // client-side CPU before shipping: p_j (classic) or q_j (header-push) plus the per-job overhead; needs a
  // client core when > 0. Under late binding the staged job has already paid it (pp_done).
  double client_cpu = pp_done ? 0.0 : (ship_client_cpu(j, mode) / c.speed + ovh);
  double pp_start = client_cpu > 0 ? predicted_core_start(0, t0, t0) : t0;
  r.pp_done = pp_start + client_cpu;
  double bytes = ship_bytes(j, mode, k);
  double up_start = std::max(r.pp_done + alpha() * nd.rtt_s, uplink_free_at_[static_cast<size_t>(k)]);
  r.up_done = up_start + xfer_s(bytes, nd.bw_up_bps);
  double start = predicted_core_start(k, r.up_done, t0);
  r.start = start;
  double remote_cpu = remote_compile_work(what, j.preprocess_cpu_s, mode) / nd.speed;  // w from source
  r.done = start + remote_cpu + xfer_s(j.obj_size_bytes, nd.bw_down_bps);
  // uncontended R_jk with this policy's transport (alpha, rho)
  r.remote = client_cpu + alpha() * nd.rtt_s + xfer_s(bytes, nd.bw_up_bps) + remote_cpu + xfer_s(j.obj_size_bytes, nd.bw_down_bps);
  if (p_hit > 0) {   // E[completion] = p·T_hit + (1-p)·T_miss for an uncertain (Bloom) hit
    r.done = p_hit * hitp.done + (1 - p_hit) * r.done;
    r.remote = p_hit * hitp.remote + (1 - p_hit) * r.remote;
  }
  return r;
}

void Scheduler::record_admit(const Job& j, double now, Mode mode) {
  double client_cpu = (ship_client_cpu(j, mode) + overhead()) / topo_.client.speed;
  double pp_start = client_cpu > 0 ? predicted_core_start(0, now, now) : now;
  double pp_done = pp_start + client_cpu;
  if (pp_done > now) pp_busy_until_.insert(pp_done);
  unbound_[j.id] = Unbound{mode, now, pp_done};
}

void Scheduler::record_dispatch(const Job& j, int k, double now, bool hedge, Mode mode, bool pp_done) {
  if (k == 0) mode = Mode::Classic;
  Prediction pr = predicted_completion(j, k, now, mode, pp_done);
  if (k != 0) {
    uplink_free_at_[static_cast<size_t>(k)] = pr.up_done;
    if (!pp_done && pr.pp_done > now) pp_busy_until_.insert(pr.pp_done);
  }
  bool hit = k != 0 && cache_hit_certain(j.id, k);   // an uncertain hit reserves a core and a window slot
  if (!hit) core_busy_until_[static_cast<size_t>(k)].insert(pr.done);   // a cache hit takes no worker core
  if (!hit) inflight_[static_cast<size_t>(k)]++;   // hits do not occupy the pipeline window either
  else hits_inflight_[static_cast<size_t>(k)]++;    // ...but are capped at window_[k] in flight
  Attempt a{k, mode, now, pr.start, pr.done, pr.remote, hedge, kInf, hit};
  if (k != 0 && !hedge) a.next_check = first_hedge_check(a, now);
  running_[j.id].push_back(a);
}

void Scheduler::set_affinity(int job, const std::vector<int>& nodes, double hit_cost_s, double p_hit) {
  set_affinity(job, nodes, hit_cost_s, std::vector<double>(nodes.size(), p_hit));
}
void Scheduler::set_affinity(int job, const std::vector<int>& nodes, double hit_cost_s, const std::vector<double>& p_per_node) {
  std::vector<int> ok; std::vector<double> ps;
  for (size_t i = 0; i < nodes.size(); ++i) {
    int k = nodes[i];
    if (k <= 0 || k >= topo_.num_nodes()) continue;
    ok.push_back(k); ps.push_back(std::min(1.0, std::max(0.0, i < p_per_node.size() ? p_per_node[i] : 1.0)));
  }
  if (ok.empty()) { affinity_.erase(job); return; }
  double pmax = *std::max_element(ps.begin(), ps.end());
  affinity_[job] = Affinity{ok, std::max(0.0, hit_cost_s), pmax, ps};
}
double Scheduler::hit_p(int job, int k) const {
  auto it = affinity_.find(job);
  if (it == affinity_.end()) return 0.0;
  const Affinity& a = it->second;
  for (size_t i = 0; i < a.nodes.size(); ++i) if (a.nodes[i] == k) return i < a.p_node.size() ? a.p_node[i] : a.p_hit;
  return 0.0;
}
bool Scheduler::cache_hit_certain(int job, int k) const {
  if (k <= 0) return false;
  if (hit_p(job, k) < kCertainHit) return false;
  return hits_inflight_[static_cast<size_t>(k)] < std::max(1, window_[static_cast<size_t>(k)]);   // capped per node
}
void Scheduler::clear_affinity(int job) { affinity_.erase(job); }
bool Scheduler::cache_hit_expected(int job, int k) const {
  auto it = affinity_.find(job);
  if (it == affinity_.end()) return false;
  for (int n : it->second.nodes) if (n == k) return true;
  return false;
}

bool Scheduler::eligible(const Job& j, int k) const {
  if (disabled_[static_cast<size_t>(k)]) return false;
  if (k == 0 && non_distributable(j)) return true;   // barrier / unshippable jobs bypass local_slots
  if (window_[static_cast<size_t>(k)] <= 0) return false;
  if (k != 0 && non_distributable(j)) return false;  // nothing to ship / must be local
  return true;
}

// ---- ECT ----------------------------------------------------------------
ECTScheduler::ECTScheduler(const Topology& t, const Workload& w, std::shared_ptr<CostPredictor> pred,
                           ECTOptions opt)
    : Scheduler(t, w, std::move(pred)), opt_(opt) {
  hedge_max_ = t.params.hedge_max >= 0 ? t.params.hedge_max : static_cast<int>(t.nodes.size());
  if (opt_.bdp_window) recompute_windows();
  recompute_npp();
  // Adaptive shadow price starts at a small prior (5% of nu_max) rather than 0: the first
  // update window would otherwise be spent in the myopic trap; decays if unwarranted.
  if (priced_ && t.params.nu < 0 && !t.nodes.empty()) nu_ = 0.05 * nu_max();
}

void ECTScheduler::refresh(double /*now*/) {
  if (opt_.bdp_window) recompute_windows();
  recompute_npp();
}

// Window size: N_k* = ceil(mu_k * D_k) with
//   mu_k = min( ~c_k f_k / w,  B_up / b,  B_down / o )       [jobs/s]
//   D_k  = p/f0 + alpha L_k + b/B_up + w/f_k + o/B_down       [uncontended cycle time]
// The window counts jobs from dispatch to object receipt, and dispatch happens
// before client preprocessing, so p/f0 is part of the cycle.
// Means are taken from the workload (predicted work, so the policy only uses
// what it can know); until anything is known we fall back to priors
// (1.5 MB preprocessed, 0.1 x obj, p = 0.15 w).
// mu_k and D_k for node k from workload means. Under late binding the client-side phase is not
// part of the node's pipeline: D_k excludes p/f0 (q/f0) and the window counts from ship time.
void Scheduler::node_mu_D(int kk, double* mu_out, double* D_out) const {
  const Topology& t = topo_;
  const Workload& w = wl_;
  double mean_s = 0, mean_o = 0, mean_w = 0, mean_p = 0;
  int n = 0;
  for (auto& j : w.jobs)
    if (!j.is_link_barrier && j.pre_size_bytes > 0) { mean_s += j.pre_size_bytes; mean_o += j.obj_size_bytes; mean_w += w_hat(j); mean_p += j.preprocess_cpu_s; ++n; }
  if (n) { mean_s /= n; mean_o /= n; mean_w /= n; mean_p /= n; }
  else {
    Job prior; prior.id = 0; prior.pre_size_bytes = 1.5e6;
    mean_s = prior.pre_size_bytes; mean_o = 0.1 * mean_s;
    mean_w = pred_->predict(prior); mean_p = 0.15 * mean_w;
  }
  if (mean_w <= 0) mean_w = 1.0;  // oracle predictor on an unknown job: assume 1 s
  // header-push: the client pays q, ships s_src + misses, the worker adds p to the compile
  bool hp = t.params.mode_policy == ModePolicy::HeaderPush;
  double mean_src = 0, mean_q = 0;
  if (hp && n) { for (auto& j : w.jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) { mean_src += j.src_size_bytes; mean_q += ship_client_cpu(j, Mode::HeaderPush); } mean_src /= n; mean_q /= n; }
  if (!hp && n) { mean_p = 0; for (auto& j : w.jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) mean_p += ship_client_cpu(j, Mode::Classic); mean_p /= n; }
  const Node& nd = t.node(kk);
  size_t k = static_cast<size_t>(kk);
  double mean_b = rho() * mean_s, client_c = mean_p, remote_w = remote_compile_work(mean_w, mean_p, Mode::Classic);
  if (hp) { double miss = miss_ema_[k] >= 0 ? miss_ema_[k] : 0.0; mean_b = rho() * (mean_src + miss); client_c = mean_q; remote_w = mean_w; }
  double mu_compute = cap_[k] * nd.speed / remote_w;
  double mu_up = nd.bw_up_bps > 0 ? nd.bw_up_bps / (8.0 * std::max(mean_b, 1.0)) : kInf;
  double mu_down = nd.bw_down_bps > 0 && mean_o > 0 ? nd.bw_down_bps / (8.0 * mean_o) : kInf;
  double mu = std::min({mu_compute, mu_up, mu_down});
  double D = (late_binding() ? 0.0 : (client_c + overhead()) / t.client.speed) + alpha() * nd.rtt_s + xfer_s(mean_b, nd.bw_up_bps) +
             remote_w / nd.speed + xfer_s(mean_o, nd.bw_down_bps);
  if (mu_out) *mu_out = mu;
  if (D_out) *D_out = D;
}

// N^pp: jobs admitted to client-side preprocessing but not yet bound, enough to keep every
// node's pipeline fed: ceil(c0 + t_pp * sum_k mu_k), t_pp the mean client-side time per shipped job.
void Scheduler::recompute_npp() {
  if (!late_binding()) { npp_ = std::numeric_limits<int>::max(); return; }
  double mean_c = 0; int n = 0;
  bool hp = topo_.params.mode_policy == ModePolicy::HeaderPush;
  for (auto& j : wl_.jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) { mean_c += ship_client_cpu(j, hp ? Mode::HeaderPush : Mode::Classic) + overhead(); ++n; }
  double t_pp = n ? mean_c / n / topo_.client.speed : 0.0;
  double sum_mu = 0;
  for (auto& nd : topo_.nodes) if (!disabled_[static_cast<size_t>(nd.id)]) { double mu = 0; node_mu_D(nd.id, &mu, nullptr); if (mu < kInf) sum_mu += mu; }
  double c0 = static_cast<double>(std::max(1, cap_[0]));
  npp_ = std::max(1, static_cast<int>(std::ceil(c0 + t_pp * sum_mu - 1e-9)));
  feed_rate_ = t_pp > 0 ? std::min(sum_mu, c0 / t_pp) : sum_mu;
  // Staged pool: at most one window's worth of ready-to-ship jobs across the remote nodes, so that
  // binding still chooses by priority among a full pool (list scheduling) without buffering every
  // preprocessed file of the build.
  staged_cap_ = 0;
  for (auto& nd : topo_.nodes) if (!disabled_[static_cast<size_t>(nd.id)]) staged_cap_ += std::max(1, window_[static_cast<size_t>(nd.id)]);
  staged_cap_ = std::max(staged_cap_, npp_);
}

void ECTScheduler::recompute_windows() {
  for (auto& nd : topo_.nodes) {
    size_t k = static_cast<size_t>(nd.id);
    double mu = 0, D = 0; node_mu_D(nd.id, &mu, &D);
    double nstar = std::ceil(mu * D - 1e-9);
    if (topo_.params.window_scale <= 0) { window_[k] = std::max(1, cap_[k]); continue; }   // OPTICC_WINDOW=slots
    window_[k] = std::max(1, static_cast<int>(std::lround(nstar * topo_.params.window_scale)));
    if (topo_.params.work_conserving) window_[k] = std::max(window_[k], std::max(1, cap_[k]));   // work conservation: at least every core
  }
}

std::string ECTScheduler::name() const {
  std::string s = "ect";
  if (opt_.bdp_window) s += "-window";
  if (opt_.hedge == HedgeRule::Kappa) s += "-hedge";
  if (opt_.hedge == HedgeRule::T7) s += "-hedge-t7";
  if (!priced_) s += "-unpriced";
  if (!late_binding()) s += "-early";
  return s;
}

int ECTScheduler::best_node(const Job& j, double now, int exclude, double* completion, bool* blocked,
                            const std::vector<int>* pending, Mode* mode_out) const {
  int best = -1; double bc = kInf, bcost = kInf; bool bb = false; Mode bm = Mode::Classic;
  for (int k = 0; k < topo_.num_nodes(); ++k) {
    if (k == exclude || !eligible(j, k)) continue;
    int q = pending ? (*pending)[static_cast<size_t>(k)] : 0;
    bool hitk = k > 0 && cache_hit_certain(j.id, k);   // a (certain) hit needs no window slot
    double t0 = hitk ? now : predicted_slot_free(k, now, q);
    if (t0 == kInf) continue;
    for (Mode m : modes_for(j, k)) {
      double c = predicted_completion(j, k, t0, m).done;
      // price client CPU at nu; a 1 ms/CPU-s floor breaks exact ties (same completion,
      // e.g. classic vs header-push on an idle client) towards the option that burns less client CPU.
      double cost = c + (priced_ ? std::max(nu_, 1e-3) * client_cpu_cost(j, k, m) : 0.0);
      if (cost < bcost) { bcost = cost; bc = c; best = k; bm = m; bb = !hitk && ((t0 > now) || !window_open(k)); }
    }
  }
  if (completion) *completion = bc;
  if (blocked) *blocked = bb;
  if (mode_out) *mode_out = bm;
  return best;
}

// ---- survival-ratio hedging ingredients --------------------------------------
// S(x) = P(actual/predicted > x). Empirical once >= 30 remote completions have
// been observed, log-normal prior (median 1, sigma hedge_prior_sigma) before.
double ECTScheduler::survival(double ratio) const {
  if (ratio <= 0) return 1.0;
  if (ratios_.size() >= 30) {
    auto it = std::upper_bound(ratios_.begin(), ratios_.end(), ratio);
    return static_cast<double>(ratios_.end() - it) / static_cast<double>(ratios_.size());
  }
  double sigma = std::max(topo_.params.hedge_prior_sigma, 1e-6);
  return 0.5 * std::erfc(std::log(ratio) / (sigma * std::sqrt(2.0)));
}

// lambda = min(1, ready predicted work / idle capacity), floored at lambda_min:
// duplicates are cheap when slots are idle (end of a build), dear when ready
// work is waiting for them.
double ECTScheduler::pending_work(double horizon) const {
  double w = 0;
  for (int jid : ready_) w += w_hat(wl_.jobs[static_cast<size_t>(jid)]);
  for (auto& [jid, u] : unbound_) w += w_hat(wl_.jobs[static_cast<size_t>(jid)]);
  for (int jid : staged_) w += w_hat(wl_.jobs[static_cast<size_t>(jid)]);
  if (horizon > 0 && !arrivals_.empty()) {
    // arrivals within the last 5 s of NOW (the list is trimmed only when jobs arrive, so in the build's tail it
    // would otherwise keep the steady-state rate forever and block every hedge)
    size_t first = 0; while (first < arrivals_.size() && arrivals_[first] < last_now_ - 5.0) ++first;
    size_t cnt = arrivals_.size() - first;
    double span = cnt ? std::max(1.0, std::min(5.0, last_now_ - arrivals_[first])) : 5.0;
    double rate = cnt ? static_cast<double>(cnt) / span : 0.0;
    double mean_w = 0; int n = 0;
    for (auto& j : wl_.jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) { mean_w += w_hat(j); ++n; }
    mean_w = n ? mean_w / n : 1.0;
    w += rate * horizon * mean_w;
  }
  return w;
}

// lambda = min(1, pending work / idle capacity), floored at lambda_min: duplicates are cheap when slots are idle
// (end of a build), dear when work is waiting for them. Pending work counts admitted/staged jobs and the work
// expected to arrive within the duplicate's horizon (make releases jobs continuously; under early binding the
// ready list is almost always empty, so counting ready work alone gives lambda ~ 0 and needless hedges).
double ECTScheduler::lambda_now(double horizon) const {
  double ready_work = pending_work(horizon);
  double idle_slots = 0;
  for (int k = 0; k < topo_.num_nodes(); ++k)
    if (!disabled_[static_cast<size_t>(k)]) idle_slots += std::max(0, window_[static_cast<size_t>(k)] - inflight_[static_cast<size_t>(k)]);
  double mean_w = 0; int n = 0;
  for (auto& j : wl_.jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) { mean_w += w_hat(j); ++n; }
  mean_w = n ? mean_w / n : 1.0;
  double idle_work = idle_slots * std::max(mean_w, 1e-9);
  double lam = idle_work <= 0 ? 1.0 : std::min(1.0, ready_work / idle_work);
  return std::max(lam, topo_.params.hedge_lambda_min);
}

// Decide whether attempt `a` of job j should be duplicated now, given the
// fresh alternative would finish r seconds from now. Sets *recheck_dt when not due.
bool ECTScheduler::hedge_due(const Attempt& a, const Job& /*j*/, double now, double r, double* recheck_dt) const {
  double expected = std::max({a.predicted_done - a.dispatched, a.predicted_remote, 1e-9});
  double t = now - a.dispatched;
  if (opt_.hedge == HedgeRule::Kappa) {
    double threshold = topo_.params.hedge_kappa * expected;
    if (t + 1e-12 >= threshold) return true;
    *recheck_dt = threshold - t;
    return false;
  }
  // Survival-ratio rule: hedge iff P(W > t + r | W > t) >= lambda / (1 + lambda), evaluated on
  // the normalised completion W/expected. A just-dispatched job is never
  // duplicated (t must be positive: one check interval of information).
  double step = std::max(0.05 * expected, 1e-3);
  if (t <= 0) { *recheck_dt = step; return false; }
  // Guard: never duplicate while the idle capacity is spoken for by waiting or arriving work -- a duplicate then
  // delays real work, so hedging could only make the build slower than not hedging.
  {
    double idle_slots = 0;
    for (int k = 0; k < topo_.num_nodes(); ++k)
      if (!disabled_[static_cast<size_t>(k)]) idle_slots += std::max(0, window_[static_cast<size_t>(k)] - inflight_[static_cast<size_t>(k)]);
    double mean_w = 0; int n = 0;
    for (auto& jj : wl_.jobs) if (!jj.is_link_barrier && jj.pre_size_bytes > 0) { mean_w += w_hat(jj); ++n; }
    mean_w = n ? mean_w / n : 1.0;
    if (pending_work(r) >= idle_slots * mean_w) { *recheck_dt = step; return false; }
  }
  double s_t = survival(t / expected);
  double lam = lambda_now(r);
  double thresh = lam / (1.0 + lam);
  if (s_t <= 1e-12) return true;  // beyond every observation: the duplicate surely wins
  double s_tr = survival((t + r) / expected);
  if (s_tr / s_t > thresh) return true;
  *recheck_dt = step;
  return false;
}

double ECTScheduler::first_hedge_check(const Attempt& a, double now) const {
  if (opt_.hedge == HedgeRule::None) return kInf;
  double expected = std::max({a.predicted_done - a.dispatched, a.predicted_remote, 1e-9});
  if (opt_.hedge == HedgeRule::Kappa) return now + topo_.params.hedge_kappa * expected;
  return now + std::max(0.05 * expected, 1e-3);
}

// Earliest pending hedging check, including overdue ones: a wall-clock driver that polls (the coordinator's
// 50 ms timer thread) must still see a check whose time passed between polls.
double ECTScheduler::next_wakeup(double /*now*/) const {
  if (opt_.hedge == HedgeRule::None) return kInf;
  double best = kInf;
  for (auto& [jid, atts] : running_)
    if (atts.size() == 1 && atts[0].node != 0) best = std::min(best, atts[0].next_check);
  return best;
}

void ECTScheduler::on_job_done(int job, int node, double now, double measured_w) {
  if (node != 0) {
    auto it = running_.find(job);
    if (it != running_.end()) {
      // the winning attempt is the one on `node`; prefer the original over a hedge
      const Attempt* win = nullptr;
      for (auto& a : it->second) if (a.node == node && (!win || (win->hedge && !a.hedge))) win = &a;
      if (win) {
        double expected = std::max({win->predicted_done - win->dispatched, win->predicted_remote, 1e-9});
        double ratio = (now - win->dispatched) / expected;
        ratios_.insert(std::upper_bound(ratios_.begin(), ratios_.end(), ratio), ratio);
      }
    }
  }
  Scheduler::on_job_done(job, node, now, measured_w);
}

std::vector<Decision> ECTScheduler::decide(double now) {
  std::vector<Decision> out;
  last_now_ = std::max(last_now_, now);
  prune(now);
  update_nu(now);
  // Survival-ratio / kappa rule: duplicate stragglers before admitting new work. Evaluated when
  // the driver calls us at next_wakeup(), in job-id order.
  if (opt_.hedge != HedgeRule::None) {
    int active = 0;
    for (auto& [jid, atts] : running_) if (atts.size() > 1) ++active;
    for (auto& [jid, atts] : running_) {
      if (active >= hedge_max_) break;
      if (atts.size() != 1 || atts[0].node == 0) continue;
      Attempt& a = atts[0];
      if (a.next_check > now) continue;
      const Job& j = wl_.jobs[static_cast<size_t>(jid)];
      // the duplicate goes to the best OPEN remote node other than the original's (best_node may answer "the client",
      // which the remote-only rule would then skip)
      double c = kInf; Mode m = Mode::Classic; int k = -1;   // only OPEN windows are candidates: never "blocked"
      for (int kk = 1; kk < topo_.num_nodes(); ++kk) {
        if (kk == a.node || !eligible(j, kk) || !window_open(kk)) continue;
        for (Mode mm : modes_for(j, kk)) { double ck = predicted_completion(j, kk, now, mm).done; if (ck < c) { c = ck; k = kk; m = mm; } }
      }
      double recheck = std::max(0.05 * std::max(a.predicted_done - a.dispatched, 1e-3), 1e-3);
      if (k <= 0) { a.next_check = now + recheck; continue; }   // remote duplicates only
      if (!hedge_due(a, j, now, c - now, &recheck)) { a.next_check = now + std::max(recheck, 1e-6); continue; }
      double what = w_hat(j);
      m = a.mode;   // a duplicate reuses the original's mode (the .i or the closure already exists)
      record_dispatch(j, k, now, true, m);
      out.push_back(Decision{jid, k, m, true, false, what});
      ++hedges_issued_; ++active;
    }
  }
  if (late_binding()) { decide_bindings(now, &out); decide_admissions(now, &out); return out; }
  if (ready_.empty()) return out;
  sort_by_priority(ready_, now, opt_.cp_priority, false);
  std::vector<int> remaining;
  remaining.reserve(ready_.size());
  auto any_open = [&]() { for (int k = 0; k < topo_.num_nodes(); ++k) if (!disabled_[static_cast<size_t>(k)] && window_open(k)) return true; return false; };
  bool open = any_open();
  // Deferred jobs ahead in this pass count as queued for their preferred node, so
  // later jobs see a growing wait there and may prefer an open node.
  std::vector<int> pending(static_cast<size_t>(topo_.num_nodes()), 0);
  for (size_t i = 0; i < ready_.size(); ++i) {
    int jid = ready_[i];
    const Job& j = wl_.jobs[static_cast<size_t>(jid)];
    // only a CERTAIN hit may bypass closed windows (an uncertain one would queue on its worker's cores)
    auto af = affinity_.find(jid);
    bool certain = false;
    if (af != affinity_.end()) for (int hk : af->second.nodes) if (cache_hit_certain(jid, hk)) { certain = true; break; }
    if (!open && !non_distributable(j) && !certain) { remaining.push_back(jid); continue; }
    if (non_distributable(j)) {   // bypass the local window (barrier / nothing to ship)
      double what0 = w_hat(j); record_dispatch(j, 0, now, false, Mode::Classic);
      out.push_back(Decision{jid, 0, Mode::Classic, false, false, what0}); continue;
    }
    bool blocked = false; Mode m = Mode::Classic;
    int k = best_node(j, now, -1, nullptr, &blocked, &pending, &m);
    if (k == 0 && !local_gate(j)) { blocked = false; k = best_node(j, now, 0, nullptr, &blocked, &pending, &m); }   // dominance gate
    // work conservation: never wait for a faster node while a remote slot idles, unless fewer jobs are
    // ready than slots are idle (tail rule)
    // a job whose hint on k is trusted (p >= kCertainHit) but over k's in-flight cap waits for its holder:
    // moving it (work-conservation override, client fallback) turns a near-certain hit into a miss
    const bool waits_for_holder = k > 0 && blocked && hit_p(jid, k) >= kCertainHit;
    if (topo_.params.work_conserving && (k < 0 || blocked) && !waits_for_holder) {
      int idle = idle_remote_slots();
      if (idle > 0 && static_cast<int>(ready_.size() - i) >= idle) {
        Mode mm = m; int kf = best_idle_remote(j, now, &mm, false);
        if (kf > 0) { k = kf; blocked = false; m = mm; }
      }
    }
    if (k > 0 && !blocked) k = core_level_i2(j, k, now, &m, false, ready_.size() - i);
    if ((k < 0 || blocked) && !waits_for_holder && client_slot_free_for_waiting(j)) { k = 0; blocked = false; m = Mode::Classic; }   // client work conservation
    // If the best option is a window-blocked node, wait for it rather
    // than settling for a slower open node.
    if (k < 0) { remaining.push_back(jid); continue; }
    if (blocked) { pending[static_cast<size_t>(k)]++; remaining.push_back(jid); continue; }
    double what = w_hat(j);
    record_dispatch(j, k, now, false, m);
    out.push_back(Decision{jid, k, m, false, false, what});
    open = any_open();
  }
  ready_.swap(remaining);
  return out;
}

// Work conservation at CORE level. Windows may exceed c_k (max(ceil(mu D) scale, c_k)), and ECT evaluated at
// t0 = now may prefer queueing on a full fast node over an idle core elsewhere, while slot-level work conservation
// only applies when the ECT choice is blocked or absent. So a choice k with in_flight_k >= c_k is replaced by the
// best remote node with an idle core (best_idle_remote: window open, in_flight < c) whenever at least as many jobs
// are waiting as cores idle (outside the tail; in the tail, skipping slow cores stays allowed). Exempt: a job hinted
// to k (any affinity, certain or not): moving it off its holder turns a likely hit into a miss.
// Invariant: at a decision instant outside the tail, no job is bound to a node whose cores are all taken while an
// enabled remote node with an open window has a core free, except hinted jobs.
// in_flight counts attempts still in transit, so a core idle only because its job is uploading is not seen (conservative).
int ECTScheduler::core_level_i2(const Job& j, int k, double now, Mode* m, bool pp_done, size_t left) {
  if (!topo_.params.work_conserving || k <= 0 || inflight_[static_cast<size_t>(k)] < cap_[static_cast<size_t>(k)]) return k;
  if (hit_p(j.id, k) > 0) return k;
  int idle = idle_remote_slots();
  if (idle <= 0 || left < static_cast<size_t>(idle)) return k;
  Mode mm = m ? *m : Mode::Classic;
  int kf = best_idle_remote(j, now, &mm, pp_done);
  if (kf <= 0 || kf == k) return k;
  if (m) *m = mm;
  ++i2_core_moves_;
  return kf;
}

// ---- late binding ----------------------------------------------------------
// Node choice for a staged job: priced ECT over remote nodes with pp_done semantics (the client-side
// work is sunk, so only the local option still costs client CPU), window-blocked nodes evaluated at their
// predicted slot-free time. The client stays a candidate: if every remote window is full for
// long enough, compiling locally from source beats waiting, exactly as under early binding.
int ECTScheduler::bind_node(const Job& j, Mode mode, double now, bool* blocked, const std::vector<int>* pending) const {
  int best = -1; double bcost = kInf; bool bb = false;
  // the client is considered only when every remote window is full: the job was admitted because
  // shipping won at dispatch, and its client-side work is already paid; binding it locally is a last resort
  bool any_remote_open = false;
  for (int k = 1; k < topo_.num_nodes(); ++k) if (eligible(j, k) && window_open(k)) { any_remote_open = true; break; }
  for (int k = any_remote_open ? 1 : 0; k < topo_.num_nodes(); ++k) {
    if (!eligible(j, k)) continue;
    if (k > 0) { std::vector<Mode> ms = modes_for(j, k); if (std::find(ms.begin(), ms.end(), mode) == ms.end()) continue; }
    int q = pending ? (*pending)[static_cast<size_t>(k)] : 0;
    bool hitk = k > 0 && cache_hit_certain(j.id, k);
    double t0 = hitk ? now : predicted_slot_free(k, now, q);
    if (t0 == kInf) continue;
    double c = predicted_completion(j, k, t0, mode, true).done;
    double cost = c + (k == 0 && priced_ ? std::max(nu_, 1e-3) * client_cpu_cost(j, 0, Mode::Classic) : 0.0);
    if (cost < bcost) { bcost = cost; best = k; bb = !hitk && ((t0 > now) || !window_open(k)); }
  }
  if (blocked) *blocked = bb;
  return best;
}

// staged (preprocessed) jobs -> nodes; critical-path priority, then largest predicted compile first
// (Jackson's rule) within equal priority.
void ECTScheduler::decide_bindings(double now, std::vector<Decision>* out) {
  if (staged_.empty()) return;
  sort_by_priority(staged_, now, opt_.cp_priority, true);
  std::vector<int> remaining; remaining.reserve(staged_.size());
  std::vector<int> pending(static_cast<size_t>(topo_.num_nodes()), 0);
  size_t left = staged_.size();
  for (int jid : staged_) {
    const Job& j = wl_.jobs[static_cast<size_t>(jid)];
    Mode m = staged_mode_.count(jid) ? staged_mode_[jid] : Mode::Classic;
    bool blocked = false;
    int k = bind_node(j, m, now, &blocked, &pending);
    // work conservation: a staged job never waits (for a faster node, or locally) while a remote slot
    // idles, unless fewer jobs are queued than slots are idle (tail rule: then skipping slow slots is allowed)
    const bool waits_for_holder = k > 0 && blocked && hit_p(jid, k) >= kCertainHit;   // trusted hints wait for their holder, as under early binding
    if (topo_.params.work_conserving && (k < 0 || blocked || k == 0) && !waits_for_holder) {
      int idle = idle_remote_slots();
      if (idle > 0 && static_cast<int>(left) >= idle) {
        Mode mm = m; int kf = best_idle_remote(j, now, &mm, true);
        if (kf > 0) { k = kf; blocked = false; }
      }
    }
    // the tail is the BUILD's tail: every job still waiting (staged, admitted, ready), not only the staged
    // ones (N^pp keeps few jobs staged, so the staged count alone looks like a tail in mid-build)
    if (k > 0 && !blocked) k = core_level_i2(j, k, now, &m, true, left + ready_.size() + unbound_.size());
    --left;
    if ((k < 0 || blocked) && !waits_for_holder && client_slot_free_for_waiting(j)) { k = 0; blocked = false; }   // client work conservation
    if (k < 0) { remaining.push_back(jid); continue; }
    if (blocked) { pending[static_cast<size_t>(k)]++; remaining.push_back(jid); continue; }
    double what = w_hat(j);
    record_dispatch(j, k, now, false, m, true);
    staged_mode_.erase(jid);
    Decision d; d.job = jid; d.node = k; d.mode = k == 0 ? Mode::Classic : m; d.hedge = false; d.bind = true; d.w_hat = what;
    out->push_back(d);
  }
  staged_.swap(remaining);
}

// ready jobs -> local compile, or admission to client-side preprocessing (node chosen later), bounded by N^pp.
// Remote completion is estimated per node at its predicted slot-free time, with jobs already
// admitted/staged spread over the remote nodes as queued ahead. A job deferred because every remote window
// is full queues ahead of later jobs for its node (as under early binding); a job deferred only by the
// preprocessing cap N^pp does not: the nodes have slack then, and pushing later jobs to the client would
// spend the very resource that binds. Local is priced by nu.
void ECTScheduler::decide_admissions(double now, std::vector<Decision>* out) {
  if (ready_.empty()) return;
  sort_by_priority(ready_, now, opt_.cp_priority, false);
  std::vector<int> remaining; remaining.reserve(ready_.size());
  int unbound = static_cast<int>(unbound_.size()), staged = static_cast<int>(staged_.size());
  int remote_nodes = 0; for (int k = 1; k < topo_.num_nodes(); ++k) if (!disabled_[static_cast<size_t>(k)]) ++remote_nodes;
  std::vector<int> pending(static_cast<size_t>(topo_.num_nodes()), 0);
  int q = remote_nodes > 0 ? (unbound + staged + remote_nodes - 1) / remote_nodes : 0;
  for (int k = 1; k < topo_.num_nodes(); ++k) pending[static_cast<size_t>(k)] = q;
  auto any_remote_open = [&]() { for (int k = 1; k < topo_.num_nodes(); ++k) if (!disabled_[static_cast<size_t>(k)] && window_open(k)) return true; return false; };
  // Once the admission caps are exhausted only local starts remain possible; scanning the whole
  // priority-ordered ready list (thousands of jobs x nodes) on every event would make late binding ~10x
  // slower for no decision, so after exhaustion we look at a bounded tail (256 jobs) for local candidates.
  size_t tail_after_exhaustion = 256, exhausted_at = std::numeric_limits<size_t>::max();
  // Work conservation at admission. N^pp is sized from PREDICTED client-side time and
  // service rates; when those are wrong (e.g. hit-heavy builds whose remote jobs finish in milliseconds) the caps would
  // hold ready jobs at admission while remote slots idle. A job may therefore be admitted beyond the caps whenever the
  // idle remote slots outnumber the jobs already admitted or staged to fill them (each of those will occupy one), so
  // no remote capacity waits for an admission decision. Bounded by the idle slots: no unbounded buffering.
  const int idle_remote = topo_.params.work_conserving ? idle_remote_slots() : 0;
  auto wc_admit = [&]() { return staged + unbound < idle_remote; };
  for (size_t idx = 0; idx < ready_.size(); ++idx) {
    int jid = ready_[idx];
    bool caps_full = (unbound >= npp_ || staged + unbound >= staged_cap_ + npp_) && !wc_admit();
    if (caps_full && exhausted_at == std::numeric_limits<size_t>::max()) exhausted_at = idx;
    if (caps_full && (!window_open(0) || idx >= exhausted_at + tail_after_exhaustion)) {
      for (size_t r2 = idx; r2 < ready_.size(); ++r2) {   // barrier jobs never wait behind the caps
        int j2 = ready_[r2]; const Job& jj = wl_.jobs[static_cast<size_t>(j2)];
        if (non_distributable(jj)) { double w0 = w_hat(jj); record_dispatch(jj, 0, now, false, Mode::Classic); Decision d0; d0.job = j2; d0.node = 0; d0.w_hat = w0; out->push_back(d0); }
        else remaining.push_back(j2);
      }
      break;
    }
    const Job& j = wl_.jobs[static_cast<size_t>(jid)];
    if (non_distributable(j)) {
      double what0 = w_hat(j); record_dispatch(j, 0, now, false, Mode::Classic);
      Decision d0; d0.job = jid; d0.node = 0; d0.mode = Mode::Classic; d0.w_hat = what0; out->push_back(d0); continue;
    }
    bool blocked = false; Mode m = Mode::Classic;
    int k = best_node(j, now, -1, nullptr, &blocked, &pending, &m);
    if (k == 0 && !local_gate(j)) { blocked = false; k = best_node(j, now, 0, nullptr, &blocked, &pending, &m); }   // dominance gate
    if (k < 0) { remaining.push_back(jid); continue; }
    double what = w_hat(j);
    if (k == 0) {
      // local preferred but no local slot: it queues for the client ahead of later ready jobs, whose local
      // estimate therefore grows until shipping wins (the same deferred-queue logic as early binding)
      if (!window_open(0) || blocked) { pending[0]++; remaining.push_back(jid); continue; }
      record_dispatch(j, 0, now, false, Mode::Classic);
      Decision d; d.job = jid; d.node = 0; d.mode = Mode::Classic; d.w_hat = what; out->push_back(d);
      continue;
    }
    // N^pp bounds jobs in client-side preprocessing; the staged pool is bounded separately (one window's worth)
    if ((unbound >= npp_ || staged + unbound >= staged_cap_ + npp_) && !wc_admit()) {
      if (!any_remote_open()) pending[static_cast<size_t>(k)]++;
      remaining.push_back(jid); continue;
    }
    if (unbound >= npp_ || staged + unbound >= staged_cap_ + npp_) ++admit_wc_;   // admitted only by work conservation
    record_admit(j, now, m);
    ++unbound; pending[static_cast<size_t>(k)]++;
    Decision d; d.job = jid; d.node = kUnbound; d.mode = m; d.w_hat = what; out->push_back(d);
  }
  ready_.swap(remaining);
}

// ---- distcc emulation -----------------------------------------------------
DistccEmulation::DistccEmulation(const Topology& t, const Workload& w, std::shared_ptr<CostPredictor> pred, bool pump, bool hp)
    : Scheduler(t, w, std::move(pred)), pump_(pump), hp_(hp) {
  int n = t.num_nodes();
  slots_.assign(static_cast<size_t>(n), 0);
  slots_[0] = std::max(0, t.params.distcc_localhost_slots);   // "localhost/N" in DISTCC_HOSTS
  for (auto& nd : t.nodes) {
    size_t k = static_cast<size_t>(nd.id);
    slots_[k] = (k - 1 < t.params.host_slots.size()) ? t.params.host_slots[k - 1] : nd.cores;
  }
  window_ = slots_;
}

double DistccEmulation::rho() const {
  if (hp_) return Scheduler::rho();
  return topo_.params.distcc_compression == "lzo" ? topo_.params.rho_lzo : 1.0;
}

std::vector<Decision> DistccEmulation::decide(double now) {
  std::vector<Decision> out;
  std::vector<int> remaining;
  prune(now);
  int max_slots = *std::max_element(slots_.begin(), slots_.end());
  for (int jid : ready_) {
    const Job& j = wl_.jobs[static_cast<size_t>(jid)];
    int chosen = -1;
    if (non_distributable(j)) {
      chosen = 0;   // make runs these itself; distcc's slot files are not involved
    } else {
      // distcc: for slot index i, try each remote host in order, then localhost
      for (int i = 0; i < max_slots && chosen < 0; ++i) {
        for (auto& nd : topo_.nodes) {
          size_t k = static_cast<size_t>(nd.id);
          if (!disabled_[k] && i < slots_[k] && inflight_[k] == i) { chosen = nd.id; break; }
        }
        if (chosen < 0 && i < slots_[0] && inflight_[0] == i) chosen = 0;
      }
      // fallback: any host with a free slot (inflight below its limit)
      if (chosen < 0) {
        for (auto& nd : topo_.nodes) { size_t k = static_cast<size_t>(nd.id); if (!disabled_[k] && inflight_[k] < slots_[k]) { chosen = nd.id; break; } }
        if (chosen < 0 && inflight_[0] < slots_[0]) chosen = 0;
      }
    }
    if (chosen < 0) { remaining.push_back(jid); continue; }
    double what = w_hat(j);
    Mode mode = (pump_ && chosen > 0 && j.src_size_bytes > 0) ? Mode::HeaderPush : Mode::Classic;
    if (hp_ && chosen > 0) {   // opticc's mode choice: header-push wherever this policy allows it
      std::vector<Mode> ms = modes_for(j, chosen);
      mode = std::find(ms.begin(), ms.end(), Mode::HeaderPush) != ms.end() ? Mode::HeaderPush : Mode::Classic;
    }
    record_dispatch(j, chosen, now, false, mode);
    out.push_back(Decision{jid, chosen, mode, false, false, what});
  }
  ready_.swap(remaining);
  return out;
}

// ---- fixed assignment -----------------------------------------------------
FixedAssignmentScheduler::FixedAssignmentScheduler(const Topology& t, const Workload& w, std::vector<int> assignment)
    : Scheduler(t, w, std::make_shared<OraclePredictor>()), assign_(std::move(assignment)) {
  for (auto& x : window_) x = std::numeric_limits<int>::max();
}

std::vector<Decision> FixedAssignmentScheduler::decide(double now) {
  std::vector<Decision> out;
  prune(now);
  for (int jid : ready_) {
    int k = assign_.at(static_cast<size_t>(jid));
    const Job& j = wl_.jobs[static_cast<size_t>(jid)];
    double what = w_hat(j);
    record_dispatch(j, k, now, false);
    out.push_back(Decision{jid, k, Mode::Classic, false, false, what});
  }
  ready_.clear();
  return out;
}

std::unique_ptr<Scheduler> make_scheduler(const std::string& policy, const Topology& t,
                                          const Workload& w, std::shared_ptr<CostPredictor> pred) {
  if (policy == "distcc") return std::make_unique<DistccEmulation>(t, w, std::move(pred));
  if (policy == "distcc-pump") return std::make_unique<DistccEmulation>(t, w, std::move(pred), true);
  if (policy == "distcc-hp") return std::make_unique<DistccEmulation>(t, w, std::move(pred), false, true);
  if (policy == "ect") return std::make_unique<ECTScheduler>(t, w, std::move(pred), ECTOptions{false, false, HedgeRule::None});
  if (policy == "ect-window") return std::make_unique<ECTScheduler>(t, w, std::move(pred), ECTOptions{true, true, HedgeRule::None});
  if (policy == "ect-window-hedge") return std::make_unique<ECTScheduler>(t, w, std::move(pred), ECTOptions{true, true, HedgeRule::Kappa});
  if (policy == "ect-window-hedge-t7") return std::make_unique<ECTScheduler>(t, w, std::move(pred), ECTOptions{true, true, HedgeRule::T7});
  throw std::runtime_error("unknown policy: " + policy);
}

}  // namespace opticc
