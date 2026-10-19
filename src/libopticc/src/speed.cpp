// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/speed.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace opticc {

namespace {
double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<long>(m), v.end());
  double hi = v[m];
  if (v.size() % 2) return hi;
  double lo = *std::max_element(v.begin(), v.begin() + static_cast<long>(m));
  return 0.5 * (lo + hi);
}
}  // namespace

SpeedEstimator::SpeedEstimator(std::vector<double> priors, int min_samples, int window, double eta)
    : prior_(std::move(priors)), est_(prior_), r_(prior_.size()), min_samples_(std::max(1, min_samples)),
      window_(std::max(min_samples_, window)), eta_(std::min(1.0, std::max(0.01, eta))) {
  for (auto& p : prior_) if (!(p > 0)) p = 1.0;
  est_ = prior_;
}

void SpeedEstimator::observe(int k, double w_hat, double cpu_s) {
  if (k < 0 || static_cast<size_t>(k) >= r_.size() || !(w_hat > 0) || !(cpu_s > 1e-4)) return;   // k = 0: client (reference)
  auto& q = r_[static_cast<size_t>(k)];
  int band = static_cast<int>(std::floor(2.0 * std::log2(w_hat)));
  q.push_back({band, std::log(w_hat / cpu_s)});
  if (static_cast<int>(q.size()) > window_) q.pop_front();
  ++updates_;
  if (++since_refit_ >= 4) { since_refit_ = 0; refit(); }
}

void SpeedEstimator::set_prior(int k, double v) {
  if (k <= 0 || static_cast<size_t>(k) >= prior_.size() || !(v > 0)) return;
  size_t kk = static_cast<size_t>(k);
  prior_[kk] = v;
  if (static_cast<int>(r_[kk].size()) < min_samples_) { est_[kk] = v; return; }   // unproven: its estimate is its prior
  // established: re-fit against the new anchor. When no fit is possible (fewer than 2 established nodes, e.g. a single
  // worker under a *-nolocal policy) the estimate never left the prior, so it follows the prior.
  int established = 0;
  for (auto& q : r_) if (static_cast<int>(q.size()) >= min_samples_) ++established;
  if (established < 2) { est_[kk] = v; return; }
  refit();
}

void SpeedEstimator::refit() {
  // established nodes: >= min_samples completions
  // The client (k = 0, speed 1 by definition) takes part when it has samples of its own; it then fixes the level
  // (see the anchor below) and makes a single worker identifiable (client + 1 worker = 2 nodes)
  std::vector<size_t> est_nodes;
  for (size_t k = 0; k < r_.size(); ++k) if (static_cast<int>(r_[k].size()) >= min_samples_) est_nodes.push_back(k);
  if (est_nodes.size() < 2) return;   // one node alone carries no relative information
  // per-(node, band) medians with >= 3 samples
  struct Cell { size_t k; int b; double m; double w; };
  std::vector<Cell> cells; std::map<int, int> band_nodes;
  for (size_t k : est_nodes) {
    std::map<int, std::vector<double>> by;
    for (auto& [b, v] : r_[k]) by[b].push_back(v);
    for (auto& [b, vs] : by) if (vs.size() >= 3) { cells.push_back({k, b, median(vs), std::min(8.0, static_cast<double>(vs.size()))}); band_nodes[b]++; }   // weight capped: one dense node must not define a band
  }
  // keep bands seen on >= 2 nodes (they carry the relative information); fall back to all bands otherwise
  std::vector<Cell> use; for (auto& c : cells) if (band_nodes[c.b] >= 2) use.push_back(c);
  if (use.empty()) use = cells;
  std::map<size_t, double> a; std::map<int, double> bb;
  for (auto& c : use) { a[c.k] = 0.0; bb[c.b] = 0.0; }
  if (a.size() < 2) return;
  for (int it = 0; it < 20; ++it) {   // alternating weighted least squares: m_kb ~ a_k + c_b
    std::map<int, double> sb, wb; for (auto& c : use) { sb[c.b] += c.w * (c.m - a[c.k]); wb[c.b] += c.w; }
    for (auto& [b, v] : bb) v = sb[b] / wb[b];
    std::map<size_t, double> sa, wa; for (auto& c : use) { sa[c.k] += c.w * (c.m - bb[c.b]); wa[c.k] += c.w; }
    for (auto& [k, v] : a) v = sa[k] / wa[k];
  }
  // anchor: when the client is among the fitted nodes its effect is the level (s_0 = 1 exactly): local and remote
  // CPU times of the same predicted work are directly comparable (both are the compiler child's rusage), so no prior
  // enters. Otherwise (e.g. *-nolocal policies: no local samples): mean node effect over the fitted workers =
  // mean log-prior over the same workers.
  const bool client_ref = a.count(0) > 0;
  double mean_a = 0, mean_prior = 0;
  if (client_ref) { mean_a = a[0]; mean_prior = 0.0; }
  else {
    for (auto& [k, v] : a) { mean_a += v; mean_prior += std::log(prior_[k]); }
    mean_a /= static_cast<double>(a.size()); mean_prior /= static_cast<double>(a.size());
  }
  for (auto& [k, v] : a) {
    if (k == 0) continue;   // the client is the reference: never changes
    double target = std::exp(mean_prior + (v - mean_a));
    double cur = est_[k];
    double step = std::exp(eta_ * (std::log(target) - std::log(cur)));
    step = std::min(2.0, std::max(0.5, step));   // at most x2 per refit
    est_[k] = cur * step;
  }
}

double SpeedEstimator::speed(int k) const {
  size_t kk = static_cast<size_t>(k);
  if (k <= 0 || kk >= est_.size()) return 1.0;
  if (static_cast<int>(r_[kk].size()) >= min_samples_) return est_[kk];
  // optimism for unproven nodes: at least the median of the established nodes' speeds. An established client
  // counts (speed 1) -- with one worker there is no other established node, and a worker mis-rated low at start-up
  // (e.g. the microbenchmark fallback while its compile benchmark is still running) would otherwise never be tried
  // often enough to correct itself once client work conservation keeps the client busy.
  std::vector<double> e;
  if (!r_.empty() && static_cast<int>(r_[0].size()) >= min_samples_) e.push_back(1.0);
  for (size_t m = 1; m < r_.size(); ++m) if (static_cast<int>(r_[m].size()) >= min_samples_) e.push_back(est_[m]);
  if (e.empty()) return est_[kk];
  return std::max(est_[kk], median(e));
}

}  // namespace opticc
