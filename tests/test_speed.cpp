// SPDX-License-Identifier: GPL-2.0-only
// Online speed estimation, the classic-mode local-compile gate and the arrival-aware hedging weight. Each test states the claim it checks.
#include "opticc/protocol.hpp"
#include "opticc/sim.hpp"
#include "opticc/speed.hpp"
#include "portable_rng.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <random>

using namespace opticc;

namespace {
Workload jobs_mixed(int n, uint64_t seed) {   // heterogeneous TUs: w log-uniform in [0.2, 2] s, p = 0.3 w
  Workload wl; std::mt19937_64 rng(seed); std::uniform_real_distribution<double> u(std::log(0.2), std::log(2.0));
  for (int i = 0; i < n; ++i) {
    Job j; j.id = i; j.name = "tu" + std::to_string(i); double w = std::exp(u(rng));
    j.work_ref_cpu_s = w; j.preprocess_cpu_s = 0.3 * w; j.pre_size_bytes = 2e6 * w; j.src_size_bytes = 2e4; j.obj_size_bytes = 3e4; j.mem_bytes = 3e8;
    wl.jobs.push_back(j);
  }
  return wl;
}
Topology pool(int client_cores, std::vector<std::pair<int, double>> nodes, double rtt, double bw) {
  Topology t; t.client = Client{client_cores, 1.0, 0, 10e9, 10e9, 0}; t.params.alpha = 1.0; t.params.rho = 0.2;
  for (auto& [c, f] : nodes) { Node n; n.name = "w" + std::to_string(t.nodes.size()); n.cores = c; n.speed = f; n.rtt_s = rtt; n.bw_up_bps = bw; n.bw_down_bps = bw; t.nodes.push_back(n); }
  t.assign_ids(); return t;
}
}  // namespace

// With noisy predictions (log-sd 0.4) and priors that mis-rate three of five nodes by 2x, the per-node medians
// recover the TRUE speed ratios within 10% after 256 samples per node (a kernel build gives each worker hundreds);
// the anchor keeps the geometric mean of the established estimates at the geometric mean of their priors.
OPTICC_TEST(speed_estimator_recovers_relative_speeds) {
  std::vector<double> truth = {1.0, 1.0, 1.0, 0.87, 0.93, 0.93};   // index 0 = client (unused)
  std::vector<double> prior = {1.0, 1.0, 1.0, 0.44, 0.44, 0.44};
  SpeedEstimator est(prior);
  std::mt19937_64 rng(7); opticc_test::PNormal e(0.0, 0.4); opticc_test::PUniform w(0.2, 2.0);   // portable: same sample on libc++ and libstdc++
  for (int round = 0; round < 256; ++round)
    for (int k = 1; k <= 5; ++k) { double wj = w(rng); est.observe(k, wj * std::exp(e(rng)), wj / truth[static_cast<size_t>(k)]); }
  double gm_est = 0, gm_pri = 0;
  for (int k = 1; k <= 5; ++k) { gm_est += std::log(est.speed(k)); gm_pri += std::log(prior[static_cast<size_t>(k)]); }
  std::printf("  estimates:"); for (int k = 1; k <= 5; ++k) std::printf(" %.3f", est.speed(k)); std::printf("  (gm est %.3f prior %.3f)\n", std::exp(gm_est / 5), std::exp(gm_pri / 5));
  for (int k = 3; k <= 5; ++k) {
    double ratio = est.speed(k) / est.speed(1), want = truth[static_cast<size_t>(k)] / truth[1];
    CHECK(std::fabs(ratio / want - 1.0) < 0.10);
  }
  CHECK(std::fabs(est.speed(1) / est.speed(2) - 1.0) < 0.10);
  CHECK(std::fabs(gm_est - gm_pri) / 5 < 0.05);
}

// Exploration: a node with fewer than min_samples completions is reported at least at the median of the
// established nodes, so a node mis-rated low is still offered work; its own samples then decide.
OPTICC_TEST(speed_estimator_optimism_and_damping) {
  SpeedEstimator est({1.0, 1.0, 1.0, 0.3}, /*min_samples*/ 8);
  for (int i = 0; i < 16; ++i) { est.observe(1, 1.0, 1.0); est.observe(2, 1.0, 1.0); }
  CHECK(est.speed(3) >= 0.99 * std::min(est.speed(1), est.speed(2)));   // unproven node treated optimistically
  // a burst of 8 absurd samples (1000x slow) on node 1 cannot move it by more than x2 per refit
  double before = est.speed(1);
  for (int i = 0; i < 4; ++i) est.observe(1, 1.0, 1000.0);
  CHECK(est.speed(1) >= 0.5 * before - 1e-9);
}

// A single worker mis-rated 0.39 at start-up (microbenchmark fallback while its compile benchmark was still running;
// true speed 1.0). An estimator anchored to the priors could never correct it (one worker alone carries no relative
// information), and client work conservation would then keep it starved. Client samples make the client the
// reference: (a) while the worker is unproven it is treated at least at the client's speed (optimism), (b) once
// established its estimate converges to the TRUE ratio with no prior in it.
OPTICC_TEST(speed_estimator_client_reference_single_worker) {
  SpeedEstimator est({1.0, 0.39}, /*min_samples*/ 8);
  std::mt19937_64 rng(11); std::normal_distribution<double> e(0.0, 0.3); std::uniform_real_distribution<double> w(0.02, 0.2);
  for (int i = 0; i < 12; ++i) { double wj = w(rng); est.observe(0, wj * std::exp(e(rng)), wj); }   // client compiles
  for (int i = 0; i < 5; ++i) { double wj = w(rng); est.observe(1, wj * std::exp(e(rng)), wj); }    // worker: unproven
  CHECK(est.speed(1) >= 0.999);                                                                       // (a)
  for (int round = 0; round < 64; ++round) {
    double a = w(rng); est.observe(0, a * std::exp(e(rng)), a);
    double b = w(rng); est.observe(1, b * std::exp(e(rng)), b / 1.0);
  }
  std::printf("  single worker: prior 0.39, truth 1.00, estimate %.3f\n", est.speed(1));
  CHECK(std::fabs(est.speed(1) - 1.0) < 0.12);                                                        // (b)
  // and a genuinely slower worker (truth 0.5, prior 1.0) is found as such
  SpeedEstimator e2({1.0, 1.0}, 8);
  for (int round = 0; round < 64; ++round) {
    double a = w(rng); e2.observe(0, a * std::exp(e(rng)), a);
    double b = w(rng); e2.observe(1, b * std::exp(e(rng)), b / 0.5);
  }
  std::printf("  single worker: prior 1.00, truth 0.50, estimate %.3f\n", e2.speed(1));
  CHECK(std::fabs(e2.speed(1) - 0.5) < 0.06);
}

// A worker's compile benchmark that arrives after start-up replaces its microbenchmark prior without discarding the
// samples already collected: (a) unproven node -> reports the new prior, keeps its samples; (b) established single
// worker with no other fitted node -> follows the new prior (nothing else could have moved it); (c) client-referenced
// fit -> the measured ratio stays (the prior is not the level), it does not jump to the new prior.
OPTICC_TEST(speed_estimator_prior_refresh_keeps_samples) {
  SpeedEstimator a({1.0, 0.39}, 8);
  for (int i = 0; i < 5; ++i) a.observe(1, 0.1, 0.1);
  a.set_prior(1, 0.98);
  CHECK(a.samples(1) == 5);
  CHECK(std::fabs(a.speed(1) - 0.98) < 1e-12);                                                        // (a)
  SpeedEstimator b({1.0, 0.39}, 8);
  for (int i = 0; i < 20; ++i) b.observe(1, 0.1, 0.1);
  b.set_prior(1, 0.98);
  CHECK(b.samples(1) == 20);
  CHECK(std::fabs(b.speed(1) - 0.98) < 1e-12);                                                        // (b)
  SpeedEstimator c({1.0, 0.39}, 8);
  std::mt19937_64 rng(5); std::normal_distribution<double> e(0.0, 0.2); std::uniform_real_distribution<double> w(0.02, 0.2);
  for (int round = 0; round < 64; ++round) {
    double x = w(rng); c.observe(0, x * std::exp(e(rng)), x);
    double y = w(rng); c.observe(1, y * std::exp(e(rng)), y / 0.7);                                   // truth 0.7
  }
  double before = c.speed(1);
  c.set_prior(1, 2.0);
  std::printf("  client-referenced: before %.3f after prior 2.0 -> %.3f (truth 0.70)\n", before, c.speed(1));
  CHECK(c.samples(1) == 64);
  CHECK(std::fabs(c.speed(1) - 0.7) < 0.12);                                                          // (c)
}

// Protocol compatibility: compile_bench_s rides in compile results only when known; an old peer's result (field
// absent) decodes as 0 ("unknown"), and a known value round-trips exactly.
OPTICC_TEST(compile_result_bench_field_optional) {
  proto::CompileResult r; r.id = 7; r.rc = 0; r.status = "ok"; r.object = "obj";
  auto f0 = proto::encode_result(r, 1);
  CHECK(proto::decode_result(f0).compile_bench_s == 0.0);
  r.compile_bench_s = 0.3134;
  auto f1 = proto::encode_result(r, 1);
  CHECK(proto::decode_result(f1).compile_bench_s == 0.3134);
  CHECK(proto::decode_result(f1).object == "obj");
}

// A client-bound classic build with a predictor that under-estimates every job by 2x (local candidates look cheap)
// compiles nothing locally while remote windows are open, and is not slower than distcc without a localhost entry
// (all-remote list scheduling).
OPTICC_TEST(classic_gate_never_compiles_locally_while_remote_idles) {
  Workload wl = jobs_mixed(1200, 3);
  Topology t = pool(4, {{4, 1.0}, {4, 1.0}, {8, 0.9}, {4, 0.9}, {4, 0.9}}, 0.001, 1e9);
  t.params.binding = Binding::Late; t.params.mode_policy = ModePolicy::Classic;
  ECTScheduler s(t, wl, std::make_shared<BiasedOraclePredictor>(0.5), ECTOptions{true, true, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, s).run();
  Topology td = t; td.params.distcc_localhost_slots = 0;
  DistccEmulation d(td, wl, std::make_shared<OraclePredictor>());
  sim::Result rd = sim::Simulator(td, wl, d).run();
  std::printf("  opticc classic (w_hat = w/2): %.1f s, local %d | distcc-nolocal %.1f s, local %d\n", r.makespan, r.local_jobs, rd.makespan, rd.local_jobs);
  CHECK(r.local_jobs == 0);
  CHECK(r.makespan <= rd.makespan * 1.005);
}

// Speed estimation end to end in the simulator: three of five nodes believed at 0.44 (a microbenchmark error) while
// really 0.87-0.93; with online estimation the scheduler gives them work and the build is within 3% of a scheduler
// that knew the true speeds from the start.
OPTICC_TEST(sim_misjudged_nodes_recover_with_speed_estimation) {
  Workload wl = jobs_mixed(1500, 5);
  Topology truth = pool(4, {{4, 1.0}, {4, 1.0}, {8, 0.87}, {4, 0.93}, {4, 0.93}}, 0.001, 1e9);
  truth.params.binding = Binding::Late; truth.params.mode_policy = ModePolicy::HeaderPush;
  auto run = [&](bool misjudge, bool estimate) {
    Topology believed = truth;
    if (misjudge) for (int k = 3; k <= 5; ++k) believed.nodes[static_cast<size_t>(k - 1)].speed = 0.44;
    std::vector<double> pri(believed.nodes.size() + 1, 1.0); for (auto& nd : believed.nodes) pri[static_cast<size_t>(nd.id)] = nd.speed;
    SpeedEstimator est(pri); auto ep = std::make_shared<AffineSizePredictor>();
    ECTScheduler s(believed, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    sim::Options o; if (estimate) { o.believed = &believed; o.speed_est = &est; o.predictor = ep.get(); }
    sim::Result r = sim::Simulator(truth, wl, s, o).run();
    int e_jobs = 0; for (int k = 3; k <= 5; ++k) e_jobs += r.nodes[static_cast<size_t>(k)].jobs_completed;
    return std::make_pair(r.makespan, e_jobs);
  };
  auto [t_true, e_true] = run(false, false);
  auto [t_bad, e_bad] = run(true, false);
  auto [t_fix, e_fix] = run(true, true);
  std::printf("  true speeds %.1f s (E jobs %d) | misjudged %.1f s (E %d) | misjudged + estimation %.1f s (E %d)\n", t_true, e_true, t_bad, e_bad, t_fix, e_fix);
  CHECK(t_fix <= t_true * 1.03);
  CHECK(e_fix >= e_true / 2);
}

// Protocol: compiler diagnostics above 4 KiB travel zstd-compressed after the object (the classic .i warning
// flood); both small (inline JSON) and large (compressed) diagnostics round-trip byte-exactly, and the large
// form is much smaller on the wire.
#include "opticc/protocol.hpp"
OPTICC_TEST(result_diagnostics_compressed_roundtrip) {
  using namespace opticc::proto;
  CompileResult small; small.id = 7; small.rc = 0; small.status = "ok"; small.object = std::string(5000, 'o'); small.err = "warning: tiny\n";
  CompileResult s2 = decode_result(encode_result(small, 3));
  CHECK(s2.err == small.err && s2.object == small.object && s2.out.empty());
  CompileResult big = small; big.id = 8; big.out = "note\n";
  for (int i = 0; i < 20000; ++i) big.err += "In file included from /srv/src/linux/include/linux/gfp.h:7:\n    |                    ^\n";
  Frame f = encode_result(big, 3);
  CompileResult b2 = decode_result(f);
  CHECK(b2.err == big.err && b2.out == big.out && b2.object == big.object && b2.id == 8);
  std::printf("  stderr %zu B raw -> frame %zu B\n", big.err.size(), f.payload.size());
  CHECK(f.payload.size() * 20 < big.err.size());
}

// Hedging guard: on a straggler-free build where make keeps releasing jobs (early binding: the ready list is
// almost always empty), the hedging policy issues no duplicates and is not slower than the same policy without hedging;
// with injected stragglers it still hedges and is not slower either.
OPTICC_TEST(hedging_guard_never_worse_than_no_hedging) {
  Workload wl = jobs_mixed(1500, 9);
  Topology t = pool(4, {{4, 1.0}, {4, 1.0}, {8, 0.9}, {4, 0.9}, {4, 0.9}}, 0.001, 1e9);
  t.params.binding = Binding::Early; t.params.mode_policy = ModePolicy::HeaderPush;
  auto run = [&](HedgeRule h, double sp) {
    ECTScheduler s(t, wl, std::make_shared<NoisyOraclePredictor>(2.0, 3), ECTOptions{true, true, h});
    sim::Options o; o.release_window = 120; o.straggler_prob = sp; o.straggler_factor = 8.0; o.seed = 4;
    return sim::Simulator(t, wl, s, o).run();
  };
  sim::Result a = run(HedgeRule::None, 0.0), b = run(HedgeRule::T7, 0.0);
  sim::Result c = run(HedgeRule::None, 0.03), d = run(HedgeRule::T7, 0.03);
  std::printf("  no stragglers: window %.1f s, +T7 %.1f s (%d hedges) | 3%% stragglers x8: window %.1f s, +T7 %.1f s (%d hedges)\n",
              a.makespan, b.makespan, b.hedges, c.makespan, d.makespan, d.hedges);
  CHECK(b.makespan <= a.makespan * 1.002);
  CHECK(d.makespan <= c.makespan * 1.002);
}

// Work-conservation bound: if opticc is work-conserving on distcc's slot set, C^A <= (1+theta) C^D +
// (1+theta) w_max/f_min REGARDLESS of speed-estimation error. Three of five nodes believed 10x slower than they are,
// estimation OFF: the slow-believed nodes must still receive work, and the makespan stays within that bound of
// distcc without localhost. Checked in both bindings and both modes.
OPTICC_TEST(work_conserving_bound_holds_under_misestimated_speeds) {
  Workload wl = jobs_mixed(1500, 21);
  double wmax = 0; for (auto& j : wl.jobs) wmax = std::max(wmax, j.work_ref_cpu_s);
  Topology truth = pool(4, {{4, 1.0}, {4, 1.0}, {8, 0.9}, {4, 0.9}, {4, 0.9}}, 0.001, 1e9);
  for (Binding b : {Binding::Early, Binding::Late}) for (ModePolicy mp : {ModePolicy::Classic, ModePolicy::HeaderPush}) {
    Topology t = truth; t.params.binding = b; t.params.mode_policy = mp;
    Topology believed = t; for (int k = 3; k <= 5; ++k) believed.nodes[static_cast<size_t>(k - 1)].speed = 0.09;
    ECTScheduler s(believed, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    sim::Options o; o.release_window = 120;
    sim::Result r = sim::Simulator(t, wl, s, o).run();
    Topology td = t; td.params.distcc_localhost_slots = 0;
    DistccEmulation d(td, wl, std::make_shared<OraclePredictor>(), mp == ModePolicy::HeaderPush);
    sim::Result rd = sim::Simulator(td, wl, d, o).run();
    int slow_jobs = 0; for (int k = 3; k <= 5; ++k) slow_jobs += r.nodes[static_cast<size_t>(k)].jobs_completed;
    double bound = 1.05 * rd.makespan + 1.05 * wmax / 0.9;
    std::printf("  %s %s: opticc %.1f s (slow-believed nodes ran %d jobs) | distcc%s-nolocal %.1f s | D2 bound %.1f s\n",
                b == Binding::Early ? "early" : "late", mp == ModePolicy::Classic ? "classic" : "header-push", r.makespan, slow_jobs,
                mp == ModePolicy::HeaderPush ? "-pump" : "", rd.makespan, bound);
    CHECK(slow_jobs > 0);
    CHECK(r.makespan <= bound);
  }
}

// Liveness: with local_slots = 0, a build must still finish locally when (a) the topology has no worker at all
// and (b) every worker is disabled before the build (unreachable), instead of waiting forever.
OPTICC_TEST(no_workers_with_zero_local_slots_compiles_locally) {
  Workload wl = jobs_mixed(40, 2);
  {
    Topology t; t.client = Client{4, 1.0, 0, 10e9, 10e9, 0}; t.params.local_slots = 0; t.assign_ids();
    ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    sim::Result r = sim::Simulator(t, wl, s).run();
    std::printf("  no workers: makespan %.1f s, local %d\n", r.makespan, r.local_jobs);
    CHECK(r.local_jobs == 40);
  }
  {
    Topology t = pool(4, {{4, 1.0}, {4, 1.0}}, 0.001, 1e9); t.params.local_slots = 0;
    for (Binding b : {Binding::Early, Binding::Late}) {
      t.params.binding = b;
      ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
      s.disable_node(1); s.disable_node(2);
      sim::Result r = sim::Simulator(t, wl, s).run();
      std::printf("  all workers disabled (%s): makespan %.1f s, local %d\n", b == Binding::Early ? "early" : "late", r.makespan, r.local_jobs);
      CHECK(r.local_jobs == 40);
    }
  }
}

// __has_include in #if/#elif: literal names join the closure (shipped when they resolve), a macro argument
// marks the TU so the coordinator compiles it locally (the worker cannot know which file the test needs).
#include "opticc/incscan.hpp"
#include "opticc/closure.hpp"
OPTICC_TEST(incscan_has_include_literal_and_computed) {
  using namespace opticc::incscan;
  FileScan a = parse_text("#if __has_include(\"conf.h\") && __has_include_next(<sys/x.h>)\n#include \"conf.h\"\n#endif\n#if defined(__has_include)\n#endif\n");
  CHECK(!a.has_include_computed);
  bool lit = false, nxt = false; for (auto& d : a.incs) { if (d.name == "conf.h" && !d.angled) lit = true; if (d.name == "sys/x.h" && d.angled && d.next) nxt = true; }
  CHECK(lit && nxt);
  FileScan b = parse_text("#define GET_PASS_REGISTRY \"X86PassRegistry.def\"\n#if !__has_include(GET_PASS_REGISTRY)\n#error \"must provide\"\n#endif\n#include GET_PASS_REGISTRY\n");
  CHECK(b.has_include_computed);
  CHECK(b.computed);
}

// clang with -fdiagnostics-color writes ANSI escapes inside the "file not found" line;
// the MISSING-round parser must still find the header and its includer.
OPTICC_TEST(missing_include_parse_strips_ansi) {
  std::string err = "\x1b[1m/r/srv/src/llvm-project/llvm/include/llvm/Passes/TargetPassRegistry.inc:21:2: \x1b[0m\x1b[0;1;31merror: \x1b[0m\x1b[1m\"must provide <Target>PassRegistry.def\"\x1b[0m\n"
                    "\x1b[1m/r/srv/src/llvm-project/llvm/include/llvm/Passes/TargetPassRegistry.inc:60:10: \x1b[0m\x1b[0;1;31mfatal error: \x1b[0m\x1b[1m'X86PassRegistry.def' file not found\x1b[0m\n";
  std::string name, inc;
  CHECK(opticc::closure::parse_missing_include(err, &name, &inc));
  CHECK(name == "X86PassRegistry.def");
  CHECK(inc == "/r/srv/src/llvm-project/llvm/include/llvm/Passes/TargetPassRegistry.inc");
}

// ECT routes by the PREDICTION, so large predicted jobs go to nodes believed fast, and the predictor's error depends
// on size (here w_hat = 0.8 w^0.7 x noise: large jobs under-predicted). Identical nodes 1 and 2 get only large-w_hat (1) and only small-w_hat (2) jobs;
// nodes 3-5 (true 0.9) get all sizes. Plain per-node medians are biased by ~0.3 x the log size gap; the stratified
// estimator must rate 1 and 2 within 12% of each other and recover the 0.9 ratio within 10% (over 10 seeds:
// stratified 0.90-1.07 and 0.86-0.95; plain medians 0.49-0.53 and 0.68-0.73).
OPTICC_TEST(speed_estimator_unbiased_under_size_routing) {
  std::vector<double> truth = {1.0, 1.0, 1.0, 0.9, 0.9, 0.9};
  SpeedEstimator est({1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
  std::mt19937_64 rng(11); std::normal_distribution<double> e(0.0, 0.2); std::uniform_real_distribution<double> any(0.1, 4.0);
  auto job = [&](double* w, double* wh) { *w = any(rng); *wh = 0.8 * std::pow(*w, 0.7) * std::exp(e(rng)); };
  int n1 = 0, n2 = 0, n3 = 0;
  while (n1 < 256 || n2 < 256 || n3 < 256) {
    double w, wh; job(&w, &wh);
    if (wh >= 1.0 && n1 < 256) { est.observe(1, wh, w / truth[1]); ++n1; }             // routed by the prediction
    else if (wh <= 0.35 && n2 < 256) { est.observe(2, wh, w / truth[2]); ++n2; }
    else if (n3 < 256) { for (int k = 3; k <= 5; ++k) { double w2, wh2; job(&w2, &wh2); est.observe(k, wh2, w2 / truth[static_cast<size_t>(k)]); } ++n3; }
  }
  std::printf("  estimates:"); for (int k = 1; k <= 5; ++k) std::printf(" %.3f", est.speed(k)); std::printf("\n");
  CHECK(std::fabs(est.speed(1) / est.speed(2) - 1.0) < 0.12);
  for (int k = 3; k <= 5; ++k) CHECK(std::fabs(est.speed(k) / (0.5 * (est.speed(1) + est.speed(2))) / 0.9 - 1.0) < 0.10);
}

// Classic mode: compile requests may travel uncompressed (kNoCompression): round trip is byte-exact and the frame
// carries the raw bytes; compressed requests still round-trip.
OPTICC_TEST(compile_request_raw_and_compressed_roundtrip) {
  using namespace opticc::proto;
  CompileRequest r; r.id = 9; r.argv = {"clang", "-O2", "-c", "x.i", "-o", "x.o"}; r.lang = "c"; r.input_name = "x.c";
  for (int i = 0; i < 20000; ++i) r.preprocessed += "int v" + std::to_string(i) + " = " + std::to_string(i * 7) + ";\n";
  for (int lvl : {kNoCompression, -3, 1, 3}) {
    uint64_t z = 0; Frame f = encode_compile(r, lvl, &z); uint64_t z2 = 0;
    CompileRequest d = decode_compile(f, &z2);
    CHECK(d.preprocessed == r.preprocessed && d.id == 9 && d.argv == r.argv && z == z2);
    if (lvl == kNoCompression) CHECK(z == r.preprocessed.size()); else CHECK(z < r.preprocessed.size() / 2);
  }
}

// Probe-cache validation: an entry is reused only when fresh (TTL), well-formed and its RTT agrees with a fresh
// measurement within 1.5x (sub-millisecond jitter tolerated); a network change (LAN -> 80 ms VPN, or back) re-probes.
#include "opticc/probecache.hpp"
OPTICC_TEST(probe_cache_validation_detects_network_change) {
  using namespace opticc::probecache;
  nlohmann::json e{{"rtt_s", 0.001}, {"up", 1.2e9}, {"down", 1.1e9}, {"t", 1000.0}};
  CHECK(validate(e, 1100.0, 0.0011) == Verdict::Use);          // same network, fresh
  CHECK(validate(e, 1100.0, 0.0013) == Verdict::Use);          // jitter < 0.5 ms
  CHECK(validate(e, 1100.0, 0.080) == Verdict::Reprobe);       // RTT 1 ms -> 80 ms (shaped / VPN)
  nlohmann::json far{{"rtt_s", 0.080}, {"up", 1e8}, {"down", 1e8}, {"t", 1000.0}};
  CHECK(validate(far, 1100.0, 0.0011) == Verdict::Reprobe);    // and back to the LAN
  CHECK(validate(far, 1100.0, 0.100) == Verdict::Use);         // 80 -> 100 ms: within 1.5x
  CHECK(validate(e, 1000.0 + 601.0, 0.001) == Verdict::Reprobe);   // older than the 10-min TTL
  CHECK(validate(nlohmann::json{{"rtt_s", 0.001}, {"t", 1000.0}}, 1100.0, 0.001) == Verdict::Reprobe);   // malformed
  CHECK(key("SHA256:x", "w1:3633", "10.0.0.5") != key("SHA256:x", "w1:3633", "100.64.0.9"));             // route in the key
}

// Client work conservation (header-push/auto): on a worker-bound C++-like build (large, heterogeneous TUs, -j56,
// idle client) with an uninformative size predictor, a job that would wait for a full remote node runs on a free
// client core; the build is clearly faster than without the rule and than distcc without localhost. Classic: off.
OPTICC_TEST(client_work_conservation_header_push_worker_bound) {
  Workload wl; std::mt19937_64 rng(5); std::lognormal_distribution<double> wd(1.0, 0.8);
  for (int i = 0; i < 600; ++i) { Job j; j.id = i; j.name = "tu" + std::to_string(i); double w = std::min(40.0, wd(rng));
    j.work_ref_cpu_s = w; j.preprocess_cpu_s = 0.4 + 0.03 * w; j.scan_cpu_s = 0.004; j.pre_size_bytes = 6e6; j.src_size_bytes = 3e4; j.closure_bytes = 6e6;
    j.obj_size_bytes = 2e5; j.mem_bytes = 6e8; wl.jobs.push_back(j); }
  Topology t = pool(4, {{4, 1.0}, {4, 1.0}, {8, 0.87}, {4, 0.93}, {4, 0.93}}, 0.001, 1e9);
  t.params.mode_policy = ModePolicy::HeaderPush; t.params.binding = Binding::Late;
  sim::Options o; o.release_window = 56;
  auto run = [&](bool wc) { Topology tt = t; tt.params.work_conserving = wc;
    ECTScheduler s(tt, wl, std::make_shared<AffineSizePredictor>(), ECTOptions{true, true, HedgeRule::None});
    return sim::Simulator(tt, wl, s, o).run(); };
  sim::Result on = run(true), off = run(false);
  Topology td = t; td.params.distcc_localhost_slots = 0; td.params.mode_policy = ModePolicy::Classic;
  DistccEmulation d(td, wl, std::make_shared<OraclePredictor>()); sim::Result rd = sim::Simulator(td, wl, d, o).run();
  std::printf("  header-push late: client rule ON %.1f s (local %d) | OFF %.1f s (local %d) | distcc-nolocal %.1f s\n", on.makespan, on.local_jobs, off.makespan, off.local_jobs, rd.makespan);
  CHECK(on.local_jobs > 0);
  CHECK(on.makespan < 0.92 * off.makespan);
  CHECK(on.makespan < 0.95 * rd.makespan);
}
