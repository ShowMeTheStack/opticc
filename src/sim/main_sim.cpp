// opticc-sim CLI.
// SPDX-License-Identifier: GPL-2.0-only
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_set>

#include "opticc/sim.hpp"
#include "opticc/speed.hpp"

using namespace opticc;

static void usage() {
  std::fprintf(stderr,
    "usage: opticc-sim --nodes nodes.json --trace trace.json --policy distcc|distcc-pump|ect|ect-window|ect-window-hedge|ect-window-hedge-t7\n"
    "                  [--params params.json] [--seed N] [--out result.json] [--lp lp.json]\n"
    "                  [--rtt-ms X] [--bw-gbps Y] [--window-scale S] [--hedge-kappa K] [--predictor oracle|noisy|affine|biased] [--gamma G] [--pred-bias B]\n"
    "                  [--transport auto|fifo|ps] [--uplink-order auto|fifo|jackson] [--straggler-prob P] [--straggler-factor F]\n"
    "                  [--hedge-lambda-min L] [--distcc-compression lzo|none]\n"
    "                  [--mode classic|header-push|auto] [--priced on|off] [--nu X] [--binding early|late]   (T4')\n"
    "                  [--security noise|plain] [--enc-bps X]   (encryption CPU charged per shipped byte, DEEP_PLAN §6.2b)\n"
    "                  [--release all|jN] (make -j release model; default: the trace's release_window, else all)\n"
    "                  [--client-overhead X] (client CPU-s per job outside the compiler; default: the trace's value)\n"
    "                  [--localhost-slots N] (distcc localhost/N entry, default 4) [--q-pump X] (pump include-server cost per job)\n"
    "                  [--cache cold|warm|fraction:F] (opticc compile cache on workers, T12; warm = objects from a previous run\n"
    "                   of this policy, restricted to the trace's cache_clean list; fraction:F keeps a seeded F of them)\n"
    "                  [--ccache direct|preprocessor] (client-local ccache for baselines: clean jobs become local with\n"
    "                   w = q_j (direct mode) or p_j (preprocessor mode) + 2 ms; misses as usual) [--cache-hit-cost S]\n"
    "                  [--prefetch on|off|legacy] [--prefetch-frac F] [--prefetch-budget-mb M] [--header-union-mb U] [--prefetch-resid R]\n"
    "                   (T13: off = stores warm through misses + one NEED round per missing job; on = piggyback + background push)\n"
    "                  [--believed-speeds s1,s2,...] (speeds the scheduler believes per node; execution uses the nodes file)\n"
    "                  [--speed-est on|off] (v2: revise the believed speeds online from completed compiles)\n");
}

int main(int argc, char** argv) {
  std::string nodes_f, trace_f, policy = "ect", params_f, out_f, lp_f, predictor;
  double pred_bias = -1;
  uint64_t seed = 1; double rtt_ms = -1, bw_gbps = -1, window_scale = -1, kappa = -1, gamma = -1, lambda_min = -1;
  std::string distcc_comp, mode, priced, security, binding, release; double nu = -2, enc_bps = -1, overhead = -2, q_pump = -1; int localhost_slots = -1;
  std::string cache, ccache; double hit_cost = -1;
  bool speed_est_on = false;   // --speed-est on: online speed estimation from the believed priors
  std::string believed;   // comma list of per-node speeds the SCHEDULER believes (execution keeps the true speeds)
  sim::Options sopt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
    if (a == "--nodes") nodes_f = next(); else if (a == "--trace") trace_f = next();
    else if (a == "--policy") policy = next(); else if (a == "--params") params_f = next();
    else if (a == "--seed") seed = std::stoull(next()); else if (a == "--out") out_f = next();
    else if (a == "--lp") lp_f = next(); else if (a == "--rtt-ms") rtt_ms = std::stod(next());
    else if (a == "--bw-gbps") bw_gbps = std::stod(next()); else if (a == "--window-scale") window_scale = std::stod(next());
    else if (a == "--hedge-kappa") kappa = std::stod(next()); else if (a == "--predictor") predictor = next();
    else if (a == "--gamma") gamma = std::stod(next());
    else if (a == "--pred-bias") pred_bias = std::stod(next());
    else if (a == "--transport") { std::string v = next(); sopt.transport = v == "fifo" ? sim::Transport::Fifo : v == "ps" ? sim::Transport::ProcessorSharing : sim::Transport::Auto; }
    else if (a == "--uplink-order") { std::string v = next(); sopt.uplink_order = v == "fifo" ? sim::UplinkOrder::Fifo : v == "jackson" ? sim::UplinkOrder::Jackson : sim::UplinkOrder::Auto; }
    else if (a == "--hedge-lambda-min") lambda_min = std::stod(next());
    else if (a == "--distcc-compression") distcc_comp = next();
    else if (a == "--mode") mode = next(); else if (a == "--priced") priced = next(); else if (a == "--nu") nu = std::stod(next());
    else if (a == "--security") security = next(); else if (a == "--enc-bps") enc_bps = std::stod(next());
    else if (a == "--binding") binding = next();
    else if (a == "--release") release = next();
    else if (a == "--client-overhead") overhead = std::stod(next());
    else if (a == "--localhost-slots") localhost_slots = std::stoi(next());
    else if (a == "--q-pump") q_pump = std::stod(next());
    else if (a == "--cache") cache = next(); else if (a == "--ccache") ccache = next();
    else if (a == "--cache-hit-cost") hit_cost = std::stod(next());
    else if (a == "--prefetch") { std::string v = next(); sopt.prefetch = v == "on" ? 1 : (v == "off" ? 0 : -1); }
    else if (a == "--prefetch-frac") sopt.prefetch_frac = std::stod(next());
    else if (a == "--prefetch-budget-mb") sopt.prefetch_budget_bytes = std::stod(next()) * 1e6;
    else if (a == "--header-union-mb") sopt.header_union_bytes = std::stod(next()) * 1e6;
    else if (a == "--prefetch-resid") sopt.prefetch_resid = std::stod(next());
    else if (a == "--straggler-prob") sopt.straggler_prob = std::stod(next());
    else if (a == "--straggler-factor") sopt.straggler_factor = std::stod(next());
    else if (a == "--believed-speeds") believed = next();
    else if (a == "--speed-est") speed_est_on = next() == "on";
    else { usage(); return 2; }
  }
  if (nodes_f.empty() || trace_f.empty()) { usage(); return 2; }
  try {
    Topology topo = load_topology(nodes_f);
    Workload wl = load_workload(trace_f);
    json pspec = json::object();
    if (!params_f.empty()) { pspec = load_json(params_f); topo.params = pspec.get<Params>(); }
    if (rtt_ms >= 0) for (auto& n : topo.nodes) n.rtt_s = rtt_ms / 1000.0;
    if (bw_gbps >= 0) for (auto& n : topo.nodes) { n.bw_up_bps = n.bw_down_bps = bw_gbps * 1e9; }
    if (window_scale > 0) topo.params.window_scale = window_scale;
    if (kappa > 0) topo.params.hedge_kappa = kappa;
    if (lambda_min >= 0) topo.params.hedge_lambda_min = lambda_min;
    if (!distcc_comp.empty()) topo.params.distcc_compression = distcc_comp;
    if (!mode.empty()) topo.params.mode_policy = mode == "header-push" ? ModePolicy::HeaderPush : mode == "auto" ? ModePolicy::Auto : ModePolicy::Classic;
    if (!priced.empty()) topo.params.priced = priced != "off";
    if (nu > -2) topo.params.nu = nu;
    if (!security.empty()) topo.params.security = security;
    if (enc_bps > 0) topo.params.enc_bytes_per_s = enc_bps;
    if (!binding.empty()) topo.params.binding = binding == "early" ? Binding::Early : Binding::Late;
    if (!release.empty()) sopt.release_window = release == "all" ? 0 : std::stoi(release.substr(release[0] == 'j' ? 1 : 0));
    if (overhead > -2) topo.params.client_overhead_cpu_s_per_job = overhead;
    if (localhost_slots >= 0) topo.params.distcc_localhost_slots = localhost_slots;
    if (q_pump >= 0) topo.params.q_pump_s = q_pump;
    if (!predictor.empty()) pspec["predictor"] = predictor;
    if (gamma > 0) pspec["gamma"] = gamma;
    if (pred_bias > 0) { pspec["predictor"] = "biased"; pspec["bias"] = pred_bias; }
    sopt.seed = seed;
    if (hit_cost >= 0) sopt.cache_hit_cost_s = hit_cost;
    // compile-cache inputs: which jobs are unchanged since the previous build ("clean"). The trace may carry
    // cache_clean (e.g. from an incremental or repeat build trace); otherwise every compile job is clean.
    std::vector<int> clean;
    {
      json raw = load_json(trace_f);
      if (raw.contains("cache_clean")) clean = raw["cache_clean"].get<std::vector<int>>();
      else for (auto& j : wl.jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) clean.push_back(j.id);
      if (cache.rfind("fraction:", 0) == 0) {
        double f = std::stod(cache.substr(9)); std::mt19937_64 rng(seed ^ 0x5eedcaceULL);
        std::shuffle(clean.begin(), clean.end(), rng);
        clean.resize(static_cast<size_t>(std::max(0.0, std::min(1.0, f)) * static_cast<double>(clean.size())));
      }
    }
    if (!ccache.empty()) {   // client-local ccache: clean jobs are answered on the client after hashing
      std::unordered_set<int> cs(clean.begin(), clean.end());
      for (auto& j : wl.jobs) if (cs.count(j.id)) {
        j.work_ref_cpu_s = (ccache == "preprocessor" ? j.preprocess_cpu_s : j.scan_cpu_s) + 0.002;
        j.pre_size_bytes = 0;   // non-distributable: runs on the client
      }
    }
    auto pred = make_predictor(pspec, seed);
    if (cache == "warm" || cache.rfind("fraction:", 0) == 0) {
      // previous build: same policy, cold cache; each object then lives on the node that compiled it
      auto pred0 = make_predictor(pspec, seed);
      auto sched0 = make_scheduler(policy, topo, wl, pred0);
      sim::Options o0 = sopt; o0.cached_on.clear(); o0.compute_bounds = false;
      sim::Result r0 = sim::Simulator(topo, wl, *sched0, o0).run();
      sopt.cached_on.assign(wl.jobs.size(), -1);
      for (int jid : clean) if (jid >= 0 && static_cast<size_t>(jid) < wl.jobs.size()) sopt.cached_on[static_cast<size_t>(jid)] = r0.job_node[static_cast<size_t>(jid)];
    }
    // --believed-speeds: the scheduler sees a copy of the topology with mis-estimated node speeds (e.g. a bad startup
    // benchmark that rates efficiency-core workers at half their real speed); the simulator executes with the true ones.
    Topology topo_b = topo;
    if (!believed.empty()) {
      size_t pos = 0; int k = 0;
      while (pos <= believed.size() && k < static_cast<int>(topo_b.nodes.size())) {
        size_t c = believed.find(',', pos); std::string tok = believed.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        if (!tok.empty()) topo_b.nodes[static_cast<size_t>(k)].speed = std::stod(tok);
        ++k; if (c == std::string::npos) break; pos = c + 1;
      }
    }
    std::unique_ptr<SpeedEstimator> est; std::shared_ptr<CostPredictor> est_pred;
    bool is_opticc = policy.rfind("ect", 0) == 0;
    if (speed_est_on && is_opticc) {
      std::vector<double> pri(topo_b.nodes.size() + 1, 1.0);
      for (auto& nd : topo_b.nodes) pri[static_cast<size_t>(nd.id)] = nd.speed;
      est = std::make_unique<SpeedEstimator>(pri);
      // at completion the coordinator knows the job's real preprocessed size (classic) or closure (header-push), so
      // the estimator's w_hat comes from a size-aware affine predictor trained online, as in the coordinator
      est_pred = make_predictor(json{{"predictor", "affine"}}, seed);
      sopt.believed = &topo_b; sopt.speed_est = est.get(); sopt.predictor = est_pred.get();
    }
    auto sched = make_scheduler(policy, (believed.empty() && !est) ? topo : topo_b, wl, pred);
    sim::Simulator s(topo, wl, *sched, sopt);
    sim::Result r = s.run();
    if (!lp_f.empty()) {
      BoundSpec bs = bound_spec_from_params(topo, wl);
      bs.mode_policy = sched->bound_mode_policy(); bs.alpha = sched->alpha(); bs.rho = sched->rho(); bs.per_node_store = sched->per_node_store();
      bs.overhead_cpu_s = sched->overhead(); if (!sched->per_node_store()) bs.q_override = topo.params.q_pump_s;
      export_lp(topo, wl, lp_f, bs);
    }
    json out = r.to_json();
    out["seed"] = seed; out["predictor"] = pred->name(); out["topology"] = topo;
    if (est) { json bs = json::array(); for (auto& nd : topo_b.nodes) bs.push_back({{"node", nd.name}, {"believed_final", nd.speed}, {"true", topo.node(nd.id).speed}, {"samples", est->samples(nd.id)}}); out["speed_estimation"] = bs; }
    out["cache"] = cache.empty() ? "off" : cache; out["ccache"] = ccache.empty() ? "off" : ccache; out["cache_clean_jobs"] = clean.size();
    if (!out_f.empty()) save_json(out_f, out);
    std::printf("policy=%s makespan=%.1fs T_LB=%.1fs eff=%.3f remote=%d(hp %d) local=%d up=%.1fMB down=%.1fMB dup=%.3f hedges=%d/%d stragglers=%d nu=%.3g(peak %.3g, %d upd, %d flips) release=%d ovh=%.1fs cost=$%.2f\n",
                r.policy.c_str(), r.makespan, r.lb.t_lb, r.efficiency, r.remote_jobs, r.header_push_jobs, r.local_jobs,
                r.bytes_up_total / 1e6, r.bytes_down_total / 1e6, r.duplicated_work_frac, r.hedges_won, r.hedges, r.stragglers_injected,
                sched->nu(), r.nu_peak, r.nu_updates, r.nu_direction_changes, r.release_window, r.client_overhead_s, r.cost_usd);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
