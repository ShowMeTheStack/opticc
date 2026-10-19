// opticc: compile-cost predictors.
// A predictor estimates w_j (reference-core CPU seconds) before a job runs
// and may learn from measured outcomes.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

#include "opticc/model.hpp"

namespace opticc {

class CostPredictor {
 public:
  virtual ~CostPredictor() = default;
  virtual double predict(const Job& j) = 0;
  // Called when the true work of a job becomes known (measured).
  virtual void observe(const Job& /*j*/, double /*measured_w*/) {}
  virtual std::string name() const = 0;
  // Incremented whenever observe() changed the model; callers that cache
  // predictions must re-predict when this changes.
  uint64_t version() const { return version_; }
 protected:
  void bump_version() { ++version_; }
 private:
  uint64_t version_ = 0;
};

// Returns the true w_j.
class OraclePredictor : public CostPredictor {
 public:
  double predict(const Job& j) override { return j.work_ref_cpu_s; }
  std::string name() const override { return "oracle"; }
};

// True w_j times a per-job multiplicative error e_j, log-uniform in
// [1/gamma, gamma], drawn once per job (deterministic for a given seed).
class NoisyOraclePredictor : public CostPredictor {
 public:
  NoisyOraclePredictor(double gamma, uint64_t seed) : gamma_(gamma), seed_(seed) {}
  double predict(const Job& j) override;
  std::string name() const override { return "noisy-oracle"; }
 private:
  double gamma_;
  uint64_t seed_;
  std::unordered_map<int, double> factor_;
};

// w = a + b * s_MB, fitted online by ridge regression around a prior (a0,b0).
// Cold start: returns the prior. Suitable for the real client where only
// sizes are known before compilation.
class AffineSizePredictor : public CostPredictor {
 public:
  AffineSizePredictor(double a0 = 0.3, double b0 = 0.9, double lambda = 1.0)
      : a0_(a0), b0_(b0), a_(a0), b_(b0), lambda_(lambda) {}
  double predict(const Job& j) override;
  void observe(const Job& j, double measured_w) override;
  std::string name() const override { return "affine-size"; }
  double a() const { return a_; }
  double b() const { return b_; }
 private:
  void refit();
  double a0_, b0_, a_, b_, lambda_;
  // sufficient statistics over observations (x = size in MB)
  double n_ = 0, sx_ = 0, sxx_ = 0, sy_ = 0, sxy_ = 0;
};

// Diagnostic: the coordinator decides local-vs-remote BEFORE preprocessing, when only
// the source size is known; it estimates the preprocessed size as src_size x (running mean pre/src ratio). This
// predictor reproduces that information state in the simulator: x = src_size * ratio (ratio learned online),
// w = a + b x fitted on observations. Kernel TUs have small sources and large include closures, so it under-predicts
// small-source TUs.
class AffineSrcPredictor : public CostPredictor {
 public:
  AffineSrcPredictor(double a0 = 0.3, double b0 = 0.9, double lambda = 1.0) : inner_(a0, b0, lambda) {}
  double predict(const Job& j) override { return inner_.predict(as_src(j)); }
  void observe(const Job& j, double measured_w) override {
    if (j.src_size_bytes > 0 && j.pre_size_bytes > 0) { rn_ += 1; ratio_ += (j.pre_size_bytes / j.src_size_bytes - ratio_) / rn_; }
    inner_.observe(as_src(j), measured_w); bump_version();
  }
  std::string name() const override { return "affine-src"; }
 private:
  Job as_src(const Job& j) const { Job x = j; if (j.src_size_bytes > 0) x.pre_size_bytes = j.src_size_bytes * ratio_; return x; }
  AffineSizePredictor inner_;
  double ratio_ = 30.0, rn_ = 0;
};

// Prediction-noise injection for the real tool: wraps any predictor and multiplies each
// prediction by a per-job factor, log-uniform in [1/gamma, gamma], seeded from a hash of the job name
// and `seed` (so the same job gets the same factor across runs and policies). Observations pass through.
// Oracle scaled by a constant factor: reproduces a systematically biased work measurement for ablation in the
// simulator.
class BiasedOraclePredictor : public CostPredictor {
 public:
  explicit BiasedOraclePredictor(double bias) : bias_(bias) {}
  double predict(const Job& j) override { return j.work_ref_cpu_s * bias_; }
  void observe(const Job&, double) override {}
  std::string name() const override { return "oracle*bias"; }
 private:
  double bias_;
};

class GammaNoisePredictor : public CostPredictor {
 public:
  GammaNoisePredictor(std::shared_ptr<CostPredictor> base, double gamma, uint64_t seed) : base_(std::move(base)), gamma_(gamma), seed_(seed) {}
  double predict(const Job& j) override;
  void observe(const Job& j, double measured_w) override;
  std::string name() const override { return base_->name() + "+gamma"; }
  double gamma() const { return gamma_; }
  double factor(const Job& j) const;
 private:
  void sync() { if (base_->version() != base_ver_) { base_ver_ = base_->version(); bump_version(); } }
  std::shared_ptr<CostPredictor> base_;
  double gamma_;
  uint64_t seed_;
  uint64_t base_ver_ = 0;
};

// Per-TU compile times persisted across builds (the idea of ninja's .ninja_log). A job whose name
// (the compiler's source argument) was compiled before is predicted at the recorded time (EMA, weight 1/2 for the
// newest); unknown names fall through to the base predictor. Only real compiles are recorded (the caller's choice: never
// cache hits). The coordinator keeps the table in the client cache's state directory, so it follows that cache's life
// (a cache-off run neither reads nor writes it). Observations pass through to the base predictor unchanged.
class HistoryPredictor : public CostPredictor {
 public:
  explicit HistoryPredictor(std::shared_ptr<CostPredictor> base) : base_(std::move(base)) {}
  double predict(const Job& j) override;
  void observe(const Job& j, double measured_w) override { base_->observe(j, measured_w); sync(); }
  std::string name() const override { return base_->name(); }
  void record(const std::string& name, double w);
  size_t load(const std::string& path);           // returns the number of entries read; a missing file is empty
  bool save(const std::string& path) const;       // atomic replace (tmp + rename)
  size_t size() const { return t_.size(); }
  size_t loaded() const { return loaded_; }
  uint64_t history_predictions() const { return used_; }
 private:
  void sync() { if (base_->version() != base_ver_) { base_ver_ = base_->version(); bump_version(); } }
  std::shared_ptr<CostPredictor> base_;
  std::unordered_map<std::string, double> t_;
  size_t loaded_ = 0;
  uint64_t used_ = 0, base_ver_ = 0;
};

// Factory from JSON: {"predictor":"oracle"|"noisy"|"affine", "gamma":2.0, "a0":..,"b0":..,"lambda":..}
std::shared_ptr<CostPredictor> make_predictor(const json& spec, uint64_t seed);

}  // namespace opticc
