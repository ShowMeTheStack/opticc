// SPDX-License-Identifier: GPL-2.0-only
// Portable random distributions for tests.
//
// std::normal_distribution, std::lognormal_distribution and std::uniform_real_distribution are implementation-defined:
// libc++ (macOS) and libstdc++ (Linux) draw different sequences from the same seeded engine, so a statistical test tuned
// on one platform sees a different sample on the other. Only the engines (std::mt19937, std::mt19937_64) are specified
// bit-exactly. These helpers derive every variate from raw engine output (53-bit uniforms, Box-Muller), so a seeded test
// sees the same sample everywhere (up to last-ulp differences of std::log/std::exp/std::cos).
#pragma once
#include <cmath>
#include <cstdint>

namespace opticc_test {

// uniform in [0, 1) with 53 random bits, from either engine width. The width is decided by the engine's RANGE
// (Engine::max()), not by sizeof(result_type): std::mt19937's result_type is uint_fast32_t, which is 8 bytes on Linux
// x86-64, so a sizeof test took the 64-bit path for a 32-bit engine and produced uniforms near 0.
template <class Engine> double u01(Engine& g) {
  static_assert(Engine::min() == 0, "engine must start at 0");
  uint64_t x;
  if constexpr (Engine::max() == 0xFFFFFFFFFFFFFFFFull) x = static_cast<uint64_t>(g()) >> 11;
  else {
    static_assert(Engine::max() == 0xFFFFFFFFull, "32- or 64-bit engines only");
    x = ((static_cast<uint64_t>(g()) << 32) | static_cast<uint64_t>(g())) >> 11;
  }
  return static_cast<double>(x) * (1.0 / 9007199254740992.0);   // 2^-53
}

struct PUniform {
  double a, b;
  PUniform(double lo, double hi) : a(lo), b(hi) {}
  template <class E> double operator()(E& g) const { return a + (b - a) * u01(g); }
};

struct PNormal {   // Box-Muller; uses two uniforms per variate (no cached second value: simpler, still deterministic)
  double mu, sd;
  PNormal(double m, double s) : mu(m), sd(s) {}
  template <class E> double operator()(E& g) const {
    double u1 = u01(g); while (u1 <= 0.0) u1 = u01(g);
    const double u2 = u01(g);
    return mu + sd * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

struct PLognormal {
  PNormal n;
  PLognormal(double m, double s) : n(m, s) {}
  template <class E> double operator()(E& g) const { return std::exp(n(g)); }
};

}  // namespace opticc_test
