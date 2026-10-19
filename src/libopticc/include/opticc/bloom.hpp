// opticc: a Bloom filter of a worker's object-cache manifest keys, sent in its HELLO_ACK so that a coordinator
// with no history (a FRESH client: empty client cache, new coordinator) can route each job to the worker that already
// holds its object (cache affinity) instead of compiling it locally or on a worker without it. A false positive
// costs only what a job without affinity costs (the worker misses and compiles); a false negative cannot happen for
// keys present when the filter was built (later stores reach the coordinator through RESULT frames).
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace opticc::bloom {

struct Filter {
  uint32_t m = 0, k = 0, n = 0;   // bits, hash functions, keys inserted
  std::string bits;               // m bits, little-endian within each byte

  static void hashes(const std::string& key, uint64_t* h1, uint64_t* h2) {
    uint64_t a = 1469598103934665603ULL, b = 0x9E3779B97F4A7C15ULL;
    for (unsigned char c : key) { a ^= c; a *= 1099511628211ULL; b = (b ^ c) * 0xBF58476D1CE4E5B9ULL; b ^= b >> 31; }
    *h1 = a; *h2 = b | 1;
  }
  static Filter build(const std::vector<std::string>& keys, double bits_per_key = 10.0) {
    Filter f; f.n = static_cast<uint32_t>(keys.size()); f.k = 7;
    uint64_t m = static_cast<uint64_t>(bits_per_key * static_cast<double>(keys.size()));
    if (m < 1024) m = 1024;
    m = (m + 7) / 8 * 8; f.m = static_cast<uint32_t>(m);
    f.bits.assign(f.m / 8, '\0');
    for (auto& key : keys) {
      uint64_t h1, h2; hashes(key, &h1, &h2);
      for (uint32_t i = 0; i < f.k; ++i) { uint64_t bit = (h1 + i * h2) % f.m; f.bits[bit / 8] = static_cast<char>(static_cast<unsigned char>(f.bits[bit / 8]) | (1u << (bit % 8))); }
    }
    return f;
  }
  bool maybe(const std::string& key) const {
    if (m == 0 || bits.size() * 8 != m) return false;
    uint64_t h1, h2; hashes(key, &h1, &h2);
    for (uint32_t i = 0; i < k; ++i) { uint64_t bit = (h1 + i * h2) % m; if (!(static_cast<unsigned char>(bits[bit / 8]) & (1u << (bit % 8)))) return false; }
    return true;
  }
  // JSON form: {"m":..,"k":..,"n":..,"hex":"..."} (hex: compact enough — ~1.25 bytes per key at 10 bits/key)
  nlohmann::json to_json() const {
    static const char* d = "0123456789abcdef";
    std::string hx; hx.reserve(bits.size() * 2);
    for (unsigned char c : bits) { hx.push_back(d[c >> 4]); hx.push_back(d[c & 15]); }
    return nlohmann::json{{"m", m}, {"k", k}, {"n", n}, {"hex", hx}};
  }
  static Filter from_json(const nlohmann::json& j) {
    Filter f;
    if (!j.is_object()) return f;
    std::string hx = j.value("hex", std::string());
    uint32_t m = j.value("m", 0u), k = j.value("k", 0u);
    if (m == 0 || m % 8 != 0 || hx.size() != m / 4 || k == 0 || k > 32) return f;
    auto nib = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::string b; b.reserve(m / 8);
    for (size_t i = 0; i < hx.size(); i += 2) { int hi = nib(hx[i]), lo = nib(hx[i + 1]); if (hi < 0 || lo < 0) return Filter{}; b.push_back(static_cast<char>(hi * 16 + lo)); }
    f.m = m; f.k = k; f.n = j.value("n", 0u); f.bits = std::move(b);
    return f;
  }
};

}  // namespace opticc::bloom
