// opticc: makespan lower bounds.
// Closed-form components valid without an LP solver, plus an LP export that
// an external LP solver (e.g. HiGHS) solves for the full fluid bound T_LP.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string>

#include "opticc/model.hpp"

namespace opticc {

struct LowerBounds {
  double total_work = 0;       // sum w / sum_k ~c_k f_k (all nodes incl. client)
  double client_cpu = 0;       // sum_j min(w_j, p_j) / (c_0 f_0): compile or preprocess locally
  double critical_path = 0;    // longest DAG path with job cost min(w/f0, min_k R_jk)
  double max_single_job = 0;   // max_j min(w_j/f0, min_k R_jk)
  double t_lb = 0;             // max of the above
  json to_json() const;
};

// What the bound may assume about the policy it is compared against: the mode set
// (distcc: classic; distcc-pump: header-push with no cross-job store; opticc: Params::mode_policy), the
// transport's alpha and rho, the client cost of shipping in header-push (q_j, or pump's q_pump), and the
// per-job client overhead every policy pays.
struct BoundSpec {
  ModePolicy mode_policy = ModePolicy::Classic;
  double alpha = 1.0, rho = 0.2;
  bool per_node_store = true;      // false: the closure is shipped with every job (pump)
  double q_override = -1;          // >= 0: client CPU per shipped header-push job instead of j.scan_cpu_s
  double overhead_cpu_s = 0;       // per job, on the client
};
BoundSpec bound_spec_from_params(const Topology& t, const Workload& w);
// All bounds use the true work values in the workload. The two-argument form assumes Params (opticc transport).
LowerBounds lower_bounds(const Topology& t, const Workload& w);
LowerBounds lower_bounds(const Topology& t, const Workload& w, const BoundSpec& spec);

// Fastest possible time for job j in isolation: min(local, min_k R_jk).
double min_job_time(const Topology& t, const Job& j);
double min_job_time(const Topology& t, const Job& j, const BoundSpec& spec);

// Write the LP instance as JSON for an external LP solver.
void export_lp(const Topology& t, const Workload& w, const std::string& path);
void export_lp(const Topology& t, const Workload& w, const std::string& path, const BoundSpec& spec);

}  // namespace opticc
