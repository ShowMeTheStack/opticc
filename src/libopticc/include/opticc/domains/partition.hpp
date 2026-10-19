// SPDX-License-Identifier: GPL-2.0-only
// Linker-domain formation: balanced k-way min-cut on the object reference graph,
// objective = cut weight of PC-relative edges (the relocations a domain prelink could otherwise resolve),
// vertex weight = predicted work or bytes. Multilevel: heavy-edge-matching coarsening, greedy growing on the
// coarsest graph, FM-style boundary refinement during uncoarsening. Baselines: random and directory-based
// (kbuild's natural grouping) partitions with LPT packing.
#pragma once
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace opticc::domains {

struct Graph {
  int n = 0;
  std::vector<std::string> names;      // object paths
  std::vector<double> vw;              // vertex weights
  struct Edge { int u, v; double pcrel, abs; };
  std::vector<Edge> edges;             // undirected, u < v, aggregated; self-edges dropped (intra-object)
  std::vector<std::vector<std::pair<int, double>>> adj;   // neighbour, pcrel weight
  double total_pcrel = 0, total_abs = 0, intra_pcrel = 0, intra_abs = 0;
  void finalize();
};

// Reads the workloads/symgraph.py JSON (objects[], edges_obj[[u,v,pcrel,abs]]). Optional weights by object path.
Graph load_symgraph(const nlohmann::json& j, const std::map<std::string, double>* weights = nullptr);

struct CutStats {
  double cut_pcrel = 0, cut_abs = 0, total_pcrel = 0, total_abs = 0;
  double max_weight = 0, avg_weight = 0;
  double cut_fraction() const { return total_pcrel > 0 ? cut_pcrel / total_pcrel : 0; }
  double imbalance() const { return avg_weight > 0 ? max_weight / avg_weight : 0; }
  nlohmann::json json() const;
};

CutStats evaluate(const Graph& g, const std::vector<int>& part, int k);
std::vector<int> random_partition(const Graph& g, int k, unsigned seed);               // random order, lightest-part assignment
std::vector<int> directory_partition(const Graph& g, int k, int depth = 1);            // group by path prefix, LPT into k parts
std::vector<int> mincut_partition(const Graph& g, int k, double balance = 1.05, unsigned seed = 1, int refine_passes = 12);

}  // namespace opticc::domains
