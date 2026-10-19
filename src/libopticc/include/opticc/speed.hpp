// opticc: online per-node speed estimation.
// SPDX-License-Identifier: GPL-2.0-only
//
// A one-off microbenchmark can misrate a worker badly (e.g. efficiency cores rated at half their real, loaded compile
// speed), which starves its threads. The estimator below revises every node's speed from its own completed compiles.
//
// Model: a compile of job j on node k takes c_jk = w_j / s_k CPU-seconds (w_j: reference work, s_k: speed).
// The predictor supplies w_hat_j; each completion gives one sample r = log(w_hat_j / c_jk) = log s_k + e_j,
// where e_j (prediction error) has the same distribution on every node when jobs are routed independently of e_j.
// Hence differences of per-node medians estimate log(s_k / s_m) without bias, while the absolute level is
// confounded with the predictor's scale (the predictor learns w from c_jk * s_k: any common factor cancels).
// The level is therefore ANCHORED: the geometric mean of the estimates over nodes with enough samples equals the
// geometric mean of their priors (the startup calibration compile). Updates are damped (eta) and
// clamped per step (x2 at most) so a burst of outliers cannot swing a node.
// The earliest-completion rule routes LARGER jobs to nodes believed faster and the predictor's error depends on job size, so per-node medians are biased (the independence
// assumption above fails). Samples are therefore STRATIFIED by predicted work (half-octave bands of w_hat) and a
// two-way additive model log(w_hat/cpu) = node effect + band effect is fitted to the per-(node, band) medians; the
// band effects absorb the size-dependent prediction error, the node effects are the log speeds (then anchored).
// Exploration: a node with fewer than min_samples completions is reported at least at the median speed of the
// established nodes (optimism), so a node mis-rated low is still tried and its own samples then decide.
// The CLIENT is a node of the model too (k = 0, speed 1 by definition). Clean local compiles are
// observed like remote ones (compiler-child rusage, w_hat including preprocessing); when the client is established it
// fixes the level instead of the priors, so (i) a single worker is identifiable (client + worker = 2 nodes) and (ii) priors that are ALL biased (e.g. the microbenchmark fallback while workers'
// compile benchmarks are still running) are corrected instead of preserved by the anchor. An established client also
// counts for the optimism floor (speed 1). Without local samples (*-nolocal) the prior anchor applies.
#pragma once
#include <deque>
#include <vector>

namespace opticc {

class SpeedEstimator {
 public:
  // priors[k] for k = 1..n (index 0, the client, is the reference and never changes).
  explicit SpeedEstimator(std::vector<double> priors, int min_samples = 8, int window = 256, double eta = 0.3);
  // A remote compile on node k with predicted reference work w_hat took cpu_s CPU-seconds there.
  void observe(int k, double w_hat, double cpu_s);
  // Current estimate for node k (k >= 1); the prior until enough samples, raised optimistically (see above).
  double speed(int k) const;
  int samples(int k) const { return static_cast<int>(r_.at(static_cast<size_t>(k)).size()); }
  int updates() const { return updates_; }
  // A node admitted late (it was unreachable at start-up) gets its real prior once probed, as does a worker whose
  // compile benchmark arrives after start-up (its first prior was the microbenchmark). Samples are kept: an unproven
  // node (< min_samples) reports the new prior (its estimate IS its prior until established); for established nodes
  // the fit is redone at once against the new anchor (prior anchor: the level moves; client anchor: unchanged).
  void set_prior(int k, double v);
 private:
  void refit();
  std::vector<double> prior_, est_;
  std::vector<std::deque<std::pair<int, double>>> r_;   // (half-octave band of w_hat, log(w_hat / cpu))
  int min_samples_, window_; double eta_;
  int updates_ = 0, since_refit_ = 0;
};

}  // namespace opticc
