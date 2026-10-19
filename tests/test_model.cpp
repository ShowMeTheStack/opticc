// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/model.hpp"
#include "opticc/predictor.hpp"
#include "opticc/sched.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <stdexcept>

using namespace opticc;

OPTICC_TEST(json_roundtrip) {
  Topology t;
  t.client = Client{4, 1.1, 8e9, 1e9, 2e9, 0.099};
  Node n; n.name = "w"; n.cores = 8; n.mem_bytes = 16e9; n.speed = 0.9; n.rtt_s = 0.02; n.bw_up_bps = 1e9; n.bw_down_bps = 2e9; n.price_per_hour = 0.132;
  t.nodes.push_back(n);
  t.params.alpha = 2; t.params.rho = 0.17; t.params.host_slots = {7};
  json j = t;
  Topology u = j.get<Topology>();
  CHECK(u.client.cores == 4); CHECK_NEAR(u.client.speed, 1.1, 1e-12); CHECK_NEAR(u.client.egress_bps, 1e9, 1e-6);
  CHECK(u.nodes.size() == 1); CHECK(u.nodes[0].id == 1); CHECK(u.nodes[0].name == "w"); CHECK(u.nodes[0].cores == 8);
  CHECK_NEAR(u.nodes[0].rtt_s, 0.02, 1e-12); CHECK_NEAR(u.params.rho, 0.17, 1e-12); CHECK(u.params.host_slots == std::vector<int>{7});
  Workload w;
  Job a; a.id = 0; a.name = "gen"; a.work_ref_cpu_s = 1.5; w.jobs.push_back(a);
  Job b; b.id = 1; b.name = "tu"; b.work_ref_cpu_s = 2.5; b.preprocess_cpu_s = 0.3; b.pre_size_bytes = 1e6; b.src_size_bytes = 1e5;
  b.obj_size_bytes = 8e4; b.mem_bytes = 4e8; b.deps = {0}; w.jobs.push_back(b);
  Job c; c.id = 2; c.name = "link"; c.is_link_barrier = true; c.deps = {1}; c.work_ref_cpu_s = 3; w.jobs.push_back(c);
  Workload w2 = json(w).get<Workload>();
  CHECK(w2.jobs.size() == 3); CHECK(w2.jobs[1].deps == std::vector<int>{0}); CHECK(w2.jobs[2].is_link_barrier);
  CHECK_NEAR(w2.jobs[1].pre_size_bytes, 1e6, 1e-9); CHECK_NEAR(w2.total_work(), 7.0, 1e-12);
  // mode is serialisable
  CHECK(json(Mode::HeaderPush) == "header_push");
}

OPTICC_TEST(noisy_oracle_gamma1) {
  Job j; j.id = 7; j.work_ref_cpu_s = 3.3; j.pre_size_bytes = 1e6;
  OraclePredictor o; NoisyOraclePredictor n1(1.0, 42), n2(2.0, 42);
  CHECK_NEAR(n1.predict(j), o.predict(j), 1e-12);
  double f = n2.predict(j) / j.work_ref_cpu_s;
  CHECK(f >= 0.5 && f <= 2.0);
  CHECK_NEAR(n2.predict(j), n2.predict(j), 0.0);  // deterministic per job
}

OPTICC_TEST(affine_predictor_learns) {
  AffineSizePredictor p(0.3, 0.9, 1.0);
  Job cold; cold.pre_size_bytes = 2e6;
  CHECK_NEAR(p.predict(cold), 0.3 + 0.9 * 2.0, 1e-9);
  for (int i = 1; i <= 60; ++i) {
    Job j; j.id = i; j.pre_size_bytes = 0.25e6 * i; j.work_ref_cpu_s = 0.5 + 2.0 * (j.pre_size_bytes / 1e6);
    p.observe(j, j.work_ref_cpu_s);
  }
  CHECK_REL(p.a(), 0.5, 0.05);
  CHECK_REL(p.b(), 2.0, 0.02);
}

// Window size: N* = ceil(mu D), mu = min(c f/w, B_up/b, B_down/o), D = p/f0 + alpha L + b/B_up + w/f + o/B_down.
// Values below are computed by hand from the model's formulas, not from the implementation.
OPTICC_TEST(window_formula) {
  auto topo = [](int cores, double bw_up, double bw_down) {
    Topology t; t.client = Client{4, 1.0, 0, 10e9, 10e9, 0};
    Node n; n.name = "w"; n.cores = cores; n.speed = 1.0; n.rtt_s = 0.2; n.bw_up_bps = bw_up; n.bw_down_bps = bw_down; t.nodes.push_back(n);
    t.assign_ids(); t.params.alpha = 1; t.params.rho = 0.2; return t;
  };
  auto jobs = [](double p) {
    Workload w;
    for (int i = 0; i < 10; ++i) { Job j; j.id = i; j.work_ref_cpu_s = 1.0; j.preprocess_cpu_s = p; j.pre_size_bytes = 2e6; j.obj_size_bytes = 1e5; w.jobs.push_back(j); }
    return w;
  };
  {  // compute-bound, early binding: mu = 4; D = 0.1 + 0.2 + 0.0032 + 1 + 0.0008 = 1.304 -> N* = 6
    Topology t = topo(4, 1e9, 1e9); Workload w = jobs(0.1); t.params.binding = Binding::Early;
    ECTScheduler s(t, w, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    CHECK(s.windows()[1] == 6);
    CHECK(s.windows()[0] == 4);
    ECTScheduler plain(t, w, std::make_shared<OraclePredictor>(), ECTOptions{false, false, HedgeRule::None});
    CHECK(plain.windows()[1] == 4);
    // late binding: the window counts from ship time, D excludes p/f0: D = 1.204 -> N* = 5;
    // N^pp = ceil(c0 + t_pp * sum mu) = ceil(4 + 0.1 * 4) = 5
    t.params.binding = Binding::Late;
    ECTScheduler late(t, w, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    CHECK(late.windows()[1] == 5);
    CHECK(late.preprocess_admission() == 5);
  }
  {  // uplink-bound: 16 cores, B_up 6.4 Mbit -> mu = 2; D = 0.2 + 0.5 + 1 + 0.0008 = 1.7008 -> N* = 4
    Topology t = topo(16, 6.4e6, 1e9); Workload w = jobs(0.0);
    t.params.work_conserving = false;   // the window formula itself; work conservation floors the window at the core count
    ECTScheduler s(t, w, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    CHECK(s.windows()[1] == 4);
    t.params.work_conserving = true;
    ECTScheduler wc(t, w, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    CHECK(wc.windows()[1] == 16);   // work conserving: max(N*, cores)
  }
  {  // downlink-bound: B_down = 0.8 Mbit, o = 1e5 -> mu = 1; D = 0.2 + 0.0032 + 1 + 1 = 2.2032 -> N* = 3
    Topology t = topo(16, 1e9, 0.8e6); Workload w = jobs(0.0);
    t.params.work_conserving = false;   // the window formula itself
    ECTScheduler s(t, w, std::make_shared<OraclePredictor>(), ECTOptions{true, true, HedgeRule::None});
    CHECK(s.windows()[1] == 3);
  }
}

// Hand-built topologies with wrong ids are rejected.
OPTICC_TEST(topology_ids_validated) {
  Topology t; t.client = Client{4, 1.0, 0, 0, 0, 0};
  Node a; a.name = "a"; Node b; b.name = "b"; t.nodes = {a, b};
  Workload w; Job j; j.id = 0; j.work_ref_cpu_s = 1; w.jobs.push_back(j);
  bool threw = false;
  try { ECTScheduler s(t, w, std::make_shared<OraclePredictor>(), ECTOptions{}); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  t.assign_ids();
  CHECK(t.nodes[0].id == 1 && t.nodes[1].id == 2);
  ECTScheduler ok(t, w, std::make_shared<OraclePredictor>(), ECTOptions{});
  CHECK(ok.windows().size() == 3);
}
