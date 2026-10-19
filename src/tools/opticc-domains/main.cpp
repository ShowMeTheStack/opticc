// SPDX-License-Identifier: GPL-2.0-only
// opticc-domains: form linker domains by balanced min-cut on the object reference graph.
//   opticc-domains --graph symgraph.json[.gz] --parts K [--balance 1.05] [--seed S] [--weights w.json] [--compare] [--depth 1] --out partition.json
// --weights: {"<objpath>": weight}. --compare also evaluates random and directory partitions.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "opticc/domains/partition.hpp"

using nlohmann::json;
using namespace opticc::domains;

static json load_graph_json(const std::string& p) {
  if (p.size() > 3 && p.substr(p.size() - 3) == ".gz") {
    std::string cmd = "gzip -dc '" + p + "'"; FILE* f = popen(cmd.c_str(), "r"); if (!f) throw std::runtime_error("popen gzip failed");
    std::string s; char buf[1 << 16]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n); pclose(f); return json::parse(s);
  }
  std::ifstream f(p); if (!f) throw std::runtime_error("cannot read " + p); json j; f >> j; return j;
}

int main(int argc, char** argv) try {
  std::map<std::string, std::string> opt; bool compare = false;
  for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (a == "--compare") { compare = true; continue; } if (i + 1 >= argc) { std::fprintf(stderr, "bad args\n"); return 2; } opt[a] = argv[++i]; }
  if (!opt.count("--graph") || !opt.count("--parts")) { std::fprintf(stderr, "usage: opticc-domains --graph g.json --parts K [--balance B] [--seed S] [--weights w.json] [--compare] [--depth D] --out p.json\n"); return 2; }
  json gj = load_graph_json(opt["--graph"]);
  std::map<std::string, double> w; const std::map<std::string, double>* wp = nullptr;
  if (opt.count("--weights")) { std::ifstream f(opt["--weights"]); json wj; f >> wj; for (auto& [k, v] : wj.items()) w[k] = v.get<double>(); wp = &w; }
  Graph g = load_symgraph(gj, wp);
  int k = std::stoi(opt["--parts"]); double bal = opt.count("--balance") ? std::stod(opt["--balance"]) : 1.05; unsigned seed = opt.count("--seed") ? static_cast<unsigned>(std::stoul(opt["--seed"])) : 1u;
  json out; out["n"] = g.n; out["k"] = k; out["edges"] = g.edges.size(); out["intra_object_pcrel"] = g.intra_pcrel; out["intra_object_abs"] = g.intra_abs;
  auto mc = mincut_partition(g, k, bal, seed); out["mincut"] = evaluate(g, mc, k).json();
  if (compare) {
    out["random"] = evaluate(g, random_partition(g, k, seed), k).json();
    out["directory"] = evaluate(g, directory_partition(g, k, opt.count("--depth") ? std::stoi(opt["--depth"]) : 1), k).json();
  }
  json parts = json::object(); for (int i = 0; i < g.n; ++i) parts[g.names[static_cast<size_t>(i)]] = mc[static_cast<size_t>(i)];
  out["partition"] = parts;
  if (opt.count("--out")) { std::ofstream f(opt["--out"]); f << out.dump(1) << "\n"; }
  json summary = out; summary.erase("partition"); std::cout << summary.dump(1) << "\n";
  return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "opticc-domains: error: %s\n", e.what()); return 1; }
