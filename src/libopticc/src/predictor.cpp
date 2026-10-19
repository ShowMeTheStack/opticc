// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/predictor.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace opticc {

double NoisyOraclePredictor::predict(const Job& j) {
  auto it = factor_.find(j.id);
  if (it == factor_.end()) {
    // Per-job deterministic draw: hash(seed, id) seeds a small engine.
    std::seed_seq ss{static_cast<uint32_t>(seed_), static_cast<uint32_t>(seed_ >> 32),
                     static_cast<uint32_t>(j.id)};
    std::mt19937_64 eng(ss);
    std::uniform_real_distribution<double> u(-std::log(gamma_), std::log(gamma_));
    double f = gamma_ <= 1.0 ? 1.0 : std::exp(u(eng));
    it = factor_.emplace(j.id, f).first;
  }
  return j.work_ref_cpu_s * it->second;
}

double AffineSizePredictor::predict(const Job& j) {
  double x = j.pre_size_bytes / 1e6;
  return std::max(1e-3, a_ + b_ * x);
}

void AffineSizePredictor::observe(const Job& j, double measured_w) {
  if (j.pre_size_bytes <= 0) return;  // barrier/generated jobs carry no size signal
  double x = j.pre_size_bytes / 1e6, y = measured_w;
  n_ += 1; sx_ += x; sxx_ += x * x; sy_ += y; sxy_ += x * y;
  refit();
  bump_version();
}

// Minimise sum (y - a - b x)^2 + lambda ((a-a0)^2 + (b-b0)^2).
// Normal equations: [n+l, sx; sx, sxx+l] [a;b] = [sy + l a0; sxy + l b0].
void AffineSizePredictor::refit() {
  double A11 = n_ + lambda_, A12 = sx_, A22 = sxx_ + lambda_;
  double r1 = sy_ + lambda_ * a0_, r2 = sxy_ + lambda_ * b0_;
  double det = A11 * A22 - A12 * A12;
  if (std::fabs(det) < 1e-12) return;
  a_ = (r1 * A22 - A12 * r2) / det;
  b_ = (A11 * r2 - A12 * r1) / det;
}

// FNV-1a over the job name (falls back to the id) mixed with the seed; log-uniform factor in [1/gamma, gamma].
double GammaNoisePredictor::factor(const Job& j) const {
  if (gamma_ <= 1.0) return 1.0;
  uint64_t h = 1469598103934665603ULL;
  std::string key = j.name.empty() ? std::to_string(j.id) : j.name;
  for (unsigned char ch : key) { h ^= ch; h *= 1099511628211ULL; }
  h ^= seed_ + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  std::mt19937_64 eng(h);
  double u = std::uniform_real_distribution<double>(-1.0, 1.0)(eng);
  return std::exp(u * std::log(gamma_));
}
double GammaNoisePredictor::predict(const Job& j) { sync(); return base_->predict(j) * factor(j); }
void GammaNoisePredictor::observe(const Job& j, double measured_w) { base_->observe(j, measured_w); sync(); }

double HistoryPredictor::predict(const Job& j) {
  sync();
  auto it = t_.find(j.name);
  if (it == t_.end()) return base_->predict(j);
  ++used_;
  return it->second;
}

void HistoryPredictor::record(const std::string& name, double w) {
  if (name.empty() || !(w > 0) || !std::isfinite(w)) return;
  auto it = t_.find(name);
  if (it == t_.end()) t_.emplace(name, w); else it->second = 0.5 * it->second + 0.5 * w;
}

// one record per line: "<seconds>\t<name>\n"; malformed lines are skipped; bounded (2^20 entries)
size_t HistoryPredictor::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) return 0;
  std::string line; size_t n = 0;
  while (std::getline(in, line) && t_.size() < (1u << 20)) {
    size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
    char* end = nullptr; double w = std::strtod(line.c_str(), &end);
    if (end != line.c_str() + tab || !(w > 0) || !std::isfinite(w)) continue;
    t_[line.substr(tab + 1)] = w; ++n;
  }
  loaded_ = n;
  if (n) bump_version();
  return n;
}

bool HistoryPredictor::save(const std::string& path) const {
  std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    for (auto& [k, w] : t_) if (k.find('\n') == std::string::npos) out << w << '\t' << k << '\n';
    if (!out) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::shared_ptr<CostPredictor> make_predictor(const json& spec, uint64_t seed) {
  std::string kind = spec.value("predictor", std::string("oracle"));
  if (kind == "oracle") return std::make_shared<OraclePredictor>();
  if (kind == "noisy") return std::make_shared<NoisyOraclePredictor>(spec.value("gamma", 2.0), seed);
  if (kind == "biased") return std::make_shared<BiasedOraclePredictor>(spec.value("bias", 9.0));
  if (kind == "affine")
    return std::make_shared<AffineSizePredictor>(spec.value("a0", 0.3), spec.value("b0", 0.9),
                                                 spec.value("lambda", 1.0));
  if (kind == "affine-src")
    return std::make_shared<AffineSrcPredictor>(spec.value("a0", 0.3), spec.value("b0", 0.9), spec.value("lambda", 1.0));
  throw std::runtime_error("unknown predictor: " + kind);
}

}  // namespace opticc
