// SPDX-License-Identifier: GPL-2.0-only
// Scheduler correctness and performance. Each test states the claim it checks.
#include "opticc/predictor.hpp"
#include "opticc/sim.hpp"
#include "opticc/sched.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace opticc;

namespace {
Workload flat_jobs(int n, double w, double src = 2e4) {
  Workload wl;
  for (int i = 0; i < n; ++i) {
    Job j; j.id = i; j.name = "tu" + std::to_string(i);
    j.work_ref_cpu_s = w; j.preprocess_cpu_s = 0.3 * w; j.scan_cpu_s = 0.01; j.pre_size_bytes = 2e6; j.src_size_bytes = src;
    j.obj_size_bytes = 3e4; j.mem_bytes = 3e8;
    wl.jobs.push_back(j);
  }
  return wl;
}
Topology pool3(int client_cores, std::vector<std::pair<int, double>> nodes, double rtt, double bw) {
  Topology t; t.client = Client{client_cores, 1.0, 0, 10e9, 10e9, 0}; t.params.alpha = 1.0; t.params.rho = 0.2;
  for (auto& [c, f] : nodes) { Node n; n.name = "w" + std::to_string(t.nodes.size()); n.cores = c; n.speed = f; n.rtt_s = rtt; n.bw_up_bps = bw; n.bw_down_bps = bw; t.nodes.push_back(n); }
  t.assign_ids(); return t;
}
int admitted(const std::vector<Decision>& ds) { int n = 0; for (auto& d : ds) if (d.node == kUnbound) ++n; return n; }
}  // namespace

// A job that waits outside the scheduler (cold-start system-include fingerprint) gives its N^pp admission slot back
// (park), so the next ready job is admitted at once; the parked job keeps its mode when it is staged afterwards.
// Without this, N^pp waiters could hold every admission slot for the whole scan (seconds under load).
OPTICC_TEST(park_releases_admission_slot) {
  Workload wl = flat_jobs(200, 1.0);
  Topology t = pool3(4, {{4, 1.0}, {4, 1.0}}, 0.001, 1e9);
  t.params.binding = Binding::Late; t.params.mode_policy = ModePolicy::HeaderPush;
  t.params.local_slots = 0;   // every job must be admitted for remote work (no local starts muddy the count)
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  for (auto& j : wl.jobs) s.on_job_ready(j.id, 0.0);
  std::vector<Decision> d0 = s.decide(0.0);
  const int npp = s.preprocess_admission();
  std::printf("  N^pp %d, admitted at t=0: %d, admitted count %zu\n", npp, admitted(d0), s.admitted_count());
  // admission-stage work conservation admits beyond N^pp up to the idle remote slots (8 here) when the cap is below
  // them
  CHECK(admitted(d0) == std::max(npp, 8));
  CHECK(admitted(s.decide(0.001)) == 0);           // caps exhausted: nothing more until a slot frees
  int victim = -1; for (auto& d : d0) if (d.node == kUnbound) { victim = d.job; break; }
  CHECK(victim >= 0);
  s.park(victim);
  CHECK(s.parked_count() == 1);
  std::vector<Decision> d1 = s.decide(0.002);
  std::printf("  after park: admitted %d more, parked %zu\n", admitted(d1), s.parked_count());
  CHECK(admitted(d1) == 1);                        // the freed slot is used at once
  s.on_preprocessed(victim, 0.01);                 // the wait ended: staged with its header-push mode
  bool bound_hp = false;
  for (auto& d : s.decide(0.01)) if (d.job == victim && d.bind) bound_hp = d.node > 0 && d.mode == Mode::HeaderPush;
  CHECK(bound_hp);
  CHECK(s.parked_count() == 0);
}

// Certain cache hits take no core and no window slot, but at most window_[k] of them are in flight on one node.
// A stale Bloom filter (or a re-joined worker) can no longer attract an unbounded number of "free" jobs: beyond the cap
// a hinted job reserves a core like a miss (its expected completion still counts the hit).
OPTICC_TEST(certain_hits_capped_per_node) {
  // early binding: every ready job is placed in one decision pass, so all 60 certain-hint jobs compete for node 1 at
  // once (the late-binding admission caps would let only a few through, so with late binding the test would never
  // reach the cap)
  Workload wl = flat_jobs(60, 1.0);
  Topology t = pool3(2, {{4, 1.0}, {4, 1.0}}, 0.001, 1e9);
  t.params.binding = Binding::Early; t.params.mode_policy = ModePolicy::HeaderPush; t.params.window_scale = 0;   // window = cores
  t.params.local_slots = 0;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  for (auto& j : wl.jobs) { s.set_affinity(j.id, {1}, 0.002, 1.0); s.on_job_ready(j.id, 0.0); }
  int to1 = 0;
  for (auto& d : s.decide(0.0)) if (d.node == 1) ++to1;
  std::printf("  window(1) %d, certain hits in flight on node 1: %d, reserved %d, jobs placed on node 1: %d\n", s.windows()[1],
              s.hits_inflight(1), s.inflight(1), to1);
  CHECK(s.hits_inflight(1) >= 1);
  CHECK(s.hits_inflight(1) <= s.windows()[1]);   // the cap: no unbounded "free" pile-up on the holder
  CHECK(to1 < 60);
}

// "Certain" needs the one-sided 95% Wilson LOWER bound of the holder's precision >= 0.98, not the posterior mean:
// 30/30 hits (mean 0.98) is not certain, ~133/133 is; one miss among 200 is not; a 0.8% false-positive rate over
// enough evidence is.
OPTICC_TEST(bloom_certainty_wilson_bound) {
  std::printf("  LB(30,30)=%.4f LB(133,133)=%.4f LB(199,200)=%.4f LB(4960,5000)=%.4f\n", Scheduler::wilson_lower(30, 30),
              Scheduler::wilson_lower(133, 133), Scheduler::wilson_lower(199, 200), Scheduler::wilson_lower(4960, 5000));
  CHECK(!Scheduler::hint_certain(30, 30));
  CHECK(!Scheduler::hint_certain(120, 120));
  CHECK(Scheduler::hint_certain(140, 140));
  CHECK(!Scheduler::hint_certain(199, 200));
  CHECK(Scheduler::hint_certain(4960, 5000));
  CHECK(!Scheduler::hint_certain(0, 0));
}

// Run-time jobs (the coordinator's: no DAG) are served by arrival bucket while jobs keep arriving (an older, smaller
// job is not overtaken by every newer, larger one) and by LPT again in the tail (no arrival for 2 s).
OPTICC_TEST(age_aware_order_for_runtime_jobs) {
  auto first_dispatched = [](double t_decide) {
    Workload wl;
    Topology t = pool3(4, {{8, 1.0}}, 0.001, 1e9);
    t.params.binding = Binding::Late; t.params.mode_policy = ModePolicy::HeaderPush; t.params.local_slots = 0;
    ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    Workload one = flat_jobs(6, 1.0);
    for (int i = 0; i < 6; ++i) {
      Job j = one.jobs[static_cast<size_t>(i)]; j.id = i; j.work_ref_cpu_s = i == 0 ? 0.2 : 5.0;
      wl.jobs.push_back(j); s.on_jobs_added();
      s.on_job_ready(i, i == 0 ? 0.0 : 1.5);   // job 0 (small) arrived 1.5 s before five large ones
    }
    for (auto& d : s.decide(t_decide)) return d.job;
    return -1;
  };
  int busy = first_dispatched(1.6), tail = first_dispatched(4.0);
  std::printf("  first dispatched while arriving: job %d; in the tail: job %d\n", busy, tail);
  CHECK(busy == 0);        // the oldest bucket goes first
  CHECK(tail != 0);        // tail: LPT (a 5 s job first)
}

// Persisted per-TU times. A recorded TU is predicted at its recorded time in the next build (EMA of the newest two),
// an unknown TU falls through to the base predictor, the table survives save/load, and malformed lines are skipped.
OPTICC_TEST(tu_history_roundtrip) {
  auto base = std::make_shared<AffineSizePredictor>();
  HistoryPredictor h(base);
  Job a; a.id = 0; a.name = "drivers/gpu/big.c"; a.pre_size_bytes = 1e6;
  Job b = a; b.id = 1; b.name = "lib/small.c";
  const double prior = base->predict(a);
  CHECK(std::fabs(h.predict(a) - prior) < 1e-12);
  h.record(a.name, 8.0); h.record(a.name, 12.0);   // EMA 1/2: 10
  h.record(b.name, 0.0);                            // not a compile time: ignored
  CHECK(std::fabs(h.predict(a) - 10.0) < 1e-9);
  CHECK(std::fabs(h.predict(b) - prior) < 1e-12);
  const std::string path = "tu_times.tsv";
  CHECK(h.save(path));
  { std::FILE* f = std::fopen(path.c_str(), "a"); std::fputs("garbage line\nx\tbad\n-1\tneg.c\n", f); std::fclose(f); }
  HistoryPredictor h2(std::make_shared<AffineSizePredictor>());
  size_t n = h2.load(path);
  const double pa = h2.predict(a);
  std::printf("  loaded %zu, predict(big) %.3f (prior %.3f)\n", n, pa, prior);
  CHECK(n == 1);
  CHECK(std::fabs(pa - 10.0) < 1e-9);
  CHECK(h2.history_predictions() == 1);
  CHECK(h2.load("does-not-exist.tsv") == 0);
  std::remove(path.c_str());
}

// A job whose worker was lost is staged again with its mode and bound to another worker (not compiled on the
// client); the lost attempt's reservation is released without a completion. Early binding declines (caller falls back).
OPTICC_TEST(requeue_lost_rebinds_elsewhere) {
  Workload wl = flat_jobs(4, 1.0);
  Topology t = pool3(4, {{4, 1.0}, {4, 1.0}}, 0.001, 1e9);
  t.params.binding = Binding::Late; t.params.mode_policy = ModePolicy::HeaderPush; t.params.local_slots = 0;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  for (auto& j : wl.jobs) s.on_job_ready(j.id, 0.0);
  for (auto& d : s.decide(0.0)) if (d.node == kUnbound) s.on_preprocessed(d.job, 0.0);
  int job = -1, lost = -1; Mode m = Mode::Classic;
  for (auto& d : s.decide(0.0)) if (d.bind && d.node > 0 && job < 0) { job = d.job; lost = d.node; m = d.mode; }
  CHECK(job >= 0 && lost > 0 && m == Mode::HeaderPush);
  const int before = s.inflight(lost);
  s.disable_node(lost);
  CHECK(s.requeue_lost(job, m));
  CHECK(s.inflight(lost) == before - 1);
  int rebound = -1; Mode m2 = Mode::Classic;
  for (auto& d : s.decide(0.01)) if (d.job == job && d.bind) { rebound = d.node; m2 = d.mode; }
  std::printf("  job %d lost on node %d, re-bound to node %d\n", job, lost, rebound);
  CHECK(rebound > 0 && rebound != lost);
  CHECK(m2 == Mode::HeaderPush);
  Topology te = t; te.params.binding = Binding::Early;
  ECTScheduler e(te, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  CHECK(!e.requeue_lost(0, Mode::HeaderPush));
}

// The nu controller reads remote idleness from observed occupancy. With w_hat under-predicting by 2x and every remote
// core busy (attempts running past their predicted finish, no completion yet), no core is idle, so nu must not rise
// (if overrunning attempts looked idle, nu would climb to nu_max with every worker busy).
OPTICC_TEST(nu_reads_observed_occupancy) {
  Workload wl = flat_jobs(40, 2.0);
  Topology t = pool3(4, {{4, 1.0}, {4, 1.0}}, 0.001, 1e9);
  t.params.binding = Binding::Early; t.params.mode_policy = ModePolicy::HeaderPush; t.params.local_slots = 0;
  t.params.work_conserving = false;
  ECTScheduler s(t, wl, std::make_shared<BiasedOraclePredictor>(0.5), ECTOptions{true, true, HedgeRule::None});
  for (auto& j : wl.jobs) s.on_job_ready(j.id, 0.0);
  const double nu0 = s.nu(), nmax = s.nu_ceiling();
  int dispatched = 0;
  for (double now = 0.0; now < 30.0; now += 0.25) for (auto& d : s.decide(now)) if (d.node > 0) ++dispatched;
  std::printf("  dispatched %d, busy %d+%d of 8 cores, nu0 %.3f -> nu %.3f (peak %.3f, nu_max %.1f, updates %d)\n", dispatched,
              s.inflight(1), s.inflight(2), nu0, s.nu(), s.nu_peak(), nmax, s.nu_updates());
  CHECK(nmax > 0);
  CHECK(s.nu_updates() > 3);
  CHECK(s.nu() <= nu0 + 1e-12);
  CHECK(s.nu_peak() <= nu0 + 1e-12);
}

// The signal the coordinator's background recorders/uploaders yield to (every client compile slot taken) is true
// exactly while local compiles fill the client's cores, and false again once one completes.
OPTICC_TEST(client_saturated_signal) {
  Workload wl = flat_jobs(6, 1.0);
  Topology t = pool3(2, {{1, 1.0}}, 0.001, 1e9);
  t.params.binding = Binding::Early; t.params.mode_policy = ModePolicy::Classic;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  CHECK(!s.client_saturated());
  s.disable_node(1);   // only the client is left: every job compiles locally
  for (auto& j : wl.jobs) s.on_job_ready(j.id, 0.0);
  std::vector<int> local;
  for (auto& d : s.decide(0.0)) if (d.node == 0) local.push_back(d.job);
  std::printf("  local starts %zu, saturated %d\n", local.size(), static_cast<int>(s.client_saturated()));
  CHECK(local.size() == 2);
  CHECK(s.client_saturated());
  s.on_job_done(local[0], 0, 1.0, 1.0);
  CHECK(!s.client_saturated());
}

// Dispatch-only ablation: "distcc-hp" keeps distcc's slot-order dispatch but uses opticc's header-push mode and
// transport model (alpha, rho, pipelining); "distcc" (the distcc-classic ablation) stays classic.
OPTICC_TEST(distcc_hp_policy_is_dispatch_only) {
  Workload wl = flat_jobs(12, 1.0);
  Topology t = pool3(2, {{4, 1.0}, {4, 1.0}}, 0.001, 1e9);
  t.params.mode_policy = ModePolicy::HeaderPush; t.params.alpha = 1.0; t.params.rho = 0.2;
  auto hp = make_scheduler("distcc-hp", t, wl, std::make_shared<OraclePredictor>());
  auto dc = make_scheduler("distcc", t, wl, std::make_shared<OraclePredictor>());
  CHECK(hp->name() == "distcc-hp");
  CHECK(hp->alpha() == 1.0 && dc->alpha() == 2.0);
  CHECK(hp->rho() == 0.2);
  CHECK(hp->pipelined_transport() && !dc->pipelined_transport());
  int remote_hp = 0, remote = 0, remote_dc_hp = 0;
  for (auto& j : wl.jobs) { hp->on_job_ready(j.id, 0.0); dc->on_job_ready(j.id, 0.0); }
  for (auto& d : hp->decide(0.0)) if (d.node > 0) { ++remote; if (d.mode == Mode::HeaderPush) ++remote_hp; }
  for (auto& d : dc->decide(0.0)) if (d.node > 0 && d.mode == Mode::HeaderPush) ++remote_dc_hp;
  std::printf("  distcc-hp: %d remote, %d header-push; distcc: %d header-push\n", remote, remote_hp, remote_dc_hp);
  CHECK(remote == 8);              // distcc's slot dispatch: one job per remote slot
  CHECK(remote_hp == remote);
  CHECK(remote_dc_hp == 0);
}

// No queueing while a core idles, at core level: windows larger than the core count (high-RTT windows) let ECT
// queue a job on a full fast node (3x) while the slow node has idle cores. Outside the tail, no binding may go to a node whose cores are all
// taken while another enabled node with an open window has a core free.
OPTICC_TEST(i2_core_level_no_queue_while_core_idles) {
  Workload wl = flat_jobs(60, 1.0);
  Topology t = pool3(4, {{4, 3.0}, {4, 1.0}}, 0.05, 1e9);
  t.params.binding = Binding::Late; t.params.mode_policy = ModePolicy::HeaderPush; t.params.local_slots = 0;
  t.params.window_scale = 3.0;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  for (auto& j : wl.jobs) s.on_job_ready(j.id, 0.0);
  int violations = 0, binds = 0, unbound_left = static_cast<int>(wl.jobs.size());
  double now = 0;
  for (int round = 0; round < 20; ++round, now += 0.001) {
    std::vector<int> cur = {0, s.inflight(1), s.inflight(2)};
    for (auto& d : s.decide(now)) {
      if (d.node == kUnbound) { s.on_preprocessed(d.job, now); continue; }
      if (!d.bind || d.node <= 0) continue;
      ++binds; --unbound_left;
      int k = d.node, o = 3 - k, cores = 4;
      int idle_other = (cur[static_cast<size_t>(o)] < cores && cur[static_cast<size_t>(o)] < s.windows()[static_cast<size_t>(o)]) ? cores - cur[static_cast<size_t>(o)] : 0;
      if (cur[static_cast<size_t>(k)] >= cores && idle_other > 0 && unbound_left + 1 >= idle_other) ++violations;
      cur[static_cast<size_t>(k)]++;
    }
  }
  std::printf("  windows %d/%d, binds %d, in flight fast %d slow %d, core-level moves %d, violations %d\n", s.windows()[1], s.windows()[2],
              binds, s.inflight(1), s.inflight(2), s.i2_core_moves(), violations);
  CHECK(binds >= 8);
  CHECK(s.windows()[1] > 4);   // the scenario needs a window above the core count
  CHECK(violations == 0);
  CHECK(s.inflight(2) >= 4);   // the slow node's cores are used
}

// A job bound remotely but compiled on the client (computed __has_include, or a fallback) gives the node's slot
// back at once and occupies a client core instead; no remote completion ratio is recorded for it.
// A finished job's affinity entry is erased.
OPTICC_TEST(rebind_local_frees_remote_slot) {
  Workload wl = flat_jobs(4, 1.0);
  Topology t = pool3(1, {{4, 1.0}}, 0.001, 1e9);
  t.params.binding = Binding::Early; t.params.mode_policy = ModePolicy::HeaderPush; t.params.local_slots = 0;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  s.set_affinity(0, {1}, 0.002, 0.5);
  for (auto& j : wl.jobs) s.on_job_ready(j.id, 0.0);
  int job = -1;
  for (auto& d : s.decide(0.0)) if (d.node == 1 && job < 0) job = d.job;
  CHECK(job >= 0);
  CHECK(s.decide(1.0).empty());           // ages out the client-side scan reservations of t = 0
  CHECK(!s.client_saturated());
  const int before = s.inflight(1);
  s.rebind_local(job, 0.1);
  std::printf("  node 1 in flight %d -> %d, client saturated %d\n", before, s.inflight(1), static_cast<int>(s.client_saturated()));
  CHECK(s.inflight(1) == before - 1);
  CHECK(s.client_saturated());            // 1 client core, now compiling the job
  s.on_job_done(job, 1, 2.0, 1.0);
  s.decide(2.0);
  CHECK(!s.client_saturated());
  s.on_job_done(0, 1, 2.0, 1.0);
  CHECK(s.hit_p(0, 1) == 0.0);            // affinity cleared
}

// w_j is the work of compiling from source. A classic worker compiles the .i (w - p), a header-push worker
// preprocesses itself (w), so p is not counted twice. The model
// (remote_time), the scheduler's prediction and the simulator's ground truth agree.
OPTICC_TEST(work_definition_from_source) {
  Workload wl = flat_jobs(1, 1.0);   // w = 1, p = 0.3, q = 0.01
  Job& j = wl.jobs[0]; j.pre_size_bytes = 1; j.src_size_bytes = 1; j.obj_size_bytes = 1;
  Topology t = pool3(1, {{1, 1.0}}, 0.0, 1e15);
  t.params.local_slots = 0;
  const double rc = remote_time(j, 1.0, t.node(1), t.client, t.params, Mode::Classic);
  const double rh = remote_time(j, 1.0, t.node(1), t.client, t.params, Mode::HeaderPush, 0.0);
  std::printf("  R classic %.3f (p/f0 + (w-p)/f), R header-push %.3f (q/f0 + w/f)\n", rc, rh);
  CHECK(std::fabs(rc - 1.0) < 1e-3);
  CHECK(std::fabs(rh - 1.01) < 1e-3);
  CHECK(std::fabs(remote_compile_work(1.0, 2.0, Mode::Classic) - 0.05) < 1e-12);   // floored
  t.params.mode_policy = ModePolicy::HeaderPush; t.params.binding = Binding::Early;
  auto s = make_scheduler("ect-window", t, wl, std::make_shared<OraclePredictor>());
  sim::Result r = sim::Simulator(t, wl, *s).run();
  std::printf("  simulated header-push makespan %.3f\n", r.makespan);
  CHECK(r.makespan < 1.2);   // counting p twice would give q + (w + p) = 1.31
}
