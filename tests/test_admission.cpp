// SPDX-License-Identifier: GPL-2.0-only
// Scheduler checks: admission-stage work conservation, the idle-slot helpers in isolation, and the per-worker Bloom
// trust used by the coordinator. Each test states the claim it checks.
#include "opticc/predictor.hpp"
#include "opticc/sched.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace opticc;

namespace {
Workload flat(int n, double w) {
  Workload wl;
  for (int i = 0; i < n; ++i) {
    Job j; j.id = i; j.name = "tu" + std::to_string(i);
    j.work_ref_cpu_s = w; j.preprocess_cpu_s = 0.3 * w; j.scan_cpu_s = 0.01; j.pre_size_bytes = 2e6; j.src_size_bytes = 2e4;
    j.obj_size_bytes = 3e4; j.mem_bytes = 3e8;
    wl.jobs.push_back(j);
  }
  return wl;
}
Topology pool(int client_cores, std::vector<std::pair<int, double>> nodes) {
  Topology t; t.client = Client{client_cores, 1.0, 0, 10e9, 10e9, 0}; t.params.alpha = 1.0; t.params.rho = 0.2;
  for (auto& [c, f] : nodes) { Node n; n.name = "w" + std::to_string(t.nodes.size()); n.cores = c; n.speed = f; n.rtt_s = 0.001; n.bw_up_bps = 1e9; n.bw_down_bps = 1e9; t.nodes.push_back(n); }
  t.assign_ids(); return t;
}
int admitted(const std::vector<Decision>& ds) { int n = 0; for (auto& d : ds) if (d.node == kUnbound) ++n; return n; }

// exposes the protected idle-slot helpers for isolated tests
struct ProbeECT : ECTScheduler {
  using ECTScheduler::ECTScheduler;
  using Scheduler::idle_remote_slots;
  using Scheduler::best_idle_remote;
  using Scheduler::client_slot_free_for_waiting;
};
}  // namespace

// N^pp is sized from PREDICTED client-side time and service rates (about 1 to 2 here); with that cap alone, ready jobs
// could wait at admission while 8 remote slots idle. Claim: after every decision, either no ready job waits or the jobs
// already admitted/staged cover every idle remote slot (remote capacity never waits for an admission decision), and
// the excess over N^pp is bounded by the idle slots (no unbounded buffering).
OPTICC_TEST(admission_work_conserving_beyond_npp) {
  Workload wl = flat(200, 1.0);
  Topology t = pool(4, {{4, 1.0}, {4, 1.0}});
  t.params.binding = Binding::Late; t.params.mode_policy = ModePolicy::HeaderPush; t.params.local_slots = 0;
  ProbeECT s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  for (auto& j : wl.jobs) s.on_job_ready(j.id, 0.0);
  const int npp = s.preprocess_admission();
  const int idle0 = s.idle_remote_slots();
  std::vector<Decision> d = s.decide(0.0);
  std::printf("  N^pp %d, idle remote slots %d, admitted at t=0: %d (by work conservation: %ld), ready %zu\n", npp, idle0,
              admitted(d), s.admissions_by_work_conservation(), s.ready_count());
  CHECK(npp < idle0);                                    // the scenario under test: the cap is below the capacity
  CHECK(static_cast<int>(s.admitted_count()) >= idle0);  // every idle remote slot has a job admitted for it
  CHECK(static_cast<int>(s.admitted_count()) <= std::max(npp, idle0));   // bounded: no buffering beyond the idle slots
  CHECK(s.admissions_by_work_conservation() > 0);
  // drive the pipeline (preprocess what was admitted, bind) and check after every decision: if a job is still ready,
  // the admitted/staged jobs cover every idle remote slot
  double now = 0.0; int violations = 0, checks = 0;
  for (int step = 0; step < 20; ++step) {
    now += 0.01;
    for (auto& x : d) if (x.node == kUnbound) s.on_preprocessed(x.job, now);
    d = s.decide(now);
    ++checks;
    if (s.ready_count() > 0 && static_cast<int>(s.admitted_count()) < s.idle_remote_slots()) ++violations;
  }
  std::printf("  %d checks, invariant violations %d, idle remote slots at the end %d\n", checks, violations, s.idle_remote_slots());
  CHECK(violations == 0);
}

// The idle-slot helpers in isolation. idle_remote_slots counts open windows' free cores; best_idle_remote prefers the
// faster idle node and moves to the slower one only when the faster is full, and returns -1 when no remote core idles;
// client_slot_free_for_waiting holds in header-push while a client core is free and never in classic mode.
OPTICC_TEST(i2_helpers_in_isolation) {
  Workload wl = flat(40, 1.0);
  Topology t = pool(2, {{2, 2.0}, {2, 1.0}});   // node 1 fast (2 cores), node 2 slow (2 cores)
  t.params.binding = Binding::Early; t.params.mode_policy = ModePolicy::HeaderPush; t.params.window_scale = 0;   // window = cores
  ProbeECT s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  const Job& j0 = wl.jobs[0];
  Mode m = Mode::HeaderPush;
  std::printf("  idle %d, best idle %d\n", s.idle_remote_slots(), s.best_idle_remote(j0, 0.0, &m, false));
  CHECK(s.idle_remote_slots() == 4);
  CHECK(s.best_idle_remote(j0, 0.0, &m, false) == 1);   // the faster node first
  CHECK(s.client_slot_free_for_waiting(j0));             // header-push, client cores free
  // one job first: a partly occupied open window must count only its FREE cores
  s.on_job_ready(wl.jobs[0].id, 0.0);
  int rem1 = 0;
  for (auto& d : s.decide(0.0)) if (d.node >= 1) ++rem1;
  std::printf("  one job bound remotely: %d, idle %d\n", rem1, s.idle_remote_slots());
  CHECK(rem1 == 1);
  CHECK(s.idle_remote_slots() == 3);
  for (size_t i = 1; i < wl.jobs.size(); ++i) s.on_job_ready(wl.jobs[i].id, 0.0);
  int to1 = 0, to2 = 0;
  for (auto& d : s.decide(0.0)) { if (d.node == 1) ++to1; if (d.node == 2) ++to2; }
  std::printf("  after one decision: node1 %d node2 %d, idle %d, best idle %d\n", to1, to2, s.idle_remote_slots(),
              s.best_idle_remote(j0, 0.0, &m, false));
  CHECK(s.idle_remote_slots() == 0);
  CHECK(s.best_idle_remote(j0, 0.0, &m, false) == -1);  // no remote core idles
  Topology tc = t; tc.params.mode_policy = ModePolicy::Classic;
  ProbeECT c(tc, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  CHECK(!c.client_slot_free_for_waiting(j0));            // classic: shipping costs the client, never "free"
}

// The coordinator's per-worker Bloom trust under a regime change. ~218 consecutive clean hits make the holder certain
// (Wilson LB >= 0.98, p == 1); a burst of misses (a stale filter) drops it below certainty at once and keeps it there;
// other holders are unaffected.
OPTICC_TEST(holder_trust_regime_change) {
  HolderTrust tr;
  // with the x0.995 decay the evidence saturates at n = 200; certainty (Wilson LB >= 0.98, i.e. n >~ 133) therefore
  // takes ~218 consecutive clean hits, not 133 (after 200 hits n = 126.6, p = 0.9799)
  int k = 0;
  while (tr.p(1) < 1.0 && k < 1000) { tr.outcome(1, true); tr.outcome(2, true); ++k; }
  std::printf("  certain after %d consecutive hits (h %.1f n %.1f)\n", k, tr.hits(1), tr.outcomes(1));
  CHECK(k >= 200 && k <= 240);
  CHECK(tr.p(1) == 1.0);
  const double n_before = tr.outcomes(1);
  for (int i = 0; i < 3; ++i) tr.outcome(1, false);
  std::printf("  after 3 misses: p1 %.4f p2 %.4f (n %.1f -> %.1f)\n", tr.p(1), tr.p(2), n_before, tr.outcomes(1));
  CHECK(tr.outcomes(1) < 0.2 * n_before);                // each miss halves the evidence: 3 misses keep < 1/8 of it
  CHECK(tr.p(1) < Scheduler::kCertainHit);
  CHECK(tr.p(2) == 1.0);
  for (int i = 0; i < 50; ++i) tr.outcome(1, true);
  CHECK(tr.p(1) < Scheduler::kCertainHit);               // 50 clean hits are not enough to recover certainty
}

// A worker (re)admitted with a fresh filter starts from the prior: the trust learned on its previous cache is
// forgotten (the coordinator calls reset() on every admission; the stale-filter reload is next to it).
OPTICC_TEST(holder_trust_reset_on_readmission) {
  HolderTrust tr;
  for (int i = 0; i < 300; ++i) tr.outcome(3, true);
  CHECK(tr.p(3) == 1.0);
  CHECK(tr.reset(3));
  std::printf("  after reset: p3 %.4f (h %.1f n %.1f)\n", tr.p(3), tr.hits(3), tr.outcomes(3));
  CHECK(tr.p(3) < Scheduler::kCertainHit);
  CHECK(tr.p(3) > 0.94 && tr.p(3) < 0.96);               // the Beta(19,1) prior mean 0.95
  CHECK(!tr.reset(9));                                    // unknown node: nothing to forget
}

// The compile-cache index marks a worker as a holder ONLY from that worker's own RESULT (cache "hit"/"store",
// which the worker reports after the store succeeded). A write-back CACHE_PUT is unacknowledged and must never mark
// "held" (an upload path that recorded "store" at once would be wrong). The coordinator is not unit-constructible, so
// this is a source guard: the coordinator has exactly one cache_idx_.record_* call and it takes the worker-reported
// status of a RESULT; the upload function (the one calling send_cache_put) only reads it.
OPTICC_TEST(cache_index_held_only_from_worker_result) {
  std::ifstream f("src/opticc/coordinator.cpp");
  CHECK(f.good());
  std::stringstream ss; ss << f.rdbuf(); const std::string src = ss.str();
  const std::string call = "cache_idx_.record_result(res.cache_key, node, res.cache)";
  int records = 0; size_t pos = 0, rec_at = std::string::npos;
  while ((pos = src.find("cache_idx_.record_", pos)) != std::string::npos) { rec_at = pos; ++records; ++pos; }
  std::printf("  cache_idx_.record_* calls in coordinator.cpp: %d\n", records);
  CHECK(records == 1);
  CHECK(rec_at != std::string::npos && src.compare(rec_at, call.size(), call) == 0);
  size_t put = src.find("send_cache_put(");
  CHECK(put != std::string::npos);
  size_t fn_begin = src.rfind("\n}\n", put), fn_end = src.find("\n}\n", put);
  CHECK(fn_begin != std::string::npos && fn_end != std::string::npos);
  // the upload function may READ the index (it skips keys a worker already holds) but never writes it
  const std::string up_fn = src.substr(fn_begin, fn_end - fn_begin);
  CHECK(up_fn.find("cache_idx_.record_") == std::string::npos);
  CHECK(rec_at < fn_begin || rec_at >= fn_end);
}
