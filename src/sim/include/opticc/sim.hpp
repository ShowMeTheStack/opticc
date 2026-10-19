// opticc-sim: discrete-event fluid simulator of the build model.
// Drives any opticc::Scheduler through the same callbacks the real
// coordinator uses, so policy code is shared by construction.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "opticc/bounds.hpp"
#include "opticc/model.hpp"
#include "opticc/sched.hpp"
#include "opticc/predictor.hpp"
#include "opticc/speed.hpp"

namespace opticc::sim {

enum class Transport { Auto, Fifo, ProcessorSharing };
// Order of the per-node uplink queue under FIFO transport: arrival order, or
// largest predicted compile first (Jackson's rule).
enum class UplinkOrder { Auto, Fifo, Jackson };

struct Options {
  double eps_bytes = 1e-3;       // a flow with fewer bytes left is complete (never a time tolerance)
  bool compute_bounds = true;
  Transport transport = Transport::Auto;  // Auto: ask the scheduler (pipelined_transport())
  UplinkOrder uplink_order = UplinkOrder::Auto;  // Auto: Jackson for pipelined (opticc) transports, FIFO otherwise
  uint64_t max_events = 200000000;         // watchdog: abort with a diagnostic beyond this many loop iterations
  uint64_t max_zero_dt = 1000000;          // watchdog: consecutive zero-advance iterations
  // Straggler injection: each remote compile is slowed by
  // straggler_factor with probability straggler_prob (seeded).
  double straggler_prob = 0.0;
  double straggler_factor = 1.0;
  uint64_t seed = 1;
  // Release model: -1 = the workload's release_window if set, else all at once;
  // 0 = all jobs released when their dependencies are met; N > 0 = make -jN: at most N released-but-unfinished
  // jobs, released in trace order as earlier ones complete.
  int release_window = -1;
  // Compile cache: cached_on[j] = node holding j's object from a previous build (-1 none).
  // A remote attempt of j on that node uploads only the source (header-push) and returns the object after
  // cache_hit_cost_s without taking a worker core. The scheduler is told via Scheduler::set_affinity.
  std::vector<int> cached_on;
  double cache_hit_cost_s = 0.002;
  // affinity the scheduler BELIEVES (e.g. a stale Bloom-filter hit after a header edit) without the object being
  // cached: believed_on[j] = node announced as holder, with hit probability believed_p. Only cached_on produces hits.
  std::vector<int> believed_on;
  double believed_p = 0.95;
  // Header prefetch. -1 = legacy store model (header-push with no header ids in the trace ships the full closure
  // every job, no NEED round trip); 0 = prefetch off: stores warm only
  // through jobs' misses, and every job with a miss pays one NEED round trip; 1 = prefetch on: misses travel with the
  // job (piggyback, no NEED round) and a background push of the most popular headers (warm-start ranking) runs per node
  // at prefetch_frac of its uplink until prefetch_budget_bytes, during which jobs see (1 - prefetch_frac) of the uplink.
  // Traces without header ids use an aggregate warmth model: miss_j = closure_j * max(resid, 1 - W_k / U), W_k the
  // header bytes node k has received (job misses + background), U the header union of the build.
  int prefetch = -1;
  double prefetch_frac = 0.3;
  double prefetch_budget_bytes = 64e6;
  double header_union_bytes = 64e6;
  double prefetch_resid = 0.04;
  // speed estimation: when `believed` is set, the scheduler runs on that topology (its
  // node speeds may be wrong); each remote completion feeds speed_est with the predictor's w_hat and the true CPU
  // time, the believed speeds are updated from the estimator, and the work reported to the scheduler is
  // cpu * believed speed -- exactly what the coordinator does.
  Topology* believed = nullptr;
  opticc::SpeedEstimator* speed_est = nullptr;
  opticc::CostPredictor* predictor = nullptr;
};

struct NodeStats {
  int id = 0;
  std::string name;
  int capacity = 0;
  double busy_core_s = 0;      // compile (and, for the client, preprocessing) core-seconds
  double bytes_up = 0;         // client -> node (compressed, as shipped)
  double bytes_down = 0;
  int jobs_completed = 0;      // jobs whose winning copy ran here
  int attempts = 0;
  double utilisation = 0;      // busy_core_s / (capacity * makespan)
};

struct Result {
  std::string policy;
  double makespan = 0;
  std::vector<NodeStats> nodes;           // index = node id (0 = client)
  std::vector<double> job_ready_time, job_dispatch_time, job_done_time, job_latency;
  std::vector<int> job_node;              // node of the winning copy
  double total_work = 0;
  double duplicated_work = 0;             // ref-CPU seconds spent on losing copies
  double duplicated_work_frac = 0;
  int hedges = 0;
  int hedges_won = 0;                     // duplicates that finished first
  int stragglers_injected = 0;
  int max_inflight_node = 0;              // max over time of in-flight attempts on any remote node (window check)
  double inflight_time_integral = 0;      // integral of (#jobs dispatched & not done) dt
  double client_preprocess_s = 0;
  double bytes_up_total = 0, bytes_down_total = 0;
  int remote_jobs = 0, local_jobs = 0;
  int header_push_jobs = 0;               // winning copies shipped in header-push mode
  double header_miss_bytes = 0;           // raw (uncompressed) header bytes shipped on store misses
  double prefetch_bytes = 0;              // raw header bytes pushed in the background
  int need_rounds = 0;                    // jobs that paid a NEED round trip (prefetch off)
  double client_scan_s = 0;               // client CPU spent on header-push closure/hash work (q_j)
  int cache_hits = 0;                     // winning copies served from a worker's compile cache
  double client_overhead_s = 0;           // client CPU spent on the per-job overhead
  int release_window = 0;                 // release model actually used (0 = all at once)
  double nu_final = 0, nu_peak = 0; int nu_updates = 0, nu_direction_changes = 0;   // adaptive nu diagnostics
  LowerBounds lb;
  double efficiency = 0;                  // lb.t_lb / makespan
  double cost_usd = 0;                    // hourly billing, rounded up per node
  std::vector<int> windows;
  json to_json() const;
};

class Simulator {
 public:
  Simulator(const Topology& t, const Workload& w, Scheduler& s, Options o = {});
  ~Simulator();
  Result run();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace opticc::sim
