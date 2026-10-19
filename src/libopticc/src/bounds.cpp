// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/bounds.hpp"

#include <algorithm>
#include <vector>

namespace opticc {

json LowerBounds::to_json() const {
  return json{{"total_work", total_work}, {"client_cpu", client_cpu},
              {"critical_path", critical_path}, {"max_single_job", max_single_job}, {"t_lb", t_lb}};
}

BoundSpec bound_spec_from_params(const Topology& t, const Workload& w) {
  BoundSpec b; b.mode_policy = t.params.mode_policy; b.alpha = t.params.alpha; b.rho = t.params.rho;
  b.overhead_cpu_s = t.params.client_overhead_cpu_s_per_job >= 0 ? t.params.client_overhead_cpu_s_per_job : w.client_overhead_cpu_s_per_job;
  return b;
}

// Modes the bound may assume (a lower bound takes the best allowed option).
static std::vector<Mode> bound_modes(const BoundSpec& b, const Job& j) {
  switch (b.mode_policy) {
    case ModePolicy::Classic: return {Mode::Classic};
    case ModePolicy::HeaderPush: return j.src_size_bytes > 0 ? std::vector<Mode>{Mode::HeaderPush} : std::vector<Mode>{Mode::Classic};
    case ModePolicy::Auto: return j.src_size_bytes > 0 ? std::vector<Mode>{Mode::Classic, Mode::HeaderPush} : std::vector<Mode>{Mode::Classic};
  }
  return {Mode::Classic};
}
static double ship_cpu(const BoundSpec& b, const Job& j, Mode m) {
  if (m == Mode::Classic) return j.preprocess_cpu_s;
  return b.q_override >= 0 ? b.q_override : j.scan_cpu_s;
}
// Client CPU a remote job must consume under the cheapest allowed mode.
static double min_client_cpu(const BoundSpec& b, const Job& j) {
  double best = j.preprocess_cpu_s;
  for (Mode m : bound_modes(b, j)) best = std::min(best, ship_cpu(b, j, m));
  return best;
}
// Miss volume the bound assumes for header-push: 0 with a per-node store (warm), the closure without one (pump).
static double bound_miss(const BoundSpec& b, const Job& j) { return b.per_node_store ? 0.0 : j.closure_bytes; }

static Params params_for(const Topology& t, const BoundSpec& b) {
  Params p = t.params; p.alpha = b.alpha; p.rho = b.rho; p.mode_policy = b.mode_policy; return p;
}

double min_job_time(const Topology& t, const Job& j, const BoundSpec& b) {
  double best = (j.work_ref_cpu_s + b.overhead_cpu_s) / t.client.speed;
  if (j.is_link_barrier) return best;
  Params p = params_for(t, b);
  for (auto& k : t.nodes)
    for (Mode m : bound_modes(b, j)) {
      Job jj = j; if (m != Mode::Classic) jj.scan_cpu_s = ship_cpu(b, j, m);
      double r = remote_time(jj, j.work_ref_cpu_s, k, t.client, p, m, m == Mode::Classic ? -1.0 : bound_miss(b, j));
      best = std::min(best, r + b.overhead_cpu_s / t.client.speed);
    }
  return best;
}
double min_job_time(const Topology& t, const Job& j) { return min_job_time(t, j, bound_spec_from_params(t, Workload{})); }

LowerBounds lower_bounds(const Topology& t, const Workload& w, const BoundSpec& b) {
  LowerBounds lb;
  double mean_m = w.mean_mem();
  double cap = t.client.cores * t.client.speed;
  for (auto& k : t.nodes) cap += t.capacity(k.id, mean_m) * k.speed;
  double n_jobs = static_cast<double>(w.jobs.size());
  // the per-job overhead is client-only work; it adds to total work and to the client row
  lb.total_work = (w.total_work() + n_jobs * b.overhead_cpu_s) / cap;

  double cc = 0;
  for (auto& j : w.jobs) cc += (j.is_link_barrier ? j.work_ref_cpu_s : std::min(j.work_ref_cpu_s, min_client_cpu(b, j))) + b.overhead_cpu_s;
  lb.client_cpu = cc / (t.client.cores * t.client.speed);

  std::vector<double> longest(w.jobs.size(), -1.0);
  std::vector<double> cost(w.jobs.size());
  for (auto& j : w.jobs) {
    cost[static_cast<size_t>(j.id)] = min_job_time(t, j, b);
    lb.max_single_job = std::max(lb.max_single_job, cost[static_cast<size_t>(j.id)]);
  }
  auto succ = w.successors();
  std::vector<int> indeg(w.jobs.size(), 0), order;
  for (auto& j : w.jobs) indeg[static_cast<size_t>(j.id)] = static_cast<int>(j.deps.size());
  std::vector<int> stack;
  for (auto& j : w.jobs) if (j.deps.empty()) stack.push_back(j.id);
  while (!stack.empty()) {
    int u = stack.back(); stack.pop_back(); order.push_back(u);
    for (int v : succ[static_cast<size_t>(u)]) if (--indeg[static_cast<size_t>(v)] == 0) stack.push_back(v);
  }
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    int u = *it; double best = 0;
    for (int v : succ[static_cast<size_t>(u)]) best = std::max(best, longest[static_cast<size_t>(v)]);
    longest[static_cast<size_t>(u)] = cost[static_cast<size_t>(u)] + best;
    lb.critical_path = std::max(lb.critical_path, longest[static_cast<size_t>(u)]);
  }
  lb.t_lb = std::max({lb.total_work, lb.client_cpu, lb.critical_path, lb.max_single_job});
  return lb;
}
LowerBounds lower_bounds(const Topology& t, const Workload& w) { return lower_bounds(t, w, bound_spec_from_params(t, w)); }

void export_lp(const Topology& t, const Workload& w, const std::string& path, const BoundSpec& b) {
  LowerBounds lb = lower_bounds(t, w, b);
  Params p = params_for(t, b);
  double mean_m = w.mean_mem();
  json nodes = json::array();
  for (auto& k : t.nodes)
    nodes.push_back({{"id", k.id}, {"cap", t.capacity(k.id, mean_m)}, {"speed", k.speed},
                     {"rtt_s", k.rtt_s}, {"bw_up_bps", k.bw_up_bps}, {"bw_down_bps", k.bw_down_bps}});
  json jobs = json::array();
  for (auto& j : w.jobs) {
    double bytes = b.mode_policy == ModePolicy::Classic ? shipped_bytes(j, Mode::Classic, p, b.rho)
                                                        : shipped_bytes(j, Mode::HeaderPush, p, b.rho, bound_miss(b, j));
    // w is from source; the remote compute row charges the least a worker does in an allowed mode (auto: the
    // classic w - p, so the row stays a lower bound)
    const Mode rm = b.mode_policy == ModePolicy::HeaderPush && j.src_size_bytes > 0 ? Mode::HeaderPush : Mode::Classic;
    jobs.push_back({{"id", j.id}, {"w", j.work_ref_cpu_s}, {"w_remote", remote_compile_work(j.work_ref_cpu_s, j.preprocess_cpu_s, rm)},
                    {"p", min_client_cpu(b, j)}, {"overhead", b.overhead_cpu_s},
                    {"b", bytes}, {"o", j.obj_size_bytes},
                    {"local_only", j.is_link_barrier}, {"min_time", min_job_time(t, j, b)}});
  }
  json out{{"client", {{"cores", t.client.cores}, {"speed", t.client.speed},
                       {"egress_bps", t.client.egress_bps}, {"ingress_bps", t.client.ingress_bps}}},
           {"params", {{"alpha", b.alpha}, {"rho", b.rho}, {"mode_policy", json(b.mode_policy)}}},
           {"nodes", nodes}, {"jobs", jobs},
           {"constants", {{"critical_path", lb.critical_path}, {"max_single_job", lb.max_single_job}}},
           {"closed_form", lb.to_json()}};
  save_json(path, out);
}
void export_lp(const Topology& t, const Workload& w, const std::string& path) { export_lp(t, w, path, bound_spec_from_params(t, w)); }

}  // namespace opticc
