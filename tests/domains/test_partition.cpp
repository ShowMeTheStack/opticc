// SPDX-License-Identifier: GPL-2.0-only
#include "test_harness.hpp"
#include "elf_test_util.hpp"
#include "opticc/domains/partition.hpp"

#include <algorithm>
#include <random>

using namespace opticc::domains;
using nlohmann::json;

OPTICC_TEST(partition_mincut_beats_random) {
  // planted partition: 8 clusters of 60 vertices, dense inside (p=0.3, w~U[1,20]), sparse across (p=0.01, w~U[1,5])
  std::mt19937 rng(3); json g; g["objects"] = json::array(); g["edges_obj"] = json::array();
  int C = 8, S = 60, n = C * S;
  for (int i = 0; i < n; ++i) g["objects"].push_back({{"id", i}, {"path", "./c" + std::to_string(i / S) + "/o" + std::to_string(i) + ".o"}, {"sections", json::array({{{"name", ".text"}, {"size", 100 + static_cast<int>(rng() % 50)}}})}});
  double planted_cut = 0, total = 0;
  for (int u = 0; u < n; ++u) for (int v = u + 1; v < n; ++v) {
    bool same = u / S == v / S; double p = same ? 0.3 : 0.01; if ((rng() % 10000) / 10000.0 >= p) continue;
    double w = same ? 1 + static_cast<double>(rng() % 20) : 1 + static_cast<double>(rng() % 5);
    g["edges_obj"].push_back({u, v, w, 0.0}); total += w; if (!same) planted_cut += w;
  }
  Graph G = load_symgraph(g);
  auto mc = mincut_partition(G, C, 1.05, 1); auto rp = random_partition(G, C, 1); auto dp = directory_partition(G, C, 1);
  CutStats smc = evaluate(G, mc, C), srp = evaluate(G, rp, C), sdp = evaluate(G, dp, C);
  std::printf("  planted cut frac %.3f | mincut %.3f (imb %.3f) | random %.3f | directory %.3f\n", planted_cut / total, smc.cut_fraction(), smc.imbalance(), srp.cut_fraction(), sdp.cut_fraction());
  CHECK(smc.cut_fraction() < 0.5 * srp.cut_fraction());
  CHECK(smc.cut_fraction() <= planted_cut / total * 1.15 + 0.01);
  CHECK(smc.imbalance() <= 1.06);
  CHECK(sdp.cut_fraction() <= planted_cut / total * 1.001);   // the directory grouping is the planted partition here
}
