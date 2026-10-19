// Closure-path microbenchmark: per-TU cost of what the coordinator does for a header-push job on a clean build
// (include-scan closure plus SHA-256 of every closure file, with the coordinator's hash cache), for the baseline
// scanner (`--impl head`, a snapshot of the earlier unmemoised code) and the memoised scanner (`--impl new`).
// Usage: bench_closure --tus tus.json --impl head|new [--limit N] [--threads T] [--check]
// SPDX-License-Identifier: GPL-2.0-only
#include <sys/resource.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "incscan_head.hpp"
#include "nlohmann/json.hpp"
#include "opticc/argscan.hpp"
#include "opticc/closure.hpp"
#include "opticc/incscan.hpp"
#include "opticc/util.hpp"

using namespace opticc;
struct HashEnt { int64_t mt; uint64_t sz; std::string sha; double at; };
static std::mutex hmu; static std::unordered_map<std::string, HashEnt> hcache;
static std::string hash_file(const std::string& p, bool ttl) {
  double t = util::now_s();
  if (ttl) { std::lock_guard<std::mutex> g(hmu); auto it = hcache.find(p); if (it != hcache.end() && t - it->second.at < 5.0) return it->second.sha; }
  struct stat st{}; if (::stat(p.c_str(), &st) != 0) return "";
#ifdef __APPLE__
  int64_t mt = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  int64_t mt = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  uint64_t sz = static_cast<uint64_t>(st.st_size);
  { std::lock_guard<std::mutex> g(hmu); auto it = hcache.find(p); if (it != hcache.end() && it->second.mt == mt && it->second.sz == sz) { it->second.at = t; return it->second.sha; } }
  std::string sha = util::sha256_hex(util::read_file(p));
  std::lock_guard<std::mutex> g(hmu); hcache[p] = HashEnt{mt, sz, sha, t}; return sha;
}
static double cpu_s() { rusage ru{}; getrusage(RUSAGE_SELF, &ru); return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6; }

int main(int argc, char** argv) {
  std::string tusf, impl = "new"; size_t limit = 1u << 30; int threads = 1; bool check = false, warm = false;
  for (int i = 1; i < argc; ++i) { std::string a = argv[i];
    if (a == "--tus") tusf = argv[++i]; else if (a == "--impl") impl = argv[++i]; else if (a == "--limit") limit = std::stoul(argv[++i]);
    else if (a == "--threads") threads = std::stoi(argv[++i]); else if (a == "--check") check = true; else if (a == "--warm") warm = true; }
  auto tus = nlohmann::json::parse(util::read_file(tusf));
  size_t n = std::min(limit, tus.size());
  incscan::Scanner sn; incscan_head::Scanner sh;
  std::vector<double> lat(n); std::vector<size_t> sizes(n); std::atomic<size_t> next{0}; std::atomic<int> computed{0}; std::atomic<size_t> mism{0};
  if (warm) {   // untimed warm-up pass: the steady state of a build after its first few hundred TUs
    for (size_t i = 0; i < n; ++i) { auto& tu = tus[i]; std::vector<std::string> av = tu["argv"].get<std::vector<std::string>>(); bool c = false;
      auto cl = impl == "head" ? sh.closure(tu["src"], args::include_dirs(av), {}, tu["cwd"], &c) : sn.closure(tu["src"], args::include_dirs(av), {}, tu["cwd"], &c);
      for (auto& f : cl) hash_file(f, impl != "head"); }
  }
  double c0 = cpu_s(), t0 = util::now_s();
  auto work = [&] {
    for (;;) {
      size_t i = next++; if (i >= n) break;
      auto& tu = tus[i]; std::vector<std::string> av = tu["argv"].get<std::vector<std::string>>(); std::string cwd = tu["cwd"], src = tu["src"];
      auto dirs = args::include_dirs(av); bool comp = false;
      double t = util::now_s();
      std::vector<std::string> cl = impl == "head" ? sh.closure(src, dirs, {}, cwd, &comp) : sn.closure(src, dirs, {}, cwd, &comp);
      if (impl == "head") { for (auto& f : cl) hash_file(f, false); }
      else {
        // mirrors Coordinator::build_closure: stat all (no lock), one lock to match, hash only misses
        struct E { const std::string* p; int64_t mt; uint64_t sz; }; std::vector<E> es; es.reserve(cl.size());
        for (auto& f : cl) { struct stat st{}; if (::stat(f.c_str(), &st) != 0) continue;
#ifdef __APPLE__
          int64_t mt = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
          int64_t mt = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
          es.push_back(E{&f, mt, static_cast<uint64_t>(st.st_size)}); }
        std::vector<const std::string*> todo;
        { std::lock_guard<std::mutex> g(hmu); for (auto& e : es) { auto it = hcache.find(*e.p); if (!(it != hcache.end() && it->second.mt == e.mt && it->second.sz == e.sz)) todo.push_back(e.p); } }
        for (auto* f : todo) hash_file(*f, false);
      }
      lat[i] = util::now_s() - t; sizes[i] = cl.size(); if (comp) computed++;
      if (check) { bool c2 = false; auto ref = sh.closure(src, dirs, {}, cwd, &c2); std::set<std::string> a(cl.begin(), cl.end()), b(ref.begin(), ref.end()); if (a != b) mism++; }
    }
  };
  std::vector<std::thread> th; for (int k = 0; k < threads; ++k) th.emplace_back(work); for (auto& x : th) x.join();
  double wall = util::now_s() - t0, cpu = cpu_s() - c0;
  std::vector<double> s = lat; std::sort(s.begin(), s.end());
  auto q = [&](double p) { return s[std::min(s.size() - 1, static_cast<size_t>(p * static_cast<double>(s.size())))] * 1000; };
  double mean_sz = 0; for (auto z : sizes) mean_sz += static_cast<double>(z); mean_sz /= static_cast<double>(n);
  std::printf("{\"impl\":\"%s\",\"tus\":%zu,\"threads\":%d,\"first_ms\":%.2f,\"p50_ms\":%.3f,\"p90_ms\":%.3f,\"p99_ms\":%.3f,\"max_ms\":%.2f,"
              "\"wall_s\":%.2f,\"cpu_s\":%.2f,\"cpu_ms_per_tu\":%.3f,\"mean_closure\":%.0f,\"computed\":%d,\"mismatch_vs_head\":%zu}\n",
              impl.c_str(), n, threads, lat[0] * 1000, q(0.5), q(0.9), q(0.99), s.back() * 1000, wall, cpu, cpu / static_cast<double>(n) * 1000, mean_sz, computed.load(), mism.load());
  return 0;
}
