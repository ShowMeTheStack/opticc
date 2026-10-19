// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/sim.hpp"
#include "opticc/speed.hpp"
#include "opticc/predictor.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace opticc::sim {

static constexpr double kInf = std::numeric_limits<double>::infinity();

json Result::to_json() const {
  json nodes_j = json::array();
  for (auto& n : nodes)
    nodes_j.push_back({{"id", n.id}, {"name", n.name}, {"capacity", n.capacity}, {"busy_core_s", n.busy_core_s},
                       {"bytes_up", n.bytes_up}, {"bytes_down", n.bytes_down}, {"jobs_completed", n.jobs_completed},
                       {"attempts", n.attempts}, {"utilisation", n.utilisation}});
  return json{{"policy", policy}, {"makespan_s", makespan}, {"nodes", nodes_j},
              {"job_ready_time", job_ready_time}, {"job_dispatch_time", job_dispatch_time},
              {"job_done_time", job_done_time}, {"job_latency", job_latency}, {"job_node", job_node},
              {"total_work_ref_s", total_work}, {"duplicated_work_ref_s", duplicated_work},
              {"duplicated_work_frac", duplicated_work_frac}, {"hedges", hedges}, {"hedges_won", hedges_won},
              {"stragglers_injected", stragglers_injected}, {"max_inflight_node", max_inflight_node},
              {"inflight_time_integral", inflight_time_integral}, {"client_preprocess_s", client_preprocess_s},
              {"bytes_up_total", bytes_up_total}, {"bytes_down_total", bytes_down_total},
              {"remote_jobs", remote_jobs}, {"local_jobs", local_jobs}, {"header_push_jobs", header_push_jobs},
              {"header_miss_bytes", header_miss_bytes}, {"prefetch_bytes", prefetch_bytes}, {"need_rounds", need_rounds}, {"client_scan_s", client_scan_s}, {"cache_hits", cache_hits}, {"client_overhead_s", client_overhead_s},
              {"release_window", release_window}, {"nu_final", nu_final}, {"nu_peak", nu_peak}, {"nu_updates", nu_updates},
              {"nu_direction_changes", nu_direction_changes}, {"lower_bounds", lb.to_json()},
              {"efficiency", efficiency}, {"cost_usd", cost_usd}, {"windows", windows}};
}

enum class Phase { WaitClientCore, Preprocess, Staged, Latency, Upload, NeedRound, WaitNodeCore, Compile, CacheHit, Download,
                   LocalWaitCore, LocalCompile, Done, Cancelled };
// Staged: late binding; client-side work done, the scheduler has not yet chosen the node.

struct Attempt {
  int id = 0, job = 0, node = 0;
  bool hedge = false;
  double w_hat = 0;          // predicted work at dispatch (uplink ordering)
  Mode mode = Mode::Classic;
  Phase phase = Phase::WaitClientCore;
  double t_dispatch = 0, t_phase_start = 0;
  uint64_t timer_seq = 0;
  int flow = -1;
  bool holds_client_core = false, holds_node_core = false;
  bool cache_hit = false;    // served from the node's compile cache (no core, no compile)
  double miss = 0;           // raw header bytes this attempt shipped on store misses
};

struct Flow {
  int id = 0, attempt = 0;
  double total = 0, remaining = 0;   // bytes
  int link_a = -1, link_b = -1;  // node link, client link (-1 = none)
  double rate = 0;        // bytes/s
  bool active = true;
  bool up = true;         // direction, for byte accounting
  bool ended = false;
};

struct Timer {
  double t; uint64_t seq; int attempt;
  bool operator>(const Timer& o) const { return t > o.t || (t == o.t && seq > o.seq); }
};

struct Simulator::Impl {
  const Topology& topo; const Workload& wl; Scheduler& sched; Options opt;
  double now = 0;
  uint64_t seq = 0;
  std::vector<Attempt> attempts;
  std::vector<Flow> flows;
  std::vector<int> active_flows;
  std::vector<std::deque<int>> link_fifo;      // FIFO queue per node link (pipelined transport)
  bool fifo = true;
  bool jackson = false;
  std::mt19937_64 rng;
  std::vector<int> node_inflight;              // attempts dispatched & not finished per node
  std::vector<std::unordered_set<int>> store;  // header-push content store per node (header ids present)
  // prefetch state
  std::vector<double> warm_recv;               // raw header bytes each node received through job misses
  std::vector<double> pf_rate, pf_end;         // background push rate (raw B/s) and finish time per node
  std::vector<double> pf_restore_cap;          // uplink capacity to restore when the background push ends
  std::vector<int> pop_rank;                   // header id -> popularity rank (0 = most referenced)
  std::vector<double> pop_prefix;              // cumulative bytes of headers by rank
  std::priority_queue<Timer, std::vector<Timer>, std::greater<Timer>> timers;
  // links: 0 egress, 1 ingress, 2+2(k-1) up_k, 3+2(k-1) down_k ; capacity in bytes/s (inf = unlimited)
  std::vector<double> link_cap;
  // core pools
  std::vector<int> free_cores;                 // index = node id
  std::vector<std::deque<int>> wait_core;      // attempts waiting for a core, index = node id
  // jobs
  std::vector<int> remaining_deps;
  std::vector<char> job_done, job_dispatched;
  std::vector<std::vector<int>> job_attempts;
  std::vector<int> staged_attempt;             // job -> attempt id awaiting binding (-1 = none)
  std::vector<std::vector<int>> succ;
  int jobs_left = 0;
  int inflight_jobs = 0;
  // release model: jobs whose dependencies are met wait here until make's -j window admits them
  std::deque<int> release_queue;
  int release_window = 0, released_outstanding = 0;
  Result res;

  Impl(const Topology& t, const Workload& w, Scheduler& s, Options o) : topo(t), wl(w), sched(s), opt(o), rng(o.seed) {}

  static double cap_of(double bps) { return bps <= 0 ? kInf : bps / 8.0; }
  // A flow is finished when fewer than eps bytes remain or when the time to
  // drain it is below 1 ns (below double resolution of `now`, would stall).
  bool flow_done(const Flow& f) const {
    return f.remaining <= opt.eps_bytes || f.rate == kInf || (f.rate > 0 && f.remaining / f.rate <= 1e-9);
  }
  int up_link(int k) const { return 2 + 2 * (k - 1); }
  int down_link(int k) const { return 3 + 2 * (k - 1); }

  void init() {
    topo.check_ids();
    int n = topo.num_nodes();
    double mean_m = wl.mean_mem();
    node_inflight.assign(static_cast<size_t>(n), 0);
    store.assign(static_cast<size_t>(n), {});
    warm_recv.assign(static_cast<size_t>(n), 0.0);
    free_cores.resize(static_cast<size_t>(n));
    wait_core.resize(static_cast<size_t>(n));
    res.nodes.resize(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
      free_cores[static_cast<size_t>(k)] = topo.capacity(k, mean_m);
      res.nodes[static_cast<size_t>(k)].id = k;
      res.nodes[static_cast<size_t>(k)].capacity = free_cores[static_cast<size_t>(k)];
      res.nodes[static_cast<size_t>(k)].name = k == 0 ? "client" : topo.node(k).name;
    }
    link_cap.assign(static_cast<size_t>(2 + 2 * topo.nodes.size()), kInf);
    link_fifo.resize(link_cap.size());
    fifo = opt.transport == Transport::Fifo || (opt.transport == Transport::Auto && sched.pipelined_transport());
    jackson = fifo && (opt.uplink_order == UplinkOrder::Jackson || (opt.uplink_order == UplinkOrder::Auto && sched.pipelined_transport()));
    link_cap[0] = cap_of(topo.client.egress_bps);
    link_cap[1] = cap_of(topo.client.ingress_bps);
    for (auto& nd : topo.nodes) {
      link_cap[static_cast<size_t>(up_link(nd.id))] = cap_of(nd.bw_up_bps);
      link_cap[static_cast<size_t>(down_link(nd.id))] = cap_of(nd.bw_down_bps);
    }
    init_prefetch();
    size_t J = wl.jobs.size();
    remaining_deps.resize(J); job_done.assign(J, 0); job_dispatched.assign(J, 0); job_attempts.resize(J); staged_attempt.assign(J, -1);
    res.job_ready_time.assign(J, -1); res.job_dispatch_time.assign(J, -1); res.job_done_time.assign(J, -1);
    res.job_latency.assign(J, -1); res.job_node.assign(J, -1);
    succ = wl.successors();
    jobs_left = static_cast<int>(J);
    release_window = opt.release_window >= 0 ? opt.release_window : std::max(0, wl.release_window);
    res.release_window = release_window;
    for (size_t jj = 0; jj < opt.cached_on.size() && jj < J; ++jj)
      if (opt.cached_on[jj] > 0) sched.set_affinity(static_cast<int>(jj), {opt.cached_on[jj]}, opt.cache_hit_cost_s);
    for (size_t jj = 0; jj < opt.believed_on.size() && jj < J; ++jj)
      if (opt.believed_on[jj] > 0) sched.set_affinity(static_cast<int>(jj), {opt.believed_on[jj]}, opt.cache_hit_cost_s, opt.believed_p);
    for (auto& j : wl.jobs) {
      remaining_deps[static_cast<size_t>(j.id)] = static_cast<int>(j.deps.size());
      if (j.deps.empty()) release_queue.push_back(j.id);
    }
    try_release();
    res.total_work = wl.total_work();
    res.policy = sched.name();
  }
  // make -jN: hand jobs to the scheduler in trace order while fewer than N released jobs are unfinished.
  void try_release() {
    while (!release_queue.empty() && (release_window <= 0 || released_outstanding < release_window)) {
      int jid = release_queue.front(); release_queue.pop_front();
      ++released_outstanding;
      res.job_ready_time[static_cast<size_t>(jid)] = now;
      sched.on_job_ready(jid, now);
    }
  }
  // client CPU (reference seconds) of an attempt's client-side phase: shipping cost + encryption + per-job overhead
  // (the overhead is paid once per job, by the primary attempt only)
  double client_phase_cpu(const Attempt& a) const {
    const Job& j = wl.jobs[static_cast<size_t>(a.job)];
    double ship = a.node == 0 ? 0.0 : sched.ship_client_cpu(j, a.mode) + enc_cpu_s(ship_bytes_of(a) + j.obj_size_bytes, topo.params);
    return ship + (a.hedge ? 0.0 : sched.overhead());
  }

  // ---- core pools -------------------------------------------------------
  void request_core(int k, int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    if (free_cores[static_cast<size_t>(k)] > 0) { free_cores[static_cast<size_t>(k)]--; grant_core(k, aid); }
    else { wait_core[static_cast<size_t>(k)].push_back(aid); (void)a; }
  }
  void release_core(int k) {
    auto& q = wait_core[static_cast<size_t>(k)];
    while (!q.empty()) {
      int aid = q.front(); q.pop_front();
      if (attempts[static_cast<size_t>(aid)].phase == Phase::Cancelled) continue;
      grant_core(k, aid);
      return;
    }
    free_cores[static_cast<size_t>(k)]++;
    sched.on_slot_free(k, now);
  }
  void set_timer(int aid, double dt) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    a.timer_seq = ++seq;
    timers.push(Timer{now + dt, a.timer_seq, aid});
  }
  void grant_core(int k, int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    const Job& j = wl.jobs[static_cast<size_t>(a.job)];
    a.t_phase_start = now;
    if (k == 0 && a.phase == Phase::WaitClientCore) {
      // classic: preprocess (p_j); header-push: closure lookup + hashing (q_j); plus the per-job overhead
      a.holds_client_core = true; a.phase = Phase::Preprocess;
      set_timer(aid, client_phase_cpu(a) / topo.client.speed);
    } else if (k == 0) {  // LocalWaitCore
      a.holds_client_core = true; a.phase = Phase::LocalCompile;
      set_timer(aid, (j.work_ref_cpu_s + (a.hedge ? 0.0 : sched.overhead())) / topo.client.speed);
    } else {
      a.holds_node_core = true; a.phase = Phase::Compile;
      // w_j is from source; header-push w_j / f_k (the worker preprocesses), classic (w_j - p_j) / f_k
      double dur = (remote_compile_work(j.work_ref_cpu_s, j.preprocess_cpu_s, a.mode)
                    + enc_cpu_s(ship_bytes_of(a) + j.obj_size_bytes, topo.params)) / topo.node(k).speed;
      if (opt.straggler_prob > 0 && straggler_draw(a.job, aid) < opt.straggler_prob) {
        dur *= opt.straggler_factor; res.stragglers_injected++;
      }
      set_timer(aid, dur);
    }
  }

  // Straggler draw per (job, attempt index): the same realisation for every
  // policy and seed-independent of dispatch order.
  double straggler_draw(int job, int aid) {
    int idx = 0;
    for (int x : job_attempts[static_cast<size_t>(job)]) { if (x == aid) break; ++idx; }
    std::seed_seq ss{static_cast<uint32_t>(opt.seed), static_cast<uint32_t>(opt.seed >> 32), static_cast<uint32_t>(job), static_cast<uint32_t>(idx)};
    std::mt19937_64 eng(ss);
    return std::uniform_real_distribution<double>(0.0, 1.0)(eng);
  }

  // ---- flows ------------------------------------------------------------
  void account_bytes(const Flow& f, double bytes) {
    int node = attempts[static_cast<size_t>(f.attempt)].node;
    if (f.up) { res.nodes[static_cast<size_t>(node)].bytes_up += bytes; res.bytes_up_total += bytes; }
    else { res.nodes[static_cast<size_t>(node)].bytes_down += bytes; res.bytes_down_total += bytes; }
  }
  void start_flow(int aid, double bytes, int link_a, int link_b, bool up) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    if (bytes <= 0) { on_flow_done(aid); return; }
    Flow f; f.id = static_cast<int>(flows.size()); f.attempt = aid; f.total = f.remaining = bytes; f.link_a = link_a; f.link_b = link_b;
    f.active = false; f.up = up;
    flows.push_back(f); a.flow = f.id;
    if (fifo && link_a >= 2) {
      auto& q = link_fifo[static_cast<size_t>(link_a)];
      q.push_back(f.id);
      if (q.size() == 1) { flows.back().active = true; active_flows.push_back(f.id); }
    } else {
      flows.back().active = true; active_flows.push_back(f.id);
    }
  }
  // Remove a flow from the link; charges the bytes actually moved.
  void end_flow(int fid) {
    Flow& f = flows[static_cast<size_t>(fid)];
    if (f.ended) return;
    f.ended = true;
    account_bytes(f, f.total - std::max(0.0, f.remaining));
    if (f.active) { f.active = false; active_flows.erase(std::remove(active_flows.begin(), active_flows.end(), fid), active_flows.end()); }
    if (fifo && f.link_a >= 2) {
      auto& q = link_fifo[static_cast<size_t>(f.link_a)];
      bool was_head = !q.empty() && q.front() == fid;
      q.erase(std::remove(q.begin(), q.end(), fid), q.end());
      if (was_head && !q.empty()) {
        // next flow on this link: FIFO, or the largest predicted compile first (Jackson's rule)
        auto it = q.begin();
        if (jackson && f.up) {
          it = std::max_element(q.begin(), q.end(), [&](int x, int y) {
            return attempts[static_cast<size_t>(flows[static_cast<size_t>(x)].attempt)].w_hat <
                   attempts[static_cast<size_t>(flows[static_cast<size_t>(y)].attempt)].w_hat; });
          std::iter_swap(q.begin(), it); it = q.begin();
        }
        Flow& nf = flows[static_cast<size_t>(*it)]; nf.active = true; active_flows.push_back(nf.id);
      }
    }
  }
  // Max-min fair progressive filling over the two-link constraint sets.
  void recompute_rates() {
    size_t L = link_cap.size();
    std::vector<double> rem(link_cap);
    std::vector<int> cnt(L, 0);
    std::vector<char> frozen(active_flows.size(), 0);
    for (int fid : active_flows) { Flow& f = flows[static_cast<size_t>(fid)]; f.rate = 0; }
    size_t unfrozen = active_flows.size();
    while (unfrozen > 0) {
      std::fill(cnt.begin(), cnt.end(), 0);
      for (size_t i = 0; i < active_flows.size(); ++i) {
        if (frozen[i]) continue;
        Flow& f = flows[static_cast<size_t>(active_flows[i])];
        if (f.link_a >= 0) cnt[static_cast<size_t>(f.link_a)]++;
        if (f.link_b >= 0) cnt[static_cast<size_t>(f.link_b)]++;
      }
      double delta = kInf;
      for (size_t l = 0; l < L; ++l) if (cnt[l] > 0 && rem[l] < kInf) delta = std::min(delta, rem[l] / cnt[l]);
      if (delta == kInf) {  // all remaining flows unconstrained
        for (size_t i = 0; i < active_flows.size(); ++i) if (!frozen[i]) flows[static_cast<size_t>(active_flows[i])].rate = kInf;
        break;
      }
      for (size_t i = 0; i < active_flows.size(); ++i) {
        if (frozen[i]) continue;
        Flow& f = flows[static_cast<size_t>(active_flows[i])];
        f.rate += delta;
        if (f.link_a >= 0) rem[static_cast<size_t>(f.link_a)] -= delta;
        if (f.link_b >= 0) rem[static_cast<size_t>(f.link_b)] -= delta;
      }
      for (size_t i = 0; i < active_flows.size(); ++i) {
        if (frozen[i]) continue;
        Flow& f = flows[static_cast<size_t>(active_flows[i])];
        // an unlimited link is never saturated
        auto saturated = [&](int l) {
          return l >= 0 && link_cap[static_cast<size_t>(l)] < kInf && rem[static_cast<size_t>(l)] <= 1e-9 * link_cap[static_cast<size_t>(l)];
        };
        bool sat = saturated(f.link_a) || saturated(f.link_b);
        if (sat) { frozen[i] = 1; --unfrozen; }
      }
    }
  }

  // ---- attempt lifecycle ------------------------------------------------
  void dispatch(const Decision& d) {
    if (d.bind) {  // late binding: the staged attempt gets its node and ships
      int aid = staged_attempt[static_cast<size_t>(d.job)];
      if (aid < 0) throw std::runtime_error("bind decision for a job that is not staged");
      staged_attempt[static_cast<size_t>(d.job)] = -1;
      Attempt& a = attempts[static_cast<size_t>(aid)];
      if (a.phase != Phase::Staged) throw std::runtime_error("bind decision for an attempt not in Staged");
      a.node = d.node; a.w_hat = d.w_hat;
      res.nodes[static_cast<size_t>(d.node)].attempts++;
      node_inflight[static_cast<size_t>(d.node)]++;
      if (d.node > 0) res.max_inflight_node = std::max(res.max_inflight_node, node_inflight[static_cast<size_t>(d.node)]);
      if (d.node == 0) { a.phase = Phase::LocalWaitCore; a.mode = Mode::Classic; request_core(0, aid); return; }
      after_preprocess(aid);
      return;
    }
    Attempt a; a.id = static_cast<int>(attempts.size()); a.job = d.job; a.node = d.node; a.hedge = d.hedge; a.mode = d.mode;
    a.t_dispatch = now; a.w_hat = d.w_hat;
    attempts.push_back(a);
    job_attempts[static_cast<size_t>(d.job)].push_back(a.id);
    if (d.node >= 0) {
      res.nodes[static_cast<size_t>(d.node)].attempts++;
      node_inflight[static_cast<size_t>(d.node)]++;
      if (d.node > 0) res.max_inflight_node = std::max(res.max_inflight_node, node_inflight[static_cast<size_t>(d.node)]);
    }
    if (d.hedge) res.hedges++;
    if (!job_dispatched[static_cast<size_t>(d.job)]) {
      job_dispatched[static_cast<size_t>(d.job)] = 1; inflight_jobs++;
      res.job_dispatch_time[static_cast<size_t>(d.job)] = now;
    }
    if (d.node == 0) { attempts.back().phase = Phase::LocalWaitCore; request_core(0, a.id); return; }
    if (client_phase_cpu(attempts.back()) > 0) { attempts.back().phase = Phase::WaitClientCore; request_core(0, a.id); }
    else if (d.node == kUnbound) stage(a.id);
    else { attempts.back().phase = Phase::Latency; after_preprocess(a.id); }
  }
  // Late binding: client-side work done, hand the job back to the scheduler for node choice.
  void stage(int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    a.phase = Phase::Staged; a.t_phase_start = now;
    staged_attempt[static_cast<size_t>(a.job)] = aid;
    sched.on_preprocessed(a.job, now);
  }
  // Bytes an attempt is expected to ship (the scheduler's estimate; used for the encryption CPU charge,
  // which must be known before the upload reveals the exact store misses). Unbound attempts use the
  // node-independent estimate.
  double ship_bytes_of(const Attempt& a) const {
    if (a.node == 0) return 0.0;
    const Job& j = wl.jobs[static_cast<size_t>(a.job)];
    if (a.node < 0) return sched.ship_bytes(j, a.mode);
    return sched.ship_bytes(j, a.mode, a.node);
  }
  void after_preprocess(int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    a.phase = Phase::Latency; a.t_phase_start = now;
    double lat = sched.alpha() * topo.node(a.node).rtt_s;
    if (lat > 0) set_timer(aid, lat); else begin_upload(aid);
  }
  void begin_upload(int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    const Job& j = wl.jobs[static_cast<size_t>(a.job)];
    a.phase = Phase::Upload; a.t_phase_start = now;
    double bytes;
    a.cache_hit = a.node > 0 && static_cast<size_t>(a.job) < opt.cached_on.size() && opt.cached_on[static_cast<size_t>(a.job)] == a.node;
    if (a.cache_hit && a.mode != Mode::Classic) bytes = sched.rho() * j.src_size_bytes;   // key lookup needs the source only
    else if (a.mode == Mode::Classic) bytes = sched.ship_bytes(j, a.mode);
    else {
      // header-push: source plus the headers this node's store lacks (added on upload completion);
      // a policy without a per-node store (distcc-pump) ships the whole closure every time
      double miss = sched.per_node_store() ? store_misses(a.node, j) : j.closure_bytes;
      bytes = shipped_bytes(j, Mode::HeaderPush, topo.params, sched.rho(), miss);
      res.header_miss_bytes += miss;
      a.miss = miss;
      sched.on_shipped(a.job, a.node, a.mode, bytes);
    }
    start_flow(aid, bytes, up_link(a.node), 0, true);
  }
  void init_prefetch() {
    size_t n = static_cast<size_t>(topo.num_nodes());
    pf_rate.assign(n, 0.0); pf_end.assign(n, 0.0); pf_restore_cap.assign(n, 0.0);
    if (opt.prefetch != 1) return;
    // popularity ranking of the trace's headers (the warm-start list the previous build saved)
    if (!wl.header_sizes.empty()) {
      std::vector<std::pair<int, int>> cnt(wl.header_sizes.size(), {0, 0});
      for (size_t h = 0; h < cnt.size(); ++h) cnt[h].second = static_cast<int>(h);
      for (auto& j : wl.jobs) for (int h : j.headers) cnt[static_cast<size_t>(h)].first++;
      std::sort(cnt.begin(), cnt.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
      pop_rank.assign(cnt.size(), 0); pop_prefix.assign(cnt.size(), 0.0);
      double acc = 0;
      for (size_t r = 0; r < cnt.size(); ++r) {
        if (cnt[r].first == 0) { pop_rank[static_cast<size_t>(cnt[r].second)] = static_cast<int>(cnt.size()); continue; }   // never referenced: never pushed
        pop_rank[static_cast<size_t>(cnt[r].second)] = static_cast<int>(r);
        acc += wl.header_sizes[static_cast<size_t>(cnt[r].second)]; pop_prefix[r] = acc;
      }
    }
    double budget = opt.prefetch_budget_bytes;
    for (auto& nd : topo.nodes) {
      size_t k = static_cast<size_t>(nd.id); int ul = up_link(nd.id);
      double cap = link_cap[static_cast<size_t>(ul)];
      if (cap >= kInf || budget <= 0) { pf_rate[k] = kInf; pf_end[k] = 0; continue; }
      pf_rate[k] = opt.prefetch_frac * cap / std::max(sched.rho(), 1e-6);    // raw header bytes per second
      pf_end[k] = budget / pf_rate[k];
      pf_restore_cap[k] = cap;
      link_cap[static_cast<size_t>(ul)] = (1.0 - opt.prefetch_frac) * cap;   // background share of the uplink
      timers.push(Timer{pf_end[k], ++seq, -nd.id});                          // node event: restore the uplink
    }
  }
  double pf_pushed(int k) const {   // raw header bytes the background push has delivered to node k by `now`
    if (opt.prefetch != 1) return 0.0;
    size_t i = static_cast<size_t>(k);
    if (pf_rate[i] >= kInf) return opt.prefetch_budget_bytes;
    return std::min(opt.prefetch_budget_bytes, pf_rate[i] * now);
  }
  double store_misses(int k, const Job& j) const {
    if (j.headers.empty()) {
      if (opt.prefetch < 0) return j.closure_bytes;   // legacy: cold-store volume every time
      double U = std::max(opt.header_union_bytes, 1.0);
      double W = std::min(U, warm_recv[static_cast<size_t>(k)] + pf_pushed(k));
      return j.closure_bytes * std::max(opt.prefetch_resid, 1.0 - W / U);
    }
    double miss = 0;
    auto& st = store[static_cast<size_t>(k)];
    double pushed = pf_pushed(k);
    for (int h : j.headers) {
      if (st.count(h)) continue;
      if (opt.prefetch == 1 && !pop_prefix.empty()) {
        int r = pop_rank.at(static_cast<size_t>(h));
        if (r < static_cast<int>(pop_prefix.size()) && pop_prefix[static_cast<size_t>(r)] <= pushed) continue;   // already pushed
      }
      miss += wl.header_sizes.at(static_cast<size_t>(h));
    }
    return miss;
  }
  void store_insert(int k, const Job& j) {
    auto& st = store[static_cast<size_t>(k)];
    for (int h : j.headers) st.insert(h);
  }
  void on_timer(int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    const Job& j = wl.jobs[static_cast<size_t>(a.job)];
    switch (a.phase) {
      case Phase::Preprocess: {
        double dt = now - a.t_phase_start;
        res.nodes[0].busy_core_s += dt;
        double ovh = (a.hedge ? 0.0 : sched.overhead()) / topo.client.speed;
        res.client_overhead_s += std::min(ovh, dt); dt = std::max(0.0, dt - ovh);
        if (a.mode == Mode::Classic) res.client_preprocess_s += dt; else res.client_scan_s += dt;
        a.holds_client_core = false; release_core(0);
        if (a.node < 0) stage(aid); else after_preprocess(aid);
        break;
      }
      case Phase::Latency: begin_upload(aid); break;
      case Phase::NeedRound: a.phase = Phase::WaitNodeCore; a.t_phase_start = now; request_core(a.node, aid); break;
      case Phase::CacheHit:
        a.phase = Phase::Download; a.t_phase_start = now;
        start_flow(aid, j.obj_size_bytes, down_link(a.node), 1, false);
        break;
      case Phase::Compile: {
        res.nodes[static_cast<size_t>(a.node)].busy_core_s += now - a.t_phase_start;
        a.holds_node_core = false; release_core(a.node);
        a.phase = Phase::Download; a.t_phase_start = now;
        start_flow(aid, j.obj_size_bytes, down_link(a.node), 1, false);
        break;
      }
      case Phase::LocalCompile: {
        res.nodes[0].busy_core_s += now - a.t_phase_start;
        if (!a.hedge) res.client_overhead_s += sched.overhead() / topo.client.speed;
        a.holds_client_core = false; release_core(0);
        finish(aid); break;
      }
      default: throw std::runtime_error("timer in unexpected phase");
    }
  }
  void on_flow_done(int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    if (a.phase == Phase::Cancelled || a.phase == Phase::Done) return;  // raced with a sibling's finish
    a.flow = -1;
    if (a.phase == Phase::Upload && a.cache_hit) {   // lookup on the worker, then the object comes back
      a.phase = Phase::CacheHit; a.t_phase_start = now; set_timer(aid, opt.cache_hit_cost_s);
    }
    else if (a.phase == Phase::Upload) {
      if (a.mode != Mode::Classic && sched.per_node_store()) {
        store_insert(a.node, wl.jobs[static_cast<size_t>(a.job)]);
        warm_recv[static_cast<size_t>(a.node)] += a.miss;
      }
      // without prefetch the worker discovers the misses after the source arrives and asks for them (NEED):
      // one extra round trip before the compile. With prefetch the misses travel with the job.
      if (opt.prefetch == 0 && a.mode != Mode::Classic && a.miss > 0 && sched.per_node_store()) {
        a.phase = Phase::NeedRound; a.t_phase_start = now; res.need_rounds++;
        set_timer(aid, topo.node(a.node).rtt_s);
      } else { a.phase = Phase::WaitNodeCore; a.t_phase_start = now; request_core(a.node, aid); }
    }
    else if (a.phase == Phase::Download) finish(aid);
    else throw std::runtime_error("flow done in unexpected phase");
  }
  // Reference-CPU seconds a losing attempt consumed: its preprocessing (spent
  // in full once it left the Preprocess phase) plus any compile time.
  double consumed_work(const Attempt& a) const {
    const Job& j = wl.jobs[static_cast<size_t>(a.job)];
    double pp_full = a.node != 0 ? client_phase_cpu(a) : 0.0;
    switch (a.phase) {
      case Phase::WaitClientCore: return 0;
      case Phase::Preprocess: return (now - a.t_phase_start) * topo.client.speed;
      case Phase::Staged: case Phase::Latency: case Phase::Upload: case Phase::WaitNodeCore: case Phase::CacheHit: return pp_full;
      case Phase::Compile: return pp_full + (now - a.t_phase_start) * topo.node(a.node).speed;
      case Phase::LocalWaitCore: return 0;
      case Phase::LocalCompile: return (now - a.t_phase_start) * topo.client.speed;
      case Phase::Download: case Phase::Done: return pp_full + (a.cache_hit ? 0.0 : j.work_ref_cpu_s);
      default: return 0;
    }
  }
  void cancel(int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    if (a.phase == Phase::Done || a.phase == Phase::Cancelled) return;
    res.duplicated_work += consumed_work(a);
    if (a.phase == Phase::Compile) res.nodes[static_cast<size_t>(a.node)].busy_core_s += now - a.t_phase_start;
    if (a.phase == Phase::LocalCompile || a.phase == Phase::Preprocess) res.nodes[0].busy_core_s += now - a.t_phase_start;
    if (a.flow >= 0) { end_flow(a.flow); a.flow = -1; }
    a.timer_seq = ++seq;  // invalidate pending timer
    bool hc = a.holds_client_core, hn = a.holds_node_core;
    a.holds_client_core = a.holds_node_core = false;
    a.phase = Phase::Cancelled;
    if (hc) release_core(0);
    if (hn) release_core(a.node);
  }
  void finish(int aid) {
    Attempt& a = attempts[static_cast<size_t>(aid)];
    a.phase = Phase::Done;
    size_t jid = static_cast<size_t>(a.job);
    if (job_done[jid]) return;
    job_done[jid] = 1; jobs_left--; inflight_jobs--;
    if (a.hedge) res.hedges_won++;
    for (int x : job_attempts[jid]) { int nd = attempts[static_cast<size_t>(x)].node; if (nd >= 0) node_inflight[static_cast<size_t>(nd)]--; }
    res.job_done_time[jid] = now;
    res.job_latency[jid] = now - res.job_ready_time[jid];
    res.job_node[jid] = a.node;
    res.nodes[static_cast<size_t>(a.node)].jobs_completed++;
    if (a.node == 0) res.local_jobs++; else { res.remote_jobs++; if (a.mode != Mode::Classic) res.header_push_jobs++; if (a.cache_hit) res.cache_hits++; }
    for (int other : job_attempts[jid]) if (other != aid) cancel(other);
    double measured_w = wl.jobs[jid].work_ref_cpu_s;
    if (opt.believed && opt.speed_est && opt.predictor && a.node > 0 && !a.cache_hit) {
      double cpu = wl.jobs[jid].work_ref_cpu_s / topo.node(a.node).speed;           // true CPU on the worker
      opt.speed_est->observe(a.node, opt.predictor->predict(wl.jobs[jid]), cpu);
      for (auto& nd : opt.believed->nodes) nd.speed = opt.speed_est->speed(nd.id);
      measured_w = cpu * opt.believed->node(a.node).speed;
      opt.predictor->observe(wl.jobs[jid], measured_w);
      if (opt.speed_est->updates() % 16 == 0) sched.refresh(now);
    }
    sched.on_job_done(a.job, a.node, now, measured_w);
    --released_outstanding;
    for (int v : succ[jid]) if (--remaining_deps[static_cast<size_t>(v)] == 0) release_queue.push_back(v);
    try_release();
  }

  // ---- main loop --------------------------------------------------------
  Result run() {
    init();
    uint64_t events = 0, zero_dt = 0;
    while (jobs_left > 0) {
      // Decide until quiescent: a dispatch may stage a job synchronously (late binding with no
      // client-side work) or bind one, which can enable further decisions at the same instant.
      for (int round = 0; round < 1000; ++round) {
        std::vector<Decision> ds = sched.decide(now);
        if (ds.empty()) break;
        for (const Decision& d : ds) dispatch(d);
      }
      recompute_rates();
      double tnext = timers.empty() ? kInf : timers.top().t;
      // scheduler wake-ups (hedging checks): strictly in the future by contract
      double wake = sched.next_wakeup(now);
      if (wake > now) tnext = std::min(tnext, wake);
      for (int fid : active_flows) {
        Flow& f = flows[static_cast<size_t>(fid)];
        if (flow_done(f)) tnext = now;
        else if (f.rate > 0) tnext = std::min(tnext, now + f.remaining / f.rate);
      }
      if (tnext == kInf) {
        // nothing can happen: scheduler must have refused to dispatch
        throw std::runtime_error("simulation stalled at t=" + std::to_string(now) + " with " +
                                 std::to_string(jobs_left) + " jobs left, ready=" + std::to_string(sched.ready_count()) + ", staged=" + std::to_string(sched.staged_count()));
      }
      // watchdog: a livelock shows up as unbounded zero-advance iterations
      if (++events > opt.max_events) throw std::runtime_error("simulation exceeded max_events at t=" + std::to_string(now));
      if (tnext <= now) { if (++zero_dt > opt.max_zero_dt) throw std::runtime_error("simulation livelocked at t=" + std::to_string(now) + " (" + std::to_string(jobs_left) + " jobs left)"); }
      else zero_dt = 0;
      double dt = std::max(0.0, tnext - now);
      res.inflight_time_integral += inflight_jobs * dt;
      for (int fid : active_flows) {
        Flow& f = flows[static_cast<size_t>(fid)];
        f.remaining = f.rate == kInf ? 0.0 : std::max(0.0, f.remaining - f.rate * dt);
      }
      now = tnext;
      // process everything due now
      std::vector<int> done_flows;
      for (int fid : active_flows) if (flow_done(flows[static_cast<size_t>(fid)])) done_flows.push_back(fid);
      for (int fid : done_flows) {
        Flow& f = flows[static_cast<size_t>(fid)];
        if (f.ended) continue;  // ended by a sibling's cancel during this step
        int aid = f.attempt; end_flow(fid); on_flow_done(aid);
      }
      // timers fire at their time, never early
      while (!timers.empty() && timers.top().t <= now) {
        Timer t = timers.top(); timers.pop();
        if (t.attempt < 0) {   // node event: background prefetch finished, the job traffic gets the full uplink back
          int k = -t.attempt;
          link_cap[static_cast<size_t>(up_link(k))] = pf_restore_cap[static_cast<size_t>(k)];
          recompute_rates();
          continue;
        }
        Attempt& a = attempts[static_cast<size_t>(t.attempt)];
        if (a.timer_seq != t.seq || a.phase == Phase::Cancelled || a.phase == Phase::Done) continue;
        on_timer(t.attempt);
      }
    }
    res.makespan = now;
    if (opt.prefetch == 1) for (auto& nd : topo.nodes) res.prefetch_bytes += pf_pushed(nd.id);
    finalize();
    return res;
  }
  void finalize() {
    for (auto& n : res.nodes) n.utilisation = (n.capacity > 0 && res.makespan > 0) ? n.busy_core_s / (n.capacity * res.makespan) : 0;
    res.duplicated_work_frac = res.total_work > 0 ? res.duplicated_work / res.total_work : 0;
    if (opt.compute_bounds) {
      // the bound follows the policy's own mode set / transport / client cost, not --mode
      BoundSpec bs = bound_spec_from_params(topo, wl);
      bs.mode_policy = sched.bound_mode_policy(); bs.alpha = sched.alpha(); bs.rho = sched.rho(); bs.per_node_store = sched.per_node_store();
      bs.overhead_cpu_s = sched.overhead();
      if (!sched.per_node_store()) bs.q_override = topo.params.q_pump_s;
      res.lb = lower_bounds(topo, wl, bs); res.efficiency = res.makespan > 0 ? res.lb.t_lb / res.makespan : 0;
    }
    res.nu_final = sched.nu(); res.nu_peak = sched.nu_peak(); res.nu_updates = sched.nu_updates(); res.nu_direction_changes = sched.nu_direction_changes();
    double hours = std::ceil(res.makespan / 3600.0);
    res.cost_usd = topo.client.price_per_hour * hours;
    for (auto& nd : topo.nodes) res.cost_usd += nd.price_per_hour * hours;
    res.windows = sched.windows();
  }
};

Simulator::Simulator(const Topology& t, const Workload& w, Scheduler& s, Options o)
    : impl_(std::make_unique<Impl>(t, w, s, o)) {}
Simulator::~Simulator() = default;
Result Simulator::run() { return impl_->run(); }

}  // namespace opticc::sim
