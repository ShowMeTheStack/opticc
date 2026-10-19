// opticc: scheduling policies (earliest-completion rule, admission window,
// list scheduling, hedging) behind one interface used by both the
// simulator and the real client/coordinator.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "opticc/model.hpp"
#include "opticc/predictor.hpp"

namespace opticc {

// Late binding: a remote job is first *admitted* (node == kUnbound: the driver runs the client-side
// preprocessing/hashing and then calls on_preprocessed()), and later *bound* (bind == true, node >= 0) when
// the scheduler picks its node; the driver then ships it. Early binding returns node >= 0 at once.
constexpr int kUnbound = -1;

struct Decision {
  int job = -1;
  int node = 0;                 // 0 = local, kUnbound = remote, node chosen later (late binding)
  Mode mode = Mode::Classic;
  bool hedge = false;           // duplicate of an already-running job
  bool bind = false;            // binds a previously admitted (preprocessed) job to `node`
  double w_hat = 0;             // predicted work at dispatch (drivers may order uplink queues by it)
};

// Driver protocol (simulator or coordinator):
//   on_job_ready(j, now)   -> j's dependencies are satisfied
//   decide(now)            -> dispatch decisions to apply right now
//   next_wakeup(now)       -> earliest time decide() wants to be called again
//                             even if no other event happens (hedging checks)
//   on_job_done(j, k, now, measured_w) -> first copy of j finished on k;
//                             the driver cancels any other copies itself
class Scheduler {
 public:
  Scheduler(const Topology& t, const Workload& w, std::shared_ptr<CostPredictor> pred);
  virtual ~Scheduler() = default;

  virtual void on_job_ready(int job, double now);
  // Late binding: the client-side work of an admitted job (p_j or q_j) is done; the job now waits
  // for a node (decide() returns a Decision with bind == true for it).
  virtual void on_preprocessed(int job, double now);
  // An admitted job whose client-side stage must wait on something outside the scheduler (the
  // compiler's system-include fingerprint on a cold start) gives its N^pp admission slot back while it waits; its mode
  // is kept and on_preprocessed() stages it as usual afterwards. Otherwise the waiters would hold N^pp and stall every admission.
  void park(int job);
  size_t parked_count() const { return parked_.size(); }
  // Every attempt of `job` was lost with its node (transport error, never a compile error). Its
  // reservations are released WITHOUT observing a completion, and under late binding the job is staged again with
  // its mode, so it is bound to another node instead of landing on the client as a fallback compile. Returns false
  // (nothing changed) when the policy binds early: the caller then falls back locally.
  bool requeue_lost(int job, Mode mode);
  // The coordinator compiles `job` on the client after all (computed __has_include, or a
  // fallback after a failed remote attempt): the remote reservation is released at once (it held a slot of the node
  // for the whole local compile) and the client's core is reserved instead, so the client counts as busy.
  void rebind_local(int job, double now);
  virtual void on_job_done(int job, int node, double now, double measured_w);
  virtual void on_slot_free(int /*node*/, double /*now*/) {}
  // The driver learned how many bytes it actually shipped for an attempt
  // (header-push: source + store misses). Feeds the per-node miss estimate.
  virtual void on_shipped(int job, int node, Mode mode, double bytes_shipped);
  // The driver appended jobs to the (shared) Workload after construction:
  // size per-job caches; new jobs get priority w_hat/f0 (no DAG known yet).
  virtual void on_jobs_added();
  // Re-derive policy state that depends on workload statistics (admission windows)
  // from what has been observed so far. No-op for policies without such state.
  virtual void refresh(double /*now*/) {}
  virtual std::vector<Decision> decide(double now) = 0;
  // Earliest time > now at which decide() must be called again without any
  // other event (infinity when nothing is pending). Hedging timers live here.
  virtual double next_wakeup(double /*now*/) const;
  virtual std::string name() const = 0;
  // Connection round trips charged per job by the transport this policy models.
  virtual double alpha() const { return topo_.params.alpha; }
  // Compression ratio of the transport this policy models (opticc: zstd rho;
  // distcc: lzo or none).
  virtual double rho() const { return topo_.params.rho; }
  double ship_bytes(const Job& j, Mode mode) const { return shipped_bytes(j, mode, topo_.params, rho()); }
  // Bytes this policy expects to ship for j to node k in `mode` (header-push:
  // source + expected store misses on k, learned online).
  double ship_bytes(const Job& j, Mode mode, int k) const;
  // Modes this policy may use for j on k (Params::mode_policy; header-push needs a known source size).
  std::vector<Mode> modes_for(const Job& j, int k) const;
  // Shadow price of client CPU currently in force (0 when unpriced).
  double nu() const { return nu_; }
  // nu diagnostics: number of updates, direction changes and the peak value.
  int nu_updates() const { return nu_updates_; }
  int nu_direction_changes() const { return nu_dir_changes_; }
  double nu_peak() const { return nu_peak_; }
  double nu_ceiling() const { return nu_max(); }
  // Every client compile slot is taken (local compiles + client-side preprocessing)
  bool client_saturated() const { return core_busy_until_[0].size() + pp_busy_until_.size() >= static_cast<size_t>(std::max(1, cap_[0])); }
  // Client CPU (reference seconds, before /f0) that shipping j in `mode` costs this policy: p_j classic,
  // q_j header-push (opticc); distcc-pump overrides with its include-server cost.
  virtual double ship_client_cpu(const Job& j, Mode mode) const { return mode == Mode::Classic ? j.preprocess_cpu_s : j.scan_cpu_s; }
  // Whether the worker keeps a cross-job content store (opticc: yes; distcc-pump: no).
  virtual bool per_node_store() const { return true; }
  // Mode set the lower bound may assume for this policy.
  virtual ModePolicy bound_mode_policy() const { return topo_.params.mode_policy; }
  // Per-job client CPU overhead outside the compiler: Params override, else the workload's value.
  double overhead() const { return topo_.params.client_overhead_cpu_s_per_job >= 0 ? topo_.params.client_overhead_cpu_s_per_job : wl_.client_overhead_cpu_s_per_job; }
  // Jobs that must run on the client regardless of windows/slots (link barrier, nothing to ship).
  static bool non_distributable(const Job& j) { return j.is_link_barrier || j.pre_size_bytes <= 0; }
  double miss_estimate(int k) const { return miss_ema_.at(static_cast<size_t>(k)); }
  // True when the transport keeps one pipelined connection per worker so that
  // transfers to a worker are serialised FIFO (opticc). distcc opens a
  // connection per job, so its transfers share the link concurrently.
  virtual bool pipelined_transport() const { return true; }

  // Stop dispatching to a node (worker unreachable). Running attempts are unaffected.
  // Liveness: when the last enabled remote node goes, the client must be able to compile even with local_slots = 0
  // (otherwise every job waits forever); the client window is raised to its core count.
  void disable_node(int k);
  // (Re-)admit node k (it came up late or reconnected): capacity re-read from the topology (cores may have been
  // unknown while it was down), client window back to its configured value; callers then refresh() the windows.
  void enable_node(int k);
  bool node_disabled(int k) const { return disabled_.at(static_cast<size_t>(k)) != 0; }
  // Per-node admission window currently in force (for diagnostics).
  const std::vector<int>& windows() const { return window_; }
  int inflight(int node) const { return inflight_.at(static_cast<size_t>(node)); }
  size_t ready_count() const { return ready_.size(); }
  size_t staged_count() const { return staged_.size(); }
  size_t admitted_count() const { return unbound_.size() + staged_.size(); }   // in preprocessing or awaiting a node
  int preprocess_admission() const { return npp_; }                           // N^pp
  int staged_capacity() const { return staged_cap_; }
  long admissions_by_work_conservation() const { return admit_wc_; }   // admitted beyond N^pp because remote slots idled
  bool late_binding() const { return topo_.params.binding == Binding::Late && supports_late_binding(); }
  const Topology& topology() const { return topo_; }
  // Current predicted work of a job as the policy sees it (re-predicted after learning).
  double predicted_work(int job) const { return w_hat(wl_.jobs.at(static_cast<size_t>(job))); }

  // ---- compile-cache affinity ----------------------------------------------------------------------------
  // `nodes` hold job's object in their compile cache. For those nodes ECT's candidate completion becomes
  //   client work (q_j / p_j as usual) + alpha*L_k + ship(source only in header-push) + hit_cost_s + o_j / B_down
  // and no worker core is reserved (a hit is answered before a compile slot is taken). hit_cost_s is the
  // worker-side lookup + object read (default 2 ms). Call before the job is decided; clear when invalidated.
  void set_affinity(int job, const std::vector<int>& nodes, double hit_cost_s = 0.002, double p_hit = 1.0);
  // One hit probability PER HOLDER (the coordinator learns the precision of each worker's Bloom
  // filter separately; 1.0 = certain, e.g. the worker reported this exact key this session)
  void set_affinity(int job, const std::vector<int>& nodes, double hit_cost_s, const std::vector<double>& p_per_node);
  double hit_p(int job, int k) const;              // 0 when k is not a holder
  int hits_inflight(int k) const { return hits_inflight_.at(static_cast<size_t>(k)); }
  // A hit with p_hit >= kCertainHit is treated as certain (no core, no window slot). 0.98: reached by a Bloom hint
  // whose LEARNED precision (learned_hit_p) is high enough; the prior alone (0.95) and stale hints (misses) stay below
  // it. A threshold too close to 1 would make every Bloom hit of a fresh client reserve a core: the holders' windows
  // fill with millisecond hits and work conservation sends the rest to non-holders, where they miss.
  static constexpr double kCertainHit = 0.98;
  // precision of worker Bloom-filter hints: Beta(19, 1) prior (mean 0.95, weight 20) updated with the outcomes of
  // jobs that RAN ON A HOLDER (a job placed elsewhere says nothing about the hint)
  static double learned_hit_p(uint64_t hits_on_holder, uint64_t outcomes_on_holder) {
    return (19.0 + static_cast<double>(hits_on_holder)) / (20.0 + static_cast<double>(std::max(outcomes_on_holder, hits_on_holder)));
  }
  // A Bloom hint is CERTAIN only when the one-sided 95% Wilson LOWER bound of its holder's observed
  // precision reaches kCertainHit — not the posterior mean (after 30/30 hits the mean is 0.98 but the lower bound is
  // ≈0.92). With p̂ = 1 that needs n ≥ z²·0.98/0.02 ≈ 133 hits on that holder. Bound on what this lets through: while a
  // hint is certain, the holder's true precision is ≥ 0.98 with 95% confidence, so the expected number of misses among
  // the certain dispatches is ≤ 2% of them (and at most window_[k] are in flight on that node at once).
  static double wilson_lower(double hits, double n, double z = 1.6448536) {
    if (n <= 0) return 0.0;
    double ph = std::min(1.0, std::max(0.0, hits / n)), z2 = z * z;
    double c = ph + z2 / (2 * n), r = z * std::sqrt(std::max(0.0, ph * (1 - ph) / n + z2 / (4 * n * n)));
    return (c - r) / (1 + z2 / n);
  }
  static bool hint_certain(double hits, double n) { return wilson_lower(hits, n) >= kCertainHit; }
  void clear_affinity(int job);
  bool cache_hit_expected(int job, int k) const;
  bool cache_hit_certain(int job, int k) const;   // expected AND p_hit >= kCertainHit
  size_t affinity_count() const { return affinity_.size(); }

 protected:
  struct Attempt {
    int node;
    Mode mode;
    double dispatched;
    double predicted_start;     // predicted compile start on the node (after client work + transfer)
    double predicted_done;      // value inserted into core_busy_until_
    double predicted_remote;    // uncontended R_jk at dispatch
    bool hedge;
    double next_check;          // next hedging evaluation time (inf = none)
    bool cache_hit = false;     // predicted cache hit, no entry in core_busy_until_
  };

  double w_hat(const Job& j) const;
  struct Prediction {
    double done = 0;            // predicted completion time
    double start = 0;           // predicted compile start
    double pp_done = 0;         // predicted end of client preprocessing (remote only)
    double up_done = 0;         // predicted end of upload (remote only)
    double remote = 0;          // uncontended R_jk
  };
  // Queue-aware predicted completion of j on node k in `mode` if dispatched at `t0` (earliest-completion rule with queues).
  // pp_done: the client-side work is already done (late binding), so no client core or p_j/q_j is charged.
  Prediction predicted_completion(const Job& j, int k, double t0, Mode mode = Mode::Classic, bool pp_done = false) const;
  // Earliest time a core on node k is predicted free for a job arriving at `arrival`.
  double predicted_core_start(int k, double arrival, double now) const;
  // Earliest time a window slot on k frees (predicted) for the (q+1)-th job
  // waiting for it (q = ready jobs already deferred towards k in this pass), or `now` if open.
  double predicted_slot_free(int k, double now, int q = 0) const;
  void record_dispatch(const Job& j, int k, double now, bool hedge, Mode mode = Mode::Classic, bool pp_done = false);
  // Late binding: admit j to client-side preprocessing in `mode` without choosing a node.
  void record_admit(const Job& j, double now, Mode mode);
  // Window ingredients for node k: mu_k (jobs/s) and the uncontended cycle time D_k (excluding the client-side
  // phase under late binding). Uses workload means as the policy can know them.
  void node_mu_D(int k, double* mu, double* D) const;
  void recompute_npp();                   // N^pp = ceil(c0 + t_pp * sum_k mu_k)
  virtual bool supports_late_binding() const { return false; }
  // Mode of the running (non-hedge) attempt of a job, Classic if unknown.
  Mode running_mode(int job) const;
  // Shadow price bookkeeping: sample_load() accumulates time-weighted client
  // utilisation and remote idleness between calls; update_nu() applies the
  // multiplicative rule every 2 s / 25 completions (see sched.cpp).
  void sample_load(double now);
  void update_nu(double now);
  double nu_max() const;
  double client_cpu_cost(const Job& j, int k, Mode mode) const;  // u_jk in client-CPU seconds
  // First hedging evaluation time for a fresh remote attempt (inf = never). Policies override.
  virtual double first_hedge_check(const Attempt& /*a*/, double /*now*/) const { return std::numeric_limits<double>::infinity(); }
  bool eligible(const Job& j, int k) const;    // node may run j at all (ignores window)
  // Dominance gate: may distributable job j be compiled on the client now?
  // Classic mode (every shipped job costs the client p_j): only when the remote side is saturated (every enabled
  // remote window full) -- a local compile while remote capacity idles spends w_j > p_j of the bottleneck resource.
  // Header-push/auto: only when a client core is free (or the remote side is saturated). Barrier jobs: always.
  bool local_gate(const Job& j) const;
  bool client_slot_free_for_waiting(const Job& j) const;   // client work conservation (header-push/auto)
  bool remote_saturated() const;
  // Work conservation: remote slots idle now = sum over enabled remote nodes with an open window of
  // max(0, cores - in flight); and the best such slot for j (min predicted completion), -1 if none.
  int idle_remote_slots() const;
  int best_idle_remote(const Job& j, double now, Mode* mode, bool pp_done) const;
  bool window_open(int k) const { return inflight_[static_cast<size_t>(k)] < window_[static_cast<size_t>(k)]; }
  void prune(double now);
  void drop_attempts(int job);                    // release in-flight, hit and core reservations of job's attempts
  // Longest predicted path (in local compile time) from each job to a sink.
  void compute_priorities();

  const Topology& topo_;
  const Workload& wl_;
  std::shared_ptr<CostPredictor> pred_;
  double mean_mem_;
  std::vector<int> cap_;                          // ~c_k, index = node id
  std::vector<int> window_;                       // admission window, index = node id
  std::vector<int> inflight_;
  std::vector<char> disabled_;                    // index = node id
  std::vector<std::multiset<double>> core_busy_until_;  // predicted finish times, index = node id
  std::multiset<double> pp_busy_until_;           // client cores consumed by preprocessing
  std::vector<double> uplink_free_at_;            // index = node id (0 unused)
  std::map<int, std::vector<Attempt>> running_;   // ordered: deterministic iteration
  std::vector<int> ready_;
  std::vector<double> prio_;
  std::vector<double> arrivals_;                  // ready times within the last kArrivalWindow seconds
  double last_now_ = 0;
  // Jobs added at run time (the coordinator's: no DAG is known, prio = w_hat/f0) are ordered by arrival bucket
  // (kAgeBucket s, oldest first) and LPT within a bucket: under pure LPT every newer, larger job would overtake an
  // older, smaller one, starving small jobs that gate later work. Once no job arrived for kTailQuiet s (the tail) pure
  // LPT applies again (Graham's tail bound).
  std::vector<double> ready_t_;                   // index = job id; first ready time
  std::vector<char> dyn_;                         // index = job id; 1 = added at run time (on_jobs_added)
  double last_arrival_ = -1e300;
  static constexpr double kAgeBucket = 1.0, kTailQuiet = 2.0;
  void sort_by_priority(std::vector<int>& v, double now, bool cp, bool lpt_tiebreak) const;
  // late binding state
  struct Unbound { Mode mode; double dispatched; double pp_done; };
  std::map<int, Unbound> unbound_;                // admitted, client-side work in progress
  std::map<int, Mode> parked_;                    // admitted, waiting outside the scheduler (no N^pp slot)
  std::vector<int> staged_;                       // preprocessed, awaiting a node
  std::map<int, Mode> staged_mode_;
  int npp_ = std::numeric_limits<int>::max();
  int staged_cap_ = std::numeric_limits<int>::max();
  long admit_wc_ = 0;
  double feed_rate_ = std::numeric_limits<double>::infinity();   // jobs/s the remote feed can absorb
  // p_hit is the probability that the predicted hit is real
  // (1 = the worker reported this exact full key this session; < 1 = a Bloom-filter hit, which can be a false
  // positive or an entry evicted since the worker's HELLO). Only a CERTAIN hit (p_hit >= kCertainHit) frees the job
  // from a core reservation and from the window; an uncertain one only lowers the expected completion time.
  struct Affinity { std::vector<int> nodes; double hit_cost_s; double p_hit = 1.0; std::vector<double> p_node; };
  // Certain hits take no core and no window slot, but at most window_[k] of them may be in flight on node k at once,
  // so a stale filter cannot pour an unbounded number of "free" jobs onto one worker.
  std::vector<int> hits_inflight_;
  std::unordered_map<int, Affinity> affinity_;    // job -> nodes holding its object
  mutable std::vector<double> what_cache_;
  mutable std::vector<uint64_t> what_ver_;        // predictor version the cache entry was computed with
  uint64_t prio_version_ = 0;
  int completions_ = 0;
  // header-push miss estimate per node (bytes), EMA of observed misses
  std::vector<double> miss_ema_;
  std::vector<int> miss_n_;
  // shadow price state
  double nu_ = 0;
  bool priced_ = true;
  double nu_last_update_ = 0, load_last_sample_ = 0, acc_client_busy_ = 0, acc_remote_idle_ = 0, acc_dt_ = 0;
  int nu_completions_at_ = 0;
  bool nu_warmed_ = false;            // the one-time warm-up jump has been applied
  double util_ema_ = -1, idle_ema_ = -1;   // smoothed load signals (time constant kNuTau) driving the update
  int nu_updates_ = 0, nu_dir_changes_ = 0, nu_last_dir_ = 0; double nu_peak_ = 0;
};

enum class HedgeRule { None, Kappa, T7 };

struct ECTOptions {
  bool bdp_window = false;      // admission window instead of plain slot count
  bool cp_priority = false;     // critical-path ordering of the ready set
  HedgeRule hedge = HedgeRule::None;
};

// Earliest-completion-time list scheduling with optional admission window,
// critical-path priority and hedging (kappa rule or survival-ratio rule).
class ECTScheduler : public Scheduler {
 public:
  ECTScheduler(const Topology& t, const Workload& w, std::shared_ptr<CostPredictor> pred,
               ECTOptions opt);
  std::vector<Decision> decide(double now) override;
  double next_wakeup(double now) const override;
  std::string name() const override;
  void refresh(double now) override;
  void on_job_done(int job, int node, double now, double measured_w) override;
  bool supports_late_binding() const override { return true; }
  int hedges_issued() const { return hedges_issued_; }
  int i2_core_moves() const { return i2_core_moves_; }   // bindings moved off a full node's queue
  // Survival-ratio hedging ingredients, exposed for tests/diagnostics.
  double survival(double ratio) const;            // S(x) = P(actual/predicted > x)
  double lambda_now(double horizon = 0.0) const;  // duplicate-work weight from idle capacity (arrival-aware)
  // Hedging guard: work already waiting (ready + admitted/staged) plus work expected to arrive within `horizon`
  // (recent arrival rate x mean predicted work), in reference CPU-seconds.
  double pending_work(double horizon) const;
 private:
  void recompute_windows();
  // argmin over (node, mode) of queue-aware completion plus nu * client CPU;
  // blocked (window-full) nodes are evaluated at their predicted slot-free time.
  // *blocked tells whether the winner is currently window-blocked.
  int best_node(const Job& j, double now, int exclude, double* completion, bool* blocked,
                const std::vector<int>* pending = nullptr, Mode* mode = nullptr) const;
  // Late binding: choose the node for a staged (preprocessed) job of fixed mode; remote nodes preferred,
  // the client only when no remote node is eligible. pp_done semantics in the prediction.
  int bind_node(const Job& j, Mode mode, double now, bool* blocked, const std::vector<int>* pending) const;
  void decide_admissions(double now, std::vector<Decision>* out);   // late binding: ready -> local | admitted
  void decide_bindings(double now, std::vector<Decision>* out);     // late binding: staged -> node
  bool hedge_due(const Attempt& a, const Job& j, double now, double r, double* recheck_dt) const;
  double first_hedge_check(const Attempt& a, double now) const override;
  ECTOptions opt_;
  int hedges_issued_ = 0;
  int i2_core_moves_ = 0;
  // Core-level rule: a node chosen by ECT whose cores are all taken (in flight >= c_k: the job
  // would queue) is replaced by the best node with an idle core, outside the tail and unless the job is hinted to k
  int core_level_i2(const Job& j, int k, double now, Mode* m, bool pp_done, size_t left);
  int hedge_max_;
  std::vector<double> ratios_;                    // sorted actual/predicted completion ratios (remote winners)
};

// distcc 3.x behaviour: hosts in list order with "/N" slots, localhost last,
// first free slot wins (lowest slot index across hosts first), connection per
// job (alpha = 2), no cost model, no window beyond the slot count.
// pump = true emulates distcc-pump (include server): same host/slot order, alpha = 2 and PS
// transport, but header-push semantics — the client ships the source plus its whole include
// closure with every job (distccd keeps no cross-job store; the include server's cache only
// avoids rescanning), pays the serialised include-server cost q_pump per
// job, and the worker preprocesses. Modelling assumptions: the include server's
// closure is at least the real one (we charge the exact closure, a lower bound on pump's
// bytes), lzo ratio rho_lzo on sources/headers, q_pump (5 ms placeholder).
// Localhost: distcc compiles locally only with a "localhost/N" host entry; the slot count
// comes from Params::distcc_localhost_slots, not from the client's cores.
// Dispatch-only ablation: `hp` = distcc's slot-order dispatch (early binding, no ECT, no window, no price)
// with EVERYTHING ELSE opticc's — header-push mode wherever modes_for allows it, opticc's transport (alpha, pipelining,
// zstd rho), the worker content store. Policy "distcc-hp"; the ablation isolates the scheduler's contribution.
class DistccEmulation : public Scheduler {
 public:
  DistccEmulation(const Topology& t, const Workload& w, std::shared_ptr<CostPredictor> pred, bool pump = false, bool hp = false);
  std::vector<Decision> decide(double now) override;
  std::string name() const override { return hp_ ? "distcc-hp" : pump_ ? "distcc-pump" : "distcc"; }
  double alpha() const override { return hp_ ? Scheduler::alpha() : 2.0; }
  double rho() const override;
  bool pipelined_transport() const override { return hp_ ? Scheduler::pipelined_transport() : false; }
  // pump: the include server costs q_pump per job on the client and the closure is shipped every time
  double ship_client_cpu(const Job& j, Mode mode) const override {
    if (hp_) return Scheduler::ship_client_cpu(j, mode);
    return mode == Mode::Classic ? j.preprocess_cpu_s : topo_.params.q_pump_s;
  }
  bool per_node_store() const override { return hp_ || !pump_; }
  ModePolicy bound_mode_policy() const override { return hp_ ? Scheduler::bound_mode_policy() : pump_ ? ModePolicy::HeaderPush : ModePolicy::Classic; }
 private:
  std::vector<int> slots_;      // index = node id
  bool pump_ = false;
  bool hp_ = false;
};

// Every job goes to a fixed node immediately when ready (tests / replays).
class FixedAssignmentScheduler : public Scheduler {
 public:
  FixedAssignmentScheduler(const Topology& t, const Workload& w, std::vector<int> assignment);
  std::vector<Decision> decide(double now) override;
  std::string name() const override { return "fixed"; }
 private:
  std::vector<int> assign_;
};

// policy: "distcc" | "distcc-pump" | "distcc-hp" | "ect" | "ect-window" | "ect-window-hedge" (kappa) | "ect-window-hedge-t7"
std::unique_ptr<Scheduler> make_scheduler(const std::string& policy, const Topology& t,
                                          const Workload& w, std::shared_ptr<CostPredictor> pred);


// The PER-WORKER learned precision of a worker's
// Bloom filter. Evidence decays (x0.995 per outcome, half-life ~140 outcomes), a miss on a holder halves it, and a worker
// (re)admitted with a fresh filter is reset to the prior. p(k) is the posterior mean under a Beta(19,1) prior, or exactly
// 1.0 once the one-sided 95% Wilson lower bound reaches Scheduler::kCertainHit (then the hint counts as certain).
class HolderTrust {
 public:
  void outcome(int node, bool hit) {
    if (node <= 0) return;
    size_t k = static_cast<size_t>(node);
    if (h_.size() <= k) { h_.resize(k + 1, 0.0); n_.resize(k + 1, 0.0); }
    h_[k] *= 0.995; n_[k] *= 0.995;
    if (!hit) { h_[k] *= 0.5; n_[k] *= 0.5; }
    n_[k] += 1.0; if (hit) h_[k] += 1.0;
  }
  double p(int node) const {
    double h = hits(node), n = outcomes(node);
    if (Scheduler::hint_certain(h, n)) return 1.0;
    return std::min(Scheduler::kCertainHit - 1e-4, (19.0 + h) / (20.0 + std::max(n, h)));   // never certain below the bound
  }
  bool reset(int node) {   // (re)admission with a fresh filter; true if evidence was forgotten
    size_t k = static_cast<size_t>(node);
    if (node <= 0 || k >= h_.size()) return false;
    h_[k] = 0; n_[k] = 0; return true;
  }
  double hits(int node) const { size_t k = static_cast<size_t>(node); return node > 0 && k < h_.size() ? h_[k] : 0.0; }
  double outcomes(int node) const { size_t k = static_cast<size_t>(node); return node > 0 && k < n_.size() ? n_[k] : 0.0; }
  size_t size() const { return h_.size(); }
 private:
  std::vector<double> h_, n_;
};

}  // namespace opticc
