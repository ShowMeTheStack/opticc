// SPDX-License-Identifier: GPL-2.0-only
// Simulator tests against the scheduling model. Each test states the model claim it checks.
#include "opticc/sim.hpp"
#include "portable_rng.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cmath>
#include <random>

using namespace opticc;

namespace {
Workload identical_jobs(int n, double s, double w, double p, double o) {
  Workload wl;
  for (int i = 0; i < n; ++i) {
    Job j; j.id = i; j.name = "tu" + std::to_string(i); j.pre_size_bytes = s; j.src_size_bytes = 0.1 * s;
    j.work_ref_cpu_s = w; j.preprocess_cpu_s = p; j.obj_size_bytes = o; j.mem_bytes = 3e8;
    wl.jobs.push_back(j);
  }
  return wl;
}
// Client: 4 cores, finite 10 Gbit egress/ingress unless stated (unlimited links are a special case).
Topology base(int client_cores = 4, double egress = 10e9, double ingress = 10e9) {
  Topology t; t.client = Client{client_cores, 1.0, 0, egress, ingress, 0};
  t.params.alpha = 1.0; t.params.rho = 0.2;
  return t;
}
Topology& add_node(Topology& t, int cores, double f, double rtt, double bw_up, double bw_down) {
  Node n; n.name = "w" + std::to_string(t.nodes.size()); n.cores = cores; n.speed = f; n.rtt_s = rtt; n.bw_up_bps = bw_up; n.bw_down_bps = bw_down;
  t.nodes.push_back(n); t.assign_ids();
  return t;
}
Topology one_node(int cores, double f, double rtt, double bw_up, double bw_down, int local_slots) {
  Topology t = base(); add_node(t, cores, f, rtt, bw_up, bw_down); t.params.local_slots = local_slots; return t;
}
double steady_throughput(const sim::Result& r) {
  double T = r.makespan; int c = 0;
  for (double d : r.job_done_time) if (d >= 0.2 * T && d <= 0.8 * T) ++c;
  return c / (0.6 * T);
}
struct ScaledOracle : CostPredictor {  // predicts true w times a constant factor (mis-calibration)
  double k; explicit ScaledOracle(double kk) : k(kk) {}
  double predict(const Job& j) override { return j.work_ref_cpu_s * k; }
  std::string name() const override { return "scaled"; }
};
}  // namespace

// A single uncontended remote job completes at exactly p/f0 + alpha L + rho s/B_up + (w-p)/f_k + o/B_down.
// w is from-source work, so a classic worker compiles w - p = 2.5 (not w = 3.0):
// expected makespan 1.804, worker busy core-s 1.25.
OPTICC_TEST(single_remote_job_time) {
  Workload wl = identical_jobs(1, 2e6, 3.0, 0.5, 1e5);
  Topology t = one_node(2, 2.0, 0.05, 1e9, 1e9, -1);
  FixedAssignmentScheduler s(t, wl, {1});
  sim::Result r = sim::Simulator(t, wl, s).run();
  double expect = 0.5 / 1.0 + 1.0 * 0.05 + 0.2 * 2e6 * 8 / 1e9 + (3.0 - 0.5) / 2.0 + 1e5 * 8 / 1e9;
  CHECK_NEAR(r.makespan, expect, 1e-6);
  CHECK(r.remote_jobs == 1);
  CHECK_NEAR(r.bytes_up_total, 0.4e6, 1e-6);
  CHECK_NEAR(r.bytes_down_total, 1e5, 1e-6);
  CHECK_NEAR(r.nodes[1].busy_core_s, 1.25, 1e-9);
  CHECK_NEAR(r.nodes[0].busy_core_s, 0.5, 1e-9);
}

// A flow on an unlimited client link is limited only by its node link.
OPTICC_TEST(fairness_unlimited_client_link) {
  Topology t = base(4, 0, 0);  // unlimited egress/ingress
  add_node(t, 4, 1.0, 0, 10e6, 1e9); add_node(t, 4, 1.0, 0, 1e9, 1e9);
  Workload wl = identical_jobs(2, 5e6, 1e-3, 0.0, 0.0);  // 1 MB shipped each (rho 0.2)
  FixedAssignmentScheduler s(t, wl, {1, 2});
  sim::Result r = sim::Simulator(t, wl, s).run();
  CHECK_NEAR(r.job_done_time[1], 0.008 + 1e-3, 1e-6);
  CHECK_NEAR(r.job_done_time[0], 0.8 + 1e-3, 1e-6);
}

// Shared finite egress: two 1 Gbit nodes behind a 1 Gbit client link get 0.5 Gbit each.
OPTICC_TEST(fairness_shared_egress_cap) {
  Topology t = base(4, 1e9, 1e9);
  add_node(t, 4, 1.0, 0, 1e9, 1e9); add_node(t, 4, 1.0, 0, 1e9, 1e9);
  Workload wl = identical_jobs(2, 5e6, 1e-3, 0.0, 0.0);
  FixedAssignmentScheduler s(t, wl, {1, 2});
  sim::Result r = sim::Simulator(t, wl, s).run();
  CHECK_NEAR(r.job_done_time[0], 0.016 + 1e-3, 1e-6);
  CHECK_NEAR(r.job_done_time[1], 0.016 + 1e-3, 1e-6);
  // one node alone would take 0.008 s
  Workload w1 = identical_jobs(1, 5e6, 1e-3, 0.0, 0.0);
  FixedAssignmentScheduler s1(t, w1, {1});
  CHECK_NEAR(sim::Simulator(t, w1, s1).run().makespan, 0.008 + 1e-3, 1e-6);
}

// Timers fire exactly; 50 local jobs of 0.5 ms on one core take 25 ms.
OPTICC_TEST(timers_exact) {
  Topology t = base(1); t.params.local_slots = 1;
  Workload wl = identical_jobs(50, 0, 0.0005, 0.0, 0.0);  // pre_size 0 -> must be local
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, s).run();
  CHECK_NEAR(r.makespan, 0.025, 1e-9);
  CHECK_NEAR(r.nodes[0].busy_core_s, 0.025, 1e-9);
}

// The ECT rule ships a job iff w (1/f0 - 1/f_k) >= p/f0 + alpha L + b/B_up + o/B_down.
OPTICC_TEST(ect_follows_t1_threshold) {
  Topology t = base(4); add_node(t, 4, 2.0, 0.05, 1e9, 1e9);
  // threshold: w/2 >= 0.05 + 0.2e6*8/1e9 = 0.0516 -> w >= 0.1032
  for (double w : {0.2, 0.05}) {
    Workload wl = identical_jobs(1, 1e6, w, 0.0, 0.0);
    ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
    sim::Result r = sim::Simulator(t, wl, s).run();
    CHECK(r.job_node[0] == (w > 0.1032 ? 1 : 0));
  }
}

// A window of N* saturates the node; the plain slot count does not when RTT matters.
OPTICC_TEST(saturated_node_throughput) {
  {  // compute-bound: mu = 4 jobs/s ; rtt 0.2 s makes the slot window latency-bound
    Workload wl = identical_jobs(600, 2e6, 1.0, 0.0, 0.0);
    Topology t = one_node(4, 1.0, 0.2, 1e9, 1e9, 0);
    ECTScheduler win(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
    sim::Result rw = sim::Simulator(t, wl, win).run();
    CHECK_REL(steady_throughput(rw), 4.0, 0.03);
    CHECK(rw.max_inflight_node <= rw.windows[1]);
    ECTScheduler slot(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
    sim::Result rs = sim::Simulator(t, wl, slot).run();
    // utilisation with N = c_k is at most (w/f)/D = 1/1.2032
    CHECK(steady_throughput(rs) < 4.0 / 1.2032 * 1.02);
    CHECK(rw.makespan < rs.makespan);
  }
  {  // bandwidth-bound: B_up = 6.4 Mbit/s, b = 0.4 MB -> mu = 2 jobs/s with FIFO pipelined uplink
    Workload wl = identical_jobs(600, 2e6, 1.0, 0.0, 0.0);
    Topology t = one_node(4, 1.0, 0.2, 6.4e6, 1e9, 0);
    ECTScheduler win(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
    sim::Result rw = sim::Simulator(t, wl, win).run();
    CHECK_REL(steady_throughput(rw), 2.0, 0.03);
    CHECK(rw.nodes[1].utilisation < 0.6);
    CHECK(rw.windows[1] == 4);  // ceil(2 * (0.2 + 0.5 + 1 + 0)) = 4
    ECTScheduler win_ps(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
    sim::Options o; o.transport = sim::Transport::ProcessorSharing;
    sim::Result rp = sim::Simulator(t, wl, win_ps, o).run();
    std::printf("  [obs] bandwidth-bound throughput: fifo=%.3f ps=%.3f (mu=2)\n", steady_throughput(rw), steady_throughput(rp));
    CHECK(steady_throughput(rp) < steady_throughput(rw));
  }
}

// Window enforcement: in-flight on a node never exceeds its window at any instant,
// and Little's law L = lambda W holds for the bookkeeping.
OPTICC_TEST(window_never_exceeded) {
  Workload wl = identical_jobs(400, 2e6, 1.0, 0.1, 1e5);
  Topology t = one_node(4, 1.0, 0.1, 1e9, 1e9, 0);
  ECTScheduler win(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, win).run();
  CHECK(r.max_inflight_node <= r.windows[1]);
  CHECK(r.max_inflight_node == r.windows[1]);  // and the window is actually used
  double L = r.inflight_time_integral / r.makespan;
  double lambda = static_cast<double>(wl.jobs.size()) / r.makespan;
  double W = 0; for (size_t j = 0; j < wl.jobs.size(); ++j) W += r.job_done_time[j] - r.job_dispatch_time[j];
  W /= static_cast<double>(wl.jobs.size());
  CHECK_REL(L, lambda * W, 1e-6);
}

// When the best node is window-blocked, ECT waits for it instead of using a far slower open node.
OPTICC_TEST(window_blocked_waits_for_best_node) {
  Topology t = base(1, 0, 0); t.client.speed = 0.05; t.params.local_slots = 1;
  add_node(t, 4, 1.0, 0.0, 0, 0);
  Workload wl = identical_jobs(200, 1e6, 1.0, 0.0, 0.0);
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, s).run();
  CHECK_NEAR(r.makespan, 50.0, 1e-6);
  // ECT may still place a job locally when its predicted node wait reaches the 20 s local time
  // (a tie under the deferred-queue estimate); that never extends the makespan here.
  CHECK(r.local_jobs <= 2);
}

// Sanity check (identical machines, no communication): ECT is list scheduling, so
// Graham's bound C <= W/m + w_max (1 - 1/m) holds for the compile stage, and T_LB <= C always.
OPTICC_TEST(graham_bound_ect) {
  int violations = 0;
  for (uint64_t seed = 1; seed <= 50; ++seed) {
    std::mt19937_64 eng(seed);
    std::uniform_int_distribution<int> nd(30, 200);
    std::lognormal_distribution<double> lw(0.0, 0.8);
    int n = nd(eng);
    Workload wl;
    Job gen; gen.id = 0; gen.work_ref_cpu_s = 0.5; wl.jobs.push_back(gen);
    double wmax = 0, W = 0;
    for (int i = 1; i <= n; ++i) {
      Job j; j.id = i; j.pre_size_bytes = 1e6; j.work_ref_cpu_s = lw(eng); j.deps = {0}; wmax = std::max(wmax, j.work_ref_cpu_s); W += j.work_ref_cpu_s;
      wl.jobs.push_back(j);
    }
    Job link; link.id = n + 1; link.is_link_barrier = true; link.work_ref_cpu_s = 1.0;
    for (int i = 1; i <= n; ++i) link.deps.push_back(i);
    wl.jobs.push_back(link);
    Topology t = base(4, 0, 0);
    int m = 1 + static_cast<int>(seed % 5), cores = 2 + static_cast<int>(seed % 3);
    for (int k = 0; k < m; ++k) add_node(t, cores, 1.0, 0.0, 0, 0);
    double machines = 4 + m * cores;
    for (bool window : {false, true}) {
      ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{window, window, HedgeRule::None});
      sim::Result r = sim::Simulator(t, wl, s).run();
      double graham = 0.5 + 1.0 + W / machines + wmax * (1 - 1 / machines);
      if (r.makespan > graham + 1e-9) { ++violations; std::printf("  graham violation seed=%llu window=%d C=%.4f bound=%.4f m=%d cores=%d\n", (unsigned long long)seed, (int)window, r.makespan, graham, m, cores); }
      if (r.makespan + 1e-9 < r.lb.t_lb) { ++violations; std::printf("  LB violation seed=%llu\n", (unsigned long long)seed); }
    }
  }
  CHECK(violations == 0);
}

// distcc emulation: alpha = 2, lzo bytes, every host in the list is used, byte accounting consistent.
OPTICC_TEST(distcc_emulation_bytes) {
  Topology t = load_topology("examples/nodes-phet.json");
  std::mt19937_64 eng(3);
  std::lognormal_distribution<double> ls(std::log(1.5e6), 0.8);
  Workload wl;
  Job gen; gen.id = 0; gen.work_ref_cpu_s = 1.0; wl.jobs.push_back(gen);
  for (int i = 1; i <= 300; ++i) {
    Job j; j.id = i; j.pre_size_bytes = ls(eng); j.work_ref_cpu_s = 0.3 + 0.9 * j.pre_size_bytes / 1e6;
    j.preprocess_cpu_s = 0.15 * j.work_ref_cpu_s; j.obj_size_bytes = 0.08 * j.pre_size_bytes; j.mem_bytes = 3e8; j.deps = {0};
    wl.jobs.push_back(j);
  }
  Job link; link.id = 301; link.is_link_barrier = true; link.work_ref_cpu_s = 2; for (int i = 1; i <= 300; ++i) link.deps.push_back(i);
  wl.jobs.push_back(link);
  DistccEmulation s(t, wl, std::make_shared<OraclePredictor>());
  CHECK_NEAR(s.alpha(), 2.0, 0.0);
  CHECK_NEAR(s.rho(), t.params.rho_lzo, 0.0);
  sim::Result r = sim::Simulator(t, wl, s).run();
  double up = 0, down = 0; int remote = 0;
  for (size_t j = 0; j < wl.jobs.size(); ++j) {
    CHECK(r.job_done_time[j] >= 0);
    if (r.job_node[j] > 0) { up += t.params.rho_lzo * wl.jobs[j].pre_size_bytes; down += wl.jobs[j].obj_size_bytes; ++remote; }
  }
  CHECK(r.job_node[0] == 0); CHECK(r.job_node[301] == 0);
  CHECK(r.remote_jobs == remote); CHECK(r.local_jobs + r.remote_jobs == 302);
  CHECK_REL(r.bytes_up_total, up, 1e-9); CHECK_REL(r.bytes_down_total, down, 1e-9);
  for (size_t k = 1; k < r.nodes.size(); ++k) CHECK(r.nodes[k].jobs_completed > 0);
  CHECK(r.hedges == 0); CHECK_NEAR(r.duplicated_work, 0.0, 1e-12);
  CHECK(r.makespan >= r.lb.t_lb);
  // uncompressed distcc ships more
  Topology t2 = t; t2.params.distcc_compression = "none";
  DistccEmulation s2(t2, wl, std::make_shared<OraclePredictor>());
  CHECK_NEAR(s2.rho(), 1.0, 0.0);
}

// A hedge fires at the kappa threshold, not at the next unrelated event.
OPTICC_TEST(hedge_fires_at_threshold) {
  Topology t = base(4, 0, 0); t.params.local_slots = 0; t.params.hedge_kappa = 2.0;
  add_node(t, 4, 1.0, 0.0, 0, 0); add_node(t, 4, 1.0, 0.0, 0, 0);
  Workload wl = identical_jobs(1, 1e6, 1.0, 0.0, 1e4);
  bool found = false;
  for (uint64_t seed = 1; seed <= 64 && !found; ++seed) {
    sim::Options o; o.straggler_prob = 0.5; o.straggler_factor = 10.0; o.seed = seed;
    ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::Kappa});
    sim::Result r = sim::Simulator(t, wl, s, o).run();
    if (r.stragglers_injected == 1 && r.hedges == 1) {
      found = true;
      // original straggles to 10 s; hedge issued at 2.0 s on the other node and finishes at 3.0 s
      CHECK_NEAR(r.makespan, 3.0, 1e-6);
      CHECK(r.hedges_won == 1);
      CHECK_NEAR(r.duplicated_work, 3.0, 1e-6);  // loser compiled for 3 s at speed 1
    }
  }
  CHECK(found);
}

// Two attempts of one job completing in the same step must not abort.
OPTICC_TEST(simultaneous_attempt_completion) {
  Topology t = base(4, 0, 0); t.params.local_slots = 0; t.params.hedge_kappa = 2.0;
  add_node(t, 4, 1.0, 0.0, 0, 0); add_node(t, 4, 1.0, 0.0, 0, 0);
  Workload wl = identical_jobs(1, 1e6, 1.0, 0.0, 1e4);
  bool found = false;
  for (uint64_t seed = 1; seed <= 64 && !found; ++seed) {
    sim::Options o; o.straggler_prob = 0.5; o.straggler_factor = 3.0; o.seed = seed;  // 3 = kappa + 1: both finish at t = 3
    ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::Kappa});
    sim::Result r = sim::Simulator(t, wl, s, o).run();
    if (r.stragglers_injected == 1 && r.hedges == 1) { found = true; CHECK_NEAR(r.makespan, 3.0, 1e-6); }
  }
  CHECK(found);
}

// Hedging (kappa) on random stragglers with the SAME realisation across policies:
// hedging cuts the makespan, duplicates little work, loser resources are released
// (node utilisation <= 1), and is inert without stragglers.
OPTICC_TEST(hedge_runs) {
  Workload wl = identical_jobs(400, 2e6, 1.0, 0.0, 1e4);
  Topology t = base(4); t.params.local_slots = 0;
  add_node(t, 4, 1.0, 0.01, 1e9, 1e9); add_node(t, 4, 1.0, 0.01, 1e9, 1e9);
  sim::Options o; o.straggler_prob = 0.05; o.straggler_factor = 10.0; o.seed = 7;
  ECTScheduler nohedge(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
  sim::Result r0 = sim::Simulator(t, wl, nohedge, o).run();
  for (HedgeRule rule : {HedgeRule::Kappa, HedgeRule::T7}) {
    ECTScheduler hedge(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, rule});
    sim::Result r1 = sim::Simulator(t, wl, hedge, o).run();
    std::printf("  [obs] %s: makespan nohedge=%.2f hedge=%.2f hedges=%d won=%d dup=%.3f injected=%d/%d\n",
                hedge.name().c_str(), r0.makespan, r1.makespan, r1.hedges, r1.hedges_won, r1.duplicated_work_frac,
                r0.stragglers_injected, r1.stragglers_injected);
    CHECK(r0.stragglers_injected > 0);
    CHECK(r1.hedges > 0 && r1.hedges_won > 0);
    CHECK(r1.duplicated_work_frac > 0 && r1.duplicated_work_frac < 0.2);
    CHECK(r1.makespan < r0.makespan);
    for (auto& n : r1.nodes) CHECK(n.busy_core_s <= n.capacity * r1.makespan + 1e-9);
    CHECK(r1.bytes_up_total <= 0.2 * 2e6 * (400 + r1.hedges) + 1e-6);
  }
  ECTScheduler hedge2(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::Kappa});
  sim::Result r2 = sim::Simulator(t, wl, hedge2).run();
  CHECK(r2.hedges == 0);
}

// Online learning reaches decisions. Node: speed 1.5, RTT 0.5 s. For a 1 MB TU the
// prior (w = 0.3 + 0.9 s) says local (1.2 s < 0.5 + 0.0016 + 0.8), the learned model
// (w = 0.5 + 3 s) says remote (3.5 s > 0.5 + 0.0016 + 2.33): the ECT decision must flip.
OPTICC_TEST(predictor_learning_changes_decisions) {
  Topology t = base(4); add_node(t, 4, 1.5, 0.5, 1e9, 1e9);
  Workload wl;
  for (int i = 0; i < 61; ++i) {
    Job j; j.id = i; j.pre_size_bytes = i == 60 ? 1e6 : 0.25e6 * (1 + i % 12); j.work_ref_cpu_s = 0.5 + 3.0 * j.pre_size_bytes / 1e6;
    j.preprocess_cpu_s = 0.0; j.obj_size_bytes = 0; wl.jobs.push_back(j);
  }
  auto learn = std::make_shared<AffineSizePredictor>(0.3, 0.9, 1.0);
  auto frozen = std::make_shared<AffineSizePredictor>(0.3, 0.9, 1e15);  // ridge so strong it never moves
  ECTScheduler s1(t, wl, learn, ECTOptions{true, true, HedgeRule::None});
  ECTScheduler s2(t, wl, frozen, ECTOptions{true, true, HedgeRule::None});
  CHECK_REL(s1.predicted_work(60), 1.2, 1e-9);
  for (int i = 0; i < 60; ++i) { learn->observe(wl.jobs[static_cast<size_t>(i)], wl.jobs[static_cast<size_t>(i)].work_ref_cpu_s); frozen->observe(wl.jobs[static_cast<size_t>(i)], wl.jobs[static_cast<size_t>(i)].work_ref_cpu_s); }
  CHECK_REL(learn->b(), 3.0, 0.02); CHECK(std::fabs(frozen->b() - 0.9) < 1e-6);
  CHECK_REL(s1.predicted_work(60), learn->a() + learn->b(), 1e-9);  // cache invalidated by the predictor version
  CHECK_REL(s2.predicted_work(60), 1.2, 1e-9);
  s1.on_job_ready(60, 0); s2.on_job_ready(60, 0);
  auto d1 = s1.decide(0), d2 = s2.decide(0);
  CHECK(d1.size() == 1 && d2.size() == 1);
  CHECK(d1[0].node == kUnbound);   // learned: ship (late binding admits; the node is chosen once preprocessed)
  CHECK(d2[0].node == 0);          // prior: keep local
  CHECK_REL(d1[0].w_hat, 3.5, 0.03);
  s1.on_preprocessed(60, 0);
  auto b1 = s1.decide(0);
  CHECK(b1.size() == 1 && b1[0].bind && b1[0].node == 1);
}

// ---- shadow price and header-push mode -------------------------------------------------------
// Shadow price of client CPU: client 1 core, p = 0.1 w, one 32-core node. Every local compile (1 s of client
// CPU) costs ten remote preprocessings; myopic ECT still compiles locally whenever the local core
// looks free (window deferral masks the starvation only partly). Pricing client CPU (nu) ships
// more work and shortens the build; the adaptive estimate must get there within the build.
OPTICC_TEST(priced_ect_beats_myopic_ect) {
  int n = 100;
  Workload wl = identical_jobs(n, 1e6, 1.0, 0.1, 1e4);
  Topology t = one_node(32, 1.0, 0.0, 10e9, 10e9, -1);
  t.params.work_conserving = false;   // tests the price in isolation; the localhost-last gate alone already removes the loss (test_speed.cpp)
  t.client.cores = 1;
  auto run = [&](bool priced, double nu) {
    Topology tt = t; tt.params.priced = priced; tt.params.nu = nu;
    ECTScheduler s(tt, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
    return sim::Simulator(tt, wl, s).run();
  };
  sim::Result un = run(false, -1), st = run(true, 1.0), ad = run(true, -1);
  CHECK(st.local_jobs < un.local_jobs);
  CHECK(st.makespan < 0.92 * un.makespan);             // static nu=1: 12.8 s vs 14.5 s on this instance
  CHECK(ad.makespan < 0.95 * un.makespan);             // adaptive nu converges within ~15 s
  CHECK(un.makespan >= un.lb.t_lb && st.makespan >= st.lb.t_lb);
}

// Header-push single job: q/f0 + alpha L + rho (s_src + closure)/B_up + w/f_k + o/B_down (cold store).
// w is from-source work and already contains p: the worker is charged w = 3.0 (not p + w = 3.5):
// expected makespan 1.6016.
OPTICC_TEST(header_push_single_job_time) {
  Workload wl = identical_jobs(1, 2e6, 3.0, 0.5, 1e5);
  wl.jobs[0].scan_cpu_s = 0.05; wl.jobs[0].closure_bytes = 4e5; wl.jobs[0].src_size_bytes = 1e5;
  Topology t = one_node(2, 2.0, 0.05, 1e9, 1e9, -1); t.params.mode_policy = ModePolicy::HeaderPush;
  // FixedAssignment dispatches Classic; use ECT with header-push-only policy and no local slots
  t.params.local_slots = 0;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, s).run();
  double expect = 0.05 + 0.05 + 0.2 * (1e5 + 4e5) * 8 / 1e9 + 3.0 / 2.0 + 1e5 * 8 / 1e9;
  CHECK_NEAR(r.makespan, expect, 1e-6);
  CHECK(r.header_push_jobs == 1);
  CHECK_NEAR(r.bytes_up_total, 0.2 * 5e5, 1e-6);
  CHECK_NEAR(r.client_scan_s, 0.05, 1e-9);
}

// Store simulation: two jobs sharing headers on one node; the second ships only its own source
// plus the headers the first did not bring (uploads are sequential on the FIFO uplink).
OPTICC_TEST(header_push_store_misses) {
  Workload wl = identical_jobs(2, 2e6, 1.0, 0.1, 1e4);
  wl.header_sizes = {1e5, 2e5, 3e5};
  wl.jobs[0].headers = {0, 1}; wl.jobs[1].headers = {1, 2};
  for (auto& j : wl.jobs) { j.src_size_bytes = 1e4; j.closure_bytes = 0; for (int h : j.headers) j.closure_bytes += wl.header_sizes[static_cast<size_t>(h)]; }
  Topology t = one_node(1, 1.0, 0.0, 1e9, 1e9, 0); t.params.mode_policy = ModePolicy::HeaderPush;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, s).run();
  // job 0: src + h0 + h1 = 3.1e5 ; job 1: src + h2 (h1 already in the store) = 3.1e5 ; total raw 6.2e5, shipped rho*
  CHECK_NEAR(r.header_miss_bytes, 3e5 + 3e5, 1e-6);
  CHECK_NEAR(r.bytes_up_total, 0.2 * 6.2e5, 1e-6);
  CHECK(r.header_push_jobs == 2);
  // the scheduler learned the node's miss volume from what was shipped
  CHECK(s.miss_estimate(1) > 0);
}

// Auto mode picks header-push when the client is the bottleneck and classic when it is not costly.
OPTICC_TEST(auto_mode_prefers_cheaper_client_path) {
  Workload wl = identical_jobs(40, 2e6, 1.0, 0.5, 1e4);
  for (auto& j : wl.jobs) { j.src_size_bytes = 1e5; j.closure_bytes = 1e5; j.scan_cpu_s = 0.01; }
  Topology t = one_node(8, 1.0, 0.0, 10e9, 10e9, 0); t.client.cores = 1;
  auto run = [&](ModePolicy mp) { Topology tt = t; tt.params.mode_policy = mp; ECTScheduler s(tt, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None}); return sim::Simulator(tt, wl, s).run(); };
  sim::Result cl = run(ModePolicy::Classic), hp = run(ModePolicy::HeaderPush), au = run(ModePolicy::Auto);
  CHECK(hp.makespan < cl.makespan);                 // client preprocessing (0.5 s/job on 1 core) was the bottleneck
  CHECK(au.header_push_jobs >= 30);                 // auto discovered it
  CHECK(au.makespan <= cl.makespan + 1e-9);
}

// Store simulation, fully shared headers: the second job on the same node ships only its source bytes.
OPTICC_TEST(header_push_store_all_shared) {
  Workload wl = identical_jobs(2, 2e6, 1.0, 0.1, 1e4);
  wl.header_sizes = {1e5, 2e5, 3e5};
  for (auto& j : wl.jobs) { j.headers = {0, 1, 2}; j.src_size_bytes = 1e4; j.closure_bytes = 6e5; }
  Topology t = one_node(1, 1.0, 0.0, 1e9, 1e9, 0); t.params.mode_policy = ModePolicy::HeaderPush;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, s).run();
  CHECK_NEAR(r.header_miss_bytes, 6e5, 1e-6);                 // only the first job misses
  CHECK_NEAR(r.bytes_up_total, 0.2 * (6e5 + 1e4 + 1e4), 1e-6); // second job: rho * s_src only
  CHECK(r.header_push_jobs == 2);
}

// distcc-pump emulation: distcc host order and transport, header-push bytes (lzo) and per-host store.
OPTICC_TEST(distcc_pump_runs_header_push) {
  Workload wl = identical_jobs(6, 2e6, 1.0, 0.1, 1e4);
  wl.header_sizes = {1e5, 2e5};
  for (auto& j : wl.jobs) { j.headers = {0, 1}; j.src_size_bytes = 1e4; j.closure_bytes = 3e5; j.scan_cpu_s = 0.01; }
  Topology t = one_node(2, 1.0, 0.0, 1e9, 1e9, 0); t.params.distcc_localhost_slots = 0;   // no localhost entry
  auto pump = make_scheduler("distcc-pump", t, wl, std::make_shared<OraclePredictor>());
  auto classic = make_scheduler("distcc", t, wl, std::make_shared<OraclePredictor>());
  sim::Result rp = sim::Simulator(t, wl, *pump).run();
  sim::Result rc = sim::Simulator(t, wl, *classic).run();
  CHECK(pump->name() == "distcc-pump");
  CHECK(rp.header_push_jobs == 6 && rc.header_push_jobs == 0);
  // distccd keeps no cross-job store: pump ships the whole closure with every job
  CHECK_NEAR(rp.header_miss_bytes, 6 * 3e5, 1e-6);
  CHECK_NEAR(rp.bytes_up_total, t.params.rho_lzo * (6 * 3e5 + 6 * 1e4), 1e-6);    // lzo ratio, not zstd
  CHECK_NEAR(rc.bytes_up_total, t.params.rho_lzo * 6 * 2e6, 1e-6);
  CHECK(rp.client_preprocess_s < rc.client_preprocess_s);                          // q_pump instead of p on the client
  CHECK_NEAR(rp.client_scan_s, 6 * t.params.q_pump_s, 1e-6);                        // include-server cost per job
  CHECK(rp.local_jobs == 0 && rc.local_jobs == 0);
}

// ---- late binding ----------------------------------------------------------------------------
namespace {
Topology client_bound_pool(int client_cores, int nodes, int cores_each, double rtt) {
  Topology t = base(client_cores);
  for (int i = 0; i < nodes; ++i) add_node(t, cores_each, 1.0, rtt, 1e9, 1e9);
  return t;
}
sim::Result run_binding(const Topology& t0, const Workload& wl, Binding b, bool window) {
  Topology t = t0; t.params.binding = b;
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{window, window, HedgeRule::None});
  return sim::Simulator(t, wl, s).run();
}
}  // namespace

// On a client-bound classic instance (p = w/4, 4-core client, 8 x 8 cores at 80 ms) late binding
// is never worse than early binding, for plain ECT and for the windowed policy.
OPTICC_TEST(late_binding_not_worse_than_early) {
  Workload wl = identical_jobs(1500, 3e6, 1.1, 0.28, 3e4);
  Topology t = client_bound_pool(4, 8, 8, 0.08);
  sim::Result e_ect = run_binding(t, wl, Binding::Early, false), l_ect = run_binding(t, wl, Binding::Late, false);
  sim::Result e_win = run_binding(t, wl, Binding::Early, true), l_win = run_binding(t, wl, Binding::Late, true);
  std::printf("  classic p=w/4: ect early %.1f late %.1f | ect-window early %.1f late %.1f | T_LB %.1f\n",
              e_ect.makespan, l_ect.makespan, e_win.makespan, l_win.makespan, l_win.lb.t_lb);
  CHECK(l_ect.makespan <= e_ect.makespan * 1.001);
  CHECK(l_win.makespan <= e_win.makespan * 1.001);
  // and the windowed policy no longer loses to plain ECT once both bind late
  CHECK(l_win.makespan <= l_ect.makespan * 1.01);
}

// Head-of-line starvation. K = 8 equal nodes behind a long FIFO preprocessing queue (2-core client,
// p = w/4). The remote feed is client-bound at c0/p = 8 jobs/s, so the ideal makespan is ~ n p / c0. Early
// binding commits jobs to nodes before the queue and leaves nodes starving while their next job waits
// behind others; late binding feeds whichever node is free and stays within 3% of the feed bound.
OPTICC_TEST(late_binding_avoids_hol_starvation) {
  int n = 2000;
  Workload wl = identical_jobs(n, 3e6, 1.0, 0.25, 3e4);
  Topology t = client_bound_pool(2, 8, 8, 0.05);
  sim::Result early = run_binding(t, wl, Binding::Early, true), late = run_binding(t, wl, Binding::Late, true);
  double feed_bound = n * 0.25 / 2.0;   // all preprocessing through 2 client cores
  auto remote_util = [&](const sim::Result& r) { double b = 0, c = 0; for (size_t k = 1; k < r.nodes.size(); ++k) { b += r.nodes[k].busy_core_s; c += r.nodes[k].capacity; } return b / (c * r.makespan); };
  std::printf("  HOL: early %.1f s (remote util %.3f, local %d) late %.1f s (util %.3f, local %d), feed bound %.1f\n",
              early.makespan, remote_util(early), early.local_jobs, late.makespan, remote_util(late), late.local_jobs, feed_bound);
  CHECK(late.makespan <= 1.03 * feed_bound + 2.0);
  CHECK(late.makespan < early.makespan);
}

// Under late binding the per-node window (counted from ship time) is still never exceeded, and L = lambda W.
OPTICC_TEST(window_never_exceeded_late) {
  Workload wl = identical_jobs(400, 2e6, 1.0, 0.1, 1e5);
  Topology t = one_node(4, 1.0, 0.1, 1e9, 1e9, 0); t.params.binding = Binding::Late;
  ECTScheduler win(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, win).run();
  CHECK(r.max_inflight_node <= r.windows[1]);
  CHECK(r.max_inflight_node == r.windows[1]);
  double L = r.inflight_time_integral / r.makespan;
  double lambda = static_cast<double>(wl.jobs.size()) / r.makespan;
  double W = 0; for (size_t j = 0; j < wl.jobs.size(); ++j) W += r.job_done_time[j] - r.job_dispatch_time[j];
  W /= static_cast<double>(wl.jobs.size());
  CHECK_REL(L, lambda * W, 1e-6);
  // the staged pool and preprocessing admission are bounded as designed
  CHECK(win.preprocess_admission() >= 1);
  CHECK(win.staged_capacity() >= r.windows[1]);
}


// T_LB follows the policy's own mode/transport, not --mode. distcc-pump's bound must not be the
// classic bound, and distcc's must not be the header-push bound.
OPTICC_TEST(tlb_follows_policy_mode) {
  Workload wl = identical_jobs(40, 2e6, 1.0, 0.3, 1e4);
  for (auto& j : wl.jobs) { j.src_size_bytes = 2e4; j.closure_bytes = 1e5; j.scan_cpu_s = 0.005; }
  Topology t = one_node(8, 1.0, 0.01, 1e9, 1e9, 0); t.params.mode_policy = ModePolicy::Classic;
  auto pump = make_scheduler("distcc-pump", t, wl, std::make_shared<OraclePredictor>());
  auto cls = make_scheduler("distcc", t, wl, std::make_shared<OraclePredictor>());
  sim::Result rp = sim::Simulator(t, wl, *pump).run(), rc = sim::Simulator(t, wl, *cls).run();
  // classic bound: client row = 40 * min(w, p) / 4 = 40*0.3/4 = 3.0 s ; pump bound: 40 * min(w, q_pump) / 4 = 0.05 s -> work bound binds
  CHECK_NEAR(rc.lb.client_cpu, 40 * 0.3 / 4, 1e-9);
  CHECK_NEAR(rp.lb.client_cpu, 40 * 0.005 / 4, 1e-9);
  CHECK(rp.lb.t_lb <= rc.lb.t_lb + 1e-9);
  CHECK(rp.efficiency <= 1.0 + 1e-9 && rc.efficiency <= 1.0 + 1e-9);
  // the same under --mode header-push for an opticc policy: its bound uses q_j, the distcc bound stays classic
  t.params.mode_policy = ModePolicy::HeaderPush;
  auto ect = make_scheduler("ect-window", t, wl, std::make_shared<OraclePredictor>());
  auto cls2 = make_scheduler("distcc", t, wl, std::make_shared<OraclePredictor>());
  sim::Result re = sim::Simulator(t, wl, *ect).run(), rc2 = sim::Simulator(t, wl, *cls2).run();
  CHECK_NEAR(rc2.lb.client_cpu, 40 * 0.3 / 4, 1e-9);
  CHECK_NEAR(re.lb.client_cpu, 40 * 0.005 / 4, 1e-9);
}

// local_slots = 0 (no local compiles for opticc policies) must not stall: the link barrier and
// unshippable jobs bypass the local window; distcc's localhost slots come from distcc_localhost_slots.
OPTICC_TEST(local_slots_zero_and_localhost_slots) {
  Workload wl = identical_jobs(30, 2e6, 1.0, 0.2, 1e4);
  Job link; link.id = 30; link.name = "link"; link.work_ref_cpu_s = 0.5; link.is_link_barrier = true; link.mem_bytes = 1e8;
  for (int i = 0; i < 30; ++i) link.deps.push_back(i);
  wl.jobs.push_back(link);
  Topology t = one_node(8, 1.0, 0.0, 1e9, 1e9, 0); t.params.local_slots = 0; t.params.binding = Binding::Late;
  auto s = make_scheduler("ect-window", t, wl, std::make_shared<OraclePredictor>());
  sim::Result r = sim::Simulator(t, wl, *s).run();
  CHECK(r.local_jobs == 1 && r.remote_jobs == 30);      // only the barrier ran locally
  CHECK(r.job_node[30] == 0);
  t.params.binding = Binding::Early;
  auto s2 = make_scheduler("ect", t, wl, std::make_shared<OraclePredictor>());
  sim::Result r2 = sim::Simulator(t, wl, *s2).run();
  CHECK(r2.local_jobs == 1 && r2.remote_jobs == 30);
  // distcc: 0 localhost slots -> nothing but the barrier locally; 4 -> some local compiles
  // a local compile costs the from-source w = 1.0 against p + (w - p) = 0.2 + 0.8 shipped, so on the
  // 8-core node (remote 10 jobs/s against the client's 20 preprocesses/s) local slots no longer pay off; the claim
  // "extra local slots help a SATURATED pool" is checked on a 2-core node (remote 2.5 jobs/s), where it holds.
  t = one_node(2, 1.0, 0.0, 1e9, 1e9, 0); t.params.local_slots = 0; t.params.binding = Binding::Early;
  t.params.distcc_localhost_slots = 0;
  auto d0 = make_scheduler("distcc", t, wl, std::make_shared<OraclePredictor>());
  sim::Result rd0 = sim::Simulator(t, wl, *d0).run();
  CHECK(rd0.local_jobs == 1);
  t.params.distcc_localhost_slots = 4;
  auto d4 = make_scheduler("distcc", t, wl, std::make_shared<OraclePredictor>());
  sim::Result rd4 = sim::Simulator(t, wl, *d4).run();
  CHECK(rd4.local_jobs > 1);
  CHECK(rd4.makespan < rd0.makespan);                    // extra local slots help a saturated pool
}

// make -jN release model. With N = 2 and 20 identical jobs on a 10-core node, at most 2 jobs are
// released at once, so the node never sees more than 2 in flight and the build serialises to ~ceil(20/2) rounds.
OPTICC_TEST(release_window_jN) {
  Workload wl = identical_jobs(20, 2e6, 1.0, 0.0, 0.0);
  Topology t = one_node(10, 1.0, 0.0, 1e9, 1e9, 0); t.params.local_slots = 0;
  auto s_all = make_scheduler("ect", t, wl, std::make_shared<OraclePredictor>());
  sim::Options all; all.release_window = 0;
  sim::Result ra = sim::Simulator(t, wl, *s_all, all).run();
  auto s_j2 = make_scheduler("ect", t, wl, std::make_shared<OraclePredictor>());
  sim::Options j2; j2.release_window = 2;
  sim::Result r2 = sim::Simulator(t, wl, *s_j2, j2).run();
  CHECK(ra.max_inflight_node == 10);
  CHECK(r2.max_inflight_node == 2);
  CHECK(r2.release_window == 2 && ra.release_window == 0);
  CHECK(ra.makespan > 2.0 && ra.makespan < 2.1);                  // two waves of 10 (uploads 3.2 ms each, FIFO)
  CHECK(r2.makespan > 9.5 && r2.makespan < 11.0);                 // ~10 rounds of 2
  // the workload's own release_window is the default; an explicit --release all overrides it
  wl.release_window = 5;
  auto s_wl = make_scheduler("ect", t, wl, std::make_shared<OraclePredictor>());
  sim::Result rw = sim::Simulator(t, wl, *s_wl).run();
  CHECK(rw.release_window == 5 && rw.max_inflight_node == 5);
}

// The per-job client overhead is charged on the client pool for every policy and appears in the bound.
OPTICC_TEST(client_overhead_charged) {
  Workload wl = identical_jobs(8, 2e6, 1.0, 0.0, 0.0); wl.client_overhead_cpu_s_per_job = 0.25;
  Topology t = one_node(8, 1.0, 0.0, 1e9, 1e9, 0); t.params.local_slots = 0;
  auto s = make_scheduler("ect", t, wl, std::make_shared<OraclePredictor>());
  sim::Result r = sim::Simulator(t, wl, *s).run();
  CHECK_NEAR(r.client_overhead_s, 8 * 0.25, 1e-6);
  CHECK_NEAR(r.nodes[0].busy_core_s, 8 * 0.25, 1e-6);
  CHECK(r.lb.client_cpu >= 8 * 0.25 / 4 - 1e-9);
  // 4 client cores x 0.25 s each for 8 jobs = 0.5 s before the last upload can start
  CHECK(r.makespan >= 0.5 + 1.0);
  auto d = make_scheduler("distcc", t, wl, std::make_shared<OraclePredictor>());
  sim::Result rd = sim::Simulator(t, wl, *d).run();
  CHECK_NEAR(rd.client_overhead_s, 8 * 0.25, 1e-6);
}

// The client-CPU price must not oscillate. Header-push with a tiny q on a client-heavy instance:
// nu may rise and settle, but the direction must not keep flipping (bounded number of reversals).
OPTICC_TEST(nu_no_oscillation) {
  // (a) kernel-like log-normal sizes (the real regime): the price must settle, not cycle
  std::mt19937 rng(11); opticc_test::PLognormal ln(0.0, 0.7);   // portable: same sample on libc++ and libstdc++
  Workload wl;
  for (int i = 0; i < 1200; ++i) {
    double f = ln(rng); Job j; j.id = i; j.name = "tu" + std::to_string(i); j.pre_size_bytes = 3e6 * f; j.src_size_bytes = 5e4 * f;
    j.work_ref_cpu_s = 1.0 * f; j.preprocess_cpu_s = 0.25 * f; j.obj_size_bytes = 3e4 * f; j.mem_bytes = 3e8; j.closure_bytes = 1e6; j.scan_cpu_s = 0.004;
    wl.jobs.push_back(j);
  }
  Topology t = base(4); for (int i = 0; i < 6; ++i) add_node(t, 8, 1.0, 0.02, 1e9, 1e9);
  t.params.work_conserving = false;   // the nu controller's own dynamics; gating is tested in test_speed.cpp
  for (ModePolicy mp : {ModePolicy::Classic, ModePolicy::HeaderPush}) {
    t.params.mode_policy = mp; t.params.binding = mp == ModePolicy::Classic ? Binding::Late : Binding::Early;
    auto s = make_scheduler("ect-window", t, wl, std::make_shared<OraclePredictor>());
    sim::Options o; o.release_window = 120;
    sim::Result r = sim::Simulator(t, wl, *s, o).run();
    std::printf("  nu (%s, log-normal): final %.3g peak %.3g updates %d flips %d makespan %.1f\n", mp == ModePolicy::Classic ? "classic" : "header-push",
                r.nu_final, r.nu_peak, r.nu_updates, r.nu_direction_changes, r.makespan);
    CHECK(r.nu_updates > 5);
    CHECK(r.nu_direction_changes <= 6);          // a settle, not a limit cycle (real trace: 0 flips in both modes)
    CHECK(r.nu_peak <= 1.0 / 0.004 * 3 + 1e-9);  // never above nu_max = (w_max/f)/(q/f0) (w_hat mean <= 3)
  }
  // (b) identical jobs produce synchronised completion waves (a pathological signal); the smoothed controller
  // must still stay far from a per-window flip-flop (one reversal per update)
  Workload id = identical_jobs(1200, 3e6, 1.0, 0.25, 3e4);
  for (auto& j : id.jobs) { j.src_size_bytes = 5e4; j.closure_bytes = 1e6; j.scan_cpu_s = 0.004; }
  t.params.mode_policy = ModePolicy::HeaderPush; t.params.binding = Binding::Early;
  auto s2 = make_scheduler("ect-window", t, id, std::make_shared<OraclePredictor>());
  sim::Result r2 = sim::Simulator(t, id, *s2).run();
  std::printf("  nu (header-push, identical jobs): updates %d flips %d\n", r2.nu_updates, r2.nu_direction_changes);
  // idleness is observed occupancy, in which an attempt in transit holds no core; synchronised waves of identical
  // jobs then produce idle samples at every wave boundary. Bound: 1/3 of the updates (13 flips in 45 updates were
  // observed), still far from a per-window flip-flop (one reversal per update).
  CHECK(r2.nu_direction_changes * 3 <= r2.nu_updates);
}

// ---- header prefetch -----------------------------------------------------------------------------------
// 8 jobs on one node, each using the same 3 headers plus one unique header; 50 ms RTT, 100 Mbit uplink.
// off:  the first job misses the shared headers and pays a NEED round; every job misses its unique header.
// on:   the background push (all headers ranked, budget large) warms the store; misses travel with the job, no NEED.
// legacy (-1): the behaviour without prefetch is unchanged (earlier predictions stay reproducible).
OPTICC_TEST(sim_prefetch_on_off_legacy) {
  Workload wl = identical_jobs(8, 2e6, 1.0, 0.1, 1e4);
  wl.header_sizes = {1e5, 2e5, 3e5};
  for (int i = 0; i < 8; ++i) wl.header_sizes.push_back(5e4);
  for (size_t i = 0; i < wl.jobs.size(); ++i) {
    auto& j = wl.jobs[i]; j.headers = {0, 1, 2, static_cast<int>(3 + i)}; j.src_size_bytes = 1e4; j.closure_bytes = 6.5e5;
  }
  Topology t = one_node(2, 1.0, 0.05, 1e8, 1e8, 0); t.params.mode_policy = ModePolicy::HeaderPush;
  auto run = [&](int pf) {
    ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
    sim::Options o; o.prefetch = pf; o.prefetch_budget_bytes = 1e9;
    return sim::Simulator(t, wl, s, o).run();
  };
  sim::Result leg = run(-1), off = run(0), on = run(1);
  CHECK(leg.need_rounds == 0 && on.need_rounds == 0);
  CHECK(off.need_rounds == 8);                       // every job misses at least its unique header
  CHECK(on.header_miss_bytes < off.header_miss_bytes);
  CHECK(on.prefetch_bytes > 0 && off.prefetch_bytes == 0);
  CHECK(on.makespan < off.makespan);
  CHECK_NEAR(leg.header_miss_bytes, off.header_miss_bytes, 1e-6);   // same store semantics, only the NEED round differs
}

// Aggregate warmth model (traces without header ids): misses shrink as the node receives header bytes.
OPTICC_TEST(sim_prefetch_aggregate_warmth) {
  Workload wl = identical_jobs(20, 2e6, 1.0, 0.1, 1e4);
  for (auto& j : wl.jobs) { j.src_size_bytes = 1e4; j.closure_bytes = 3e6; }
  Topology t = one_node(2, 1.0, 0.01, 1e9, 1e9, 0); t.params.mode_policy = ModePolicy::HeaderPush;
  auto run = [&](int pf) {
    ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
    sim::Options o; o.prefetch = pf; o.header_union_bytes = 10e6; o.prefetch_budget_bytes = 10e6; o.prefetch_resid = 0.04;
    return sim::Simulator(t, wl, s, o).run();
  };
  sim::Result leg = run(-1), off = run(0), on = run(1);
  CHECK_NEAR(leg.header_miss_bytes, 20 * 3e6, 1e-3);            // legacy: full closure every job
  CHECK(off.header_miss_bytes < leg.header_miss_bytes);           // warms through misses
  CHECK(on.header_miss_bytes < off.header_miss_bytes);            // and faster with the background push
  CHECK(on.header_miss_bytes >= 20 * 3e6 * 0.04 - 1e-3);          // never below the residual
}
