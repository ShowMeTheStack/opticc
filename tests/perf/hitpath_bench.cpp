// Cost of the compiler wrapper's direct-hit object copy, per object size:
//   copy      copy_file_range / clone, no verification
//   verified  XXH64 on a read-only mapping of the same open file, then copy_file_range / clone (the path the wrapper uses)
//   userspace read into memory, XXH64, write (an alternative, for comparison)
// Prints the median and p90 in microseconds over N rounds (page cache warm, as on a repeat build). Always exits 0
// unless a copy fails or a verification disagrees.
// SPDX-License-Identifier: GPL-2.0-only
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "opticc/util.hpp"

using namespace opticc;

int main(int argc, char** argv) {
  const int rounds = argc > 1 ? std::atoi(argv[1]) : 200;
  const std::string d = util::make_temp_dir("opticc-hitbench-");
  std::printf("%-10s %10s %12s %12s %12s %12s %12s %12s\n", "size", "rounds", "copy_med", "copy_p90", "verif_med", "verif_p90", "user_med", "user_p90");
  int rc = 0;
  for (size_t size : {32u << 10, 512u << 10, 4u << 20, 16u << 20}) {
    std::string data(size, '\0');
    uint64_t x = 88172645463325252ULL;
    for (auto& c : data) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; c = static_cast<char>(x); }
    const std::string src = d + "/obj", dst = d + "/out.o";
    util::write_file(src, data);
    const std::string xxh = util::xxh64_hex(data);
    auto timeit = [&](auto&& f) {
      std::vector<double> us;
      for (int i = 0; i < rounds; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!f()) { std::fprintf(stderr, "copy failed\n"); rc = 1; }
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
      }
      std::sort(us.begin(), us.end());
      return std::make_pair(us[us.size() / 2], us[us.size() * 9 / 10]);
    };
    auto a = timeit([&] { return util::copy_verified_atomic(src, dst, -1, ""); });
    auto b = timeit([&] { return util::copy_verified_atomic(src, dst, static_cast<int64_t>(size), xxh); });
    auto c = timeit([&] {
      const std::string o = util::read_file(src);
      if (util::xxh64_hex(o) != xxh) return false;
      util::write_file(dst + ".tmp", o); return ::rename((dst + ".tmp").c_str(), dst.c_str()) == 0;
    });
    if (util::read_file(dst) != data) { std::fprintf(stderr, "content mismatch\n"); rc = 1; }
    std::string bad = data; bad[size / 2] ^= 1; util::write_file(src, bad);
    std::string why;
    if (util::copy_verified_atomic(src, dst, static_cast<int64_t>(size), xxh, &why) || why != "bad-object-checksum") { std::fprintf(stderr, "a corrupted source was not refused\n"); rc = 1; }
    std::printf("%-10zu %10d %12.1f %12.1f %12.1f %12.1f %12.1f %12.1f\n", size, rounds, a.first, a.second, b.first, b.second, c.first, c.second);
  }
  util::remove_tree(d);
  return rc;
}
