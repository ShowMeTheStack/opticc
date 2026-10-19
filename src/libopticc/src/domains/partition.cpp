// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/domains/partition.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>

namespace opticc::domains {

void Graph::finalize() {
  adj.assign(static_cast<size_t>(n), {});
  total_pcrel = total_abs = 0;
  for (const Edge& e : edges) { adj[static_cast<size_t>(e.u)].push_back({e.v, e.pcrel}); adj[static_cast<size_t>(e.v)].push_back({e.u, e.pcrel}); total_pcrel += e.pcrel; total_abs += e.abs; }
}

Graph load_symgraph(const nlohmann::json& j, const std::map<std::string, double>* weights) {
  Graph g; const auto& objs = j.at("objects"); g.n = static_cast<int>(objs.size());
  g.names.resize(static_cast<size_t>(g.n)); g.vw.assign(static_cast<size_t>(g.n), 0.0);
  for (const auto& o : objs) {
    int id = o.at("id").get<int>(); g.names[static_cast<size_t>(id)] = o.at("path").get<std::string>();
    double w = 0;
    if (weights) { auto it = weights->find(g.names[static_cast<size_t>(id)]); if (it != weights->end()) w = it->second; }
    if (w <= 0) { for (const auto& s : o.at("sections")) w += s.value("size", 0.0); }   // fallback: bytes
    g.vw[static_cast<size_t>(id)] = std::max(w, 1.0);
  }
  std::map<std::pair<int, int>, std::pair<double, double>> agg;
  for (const auto& e : j.at("edges_obj")) {
    int u = e.at(0).get<int>(), v = e.at(1).get<int>(); double p = e.at(2).get<double>(), a = e.at(3).get<double>();
    if (u == v) { g.intra_pcrel += p; g.intra_abs += a; continue; }
    if (u > v) std::swap(u, v);
    auto& x = agg[{u, v}]; x.first += p; x.second += a;
  }
  for (auto& [k, w] : agg) g.edges.push_back({k.first, k.second, w.first, w.second});
  g.finalize();
  return g;
}

nlohmann::json CutStats::json() const {
  return {{"cut_pcrel", cut_pcrel}, {"cut_abs", cut_abs}, {"total_pcrel", total_pcrel}, {"total_abs", total_abs}, {"cut_fraction_pcrel", cut_fraction()},
          {"max_weight", max_weight}, {"avg_weight", avg_weight}, {"imbalance", imbalance()}};
}

CutStats evaluate(const Graph& g, const std::vector<int>& part, int k) {
  CutStats s; s.total_pcrel = g.total_pcrel; s.total_abs = g.total_abs;
  for (const auto& e : g.edges) if (part[static_cast<size_t>(e.u)] != part[static_cast<size_t>(e.v)]) { s.cut_pcrel += e.pcrel; s.cut_abs += e.abs; }
  std::vector<double> w(static_cast<size_t>(k), 0.0); double tot = 0;
  for (int i = 0; i < g.n; ++i) { w[static_cast<size_t>(part[static_cast<size_t>(i)])] += g.vw[static_cast<size_t>(i)]; tot += g.vw[static_cast<size_t>(i)]; }
  s.max_weight = *std::max_element(w.begin(), w.end()); s.avg_weight = tot / k;
  return s;
}

static std::vector<int> lpt_assign(const Graph& g, const std::vector<int>& order, int k) {
  std::vector<int> part(static_cast<size_t>(g.n), 0); std::vector<double> w(static_cast<size_t>(k), 0.0);
  for (int v : order) { int best = static_cast<int>(std::min_element(w.begin(), w.end()) - w.begin()); part[static_cast<size_t>(v)] = best; w[static_cast<size_t>(best)] += g.vw[static_cast<size_t>(v)]; }
  return part;
}

std::vector<int> random_partition(const Graph& g, int k, unsigned seed) {
  std::vector<int> order(static_cast<size_t>(g.n)); std::iota(order.begin(), order.end(), 0);
  std::mt19937 rng(seed); std::shuffle(order.begin(), order.end(), rng);
  return lpt_assign(g, order, k);
}

std::vector<int> directory_partition(const Graph& g, int k, int depth) {
  // group vertices by the first `depth` path components after an optional leading "./"
  std::map<std::string, std::vector<int>> groups;
  for (int i = 0; i < g.n; ++i) {
    std::string p = g.names[static_cast<size_t>(i)]; if (p.rfind("./", 0) == 0) p = p.substr(2);
    std::string key; size_t pos = 0; for (int dcount = 0; dcount < depth; ++dcount) { size_t s = p.find('/', pos); if (s == std::string::npos) { key = p.substr(0, pos ? pos - 1 : 0); break; } key = p.substr(0, s); pos = s + 1; }
    groups[key].push_back(i);
  }
  std::vector<std::pair<double, std::string>> gw; for (auto& [key, vs] : groups) { double w = 0; for (int v : vs) w += g.vw[static_cast<size_t>(v)]; gw.push_back({w, key}); }
  std::sort(gw.rbegin(), gw.rend());
  std::vector<int> part(static_cast<size_t>(g.n), 0); std::vector<double> pw(static_cast<size_t>(k), 0.0);
  for (auto& [w, key] : gw) { int best = static_cast<int>(std::min_element(pw.begin(), pw.end()) - pw.begin()); for (int v : groups[key]) part[static_cast<size_t>(v)] = best; pw[static_cast<size_t>(best)] += w; }
  return part;
}

namespace {
struct Level { Graph g; std::vector<int> map; };   // map: fine vertex -> coarse vertex

Graph contract(const Graph& g, const std::vector<int>& match, int nc) {
  Graph c; c.n = nc; c.vw.assign(static_cast<size_t>(nc), 0.0); c.names.assign(static_cast<size_t>(nc), "");
  for (int i = 0; i < g.n; ++i) c.vw[static_cast<size_t>(match[static_cast<size_t>(i)])] += g.vw[static_cast<size_t>(i)];
  std::map<std::pair<int, int>, std::pair<double, double>> agg;
  for (const auto& e : g.edges) { int u = match[static_cast<size_t>(e.u)], v = match[static_cast<size_t>(e.v)]; if (u == v) continue; if (u > v) std::swap(u, v); auto& x = agg[{u, v}]; x.first += e.pcrel; x.second += e.abs; }
  for (auto& [k, w] : agg) c.edges.push_back({k.first, k.second, w.first, w.second});
  c.finalize(); return c;
}

// Heavy-edge matching; vertices heavier than `cap` stay unmatched.
std::vector<int> hem(const Graph& g, double cap, std::mt19937& rng, int& nc) {
  std::vector<int> order(static_cast<size_t>(g.n)); std::iota(order.begin(), order.end(), 0); std::shuffle(order.begin(), order.end(), rng);
  std::vector<int> match(static_cast<size_t>(g.n), -1); nc = 0;
  for (int v : order) {
    if (match[static_cast<size_t>(v)] >= 0) continue;
    int best = -1; double bw = -1;
    for (auto& [u, w] : g.adj[static_cast<size_t>(v)]) if (match[static_cast<size_t>(u)] < 0 && u != v && w > bw && g.vw[static_cast<size_t>(u)] + g.vw[static_cast<size_t>(v)] <= cap) { bw = w; best = u; }
    match[static_cast<size_t>(v)] = nc; if (best >= 0) match[static_cast<size_t>(best)] = nc; nc++;
  }
  return match;
}

void refine(const Graph& g, std::vector<int>& part, int k, double maxw, int passes, std::mt19937& rng) {
  std::vector<double> pw(static_cast<size_t>(k), 0.0);
  for (int i = 0; i < g.n; ++i) pw[static_cast<size_t>(part[static_cast<size_t>(i)])] += g.vw[static_cast<size_t>(i)];
  std::vector<int> order(static_cast<size_t>(g.n)); std::iota(order.begin(), order.end(), 0);
  for (int pass = 0; pass < passes; ++pass) {
    std::shuffle(order.begin(), order.end(), rng); int moves = 0;
    for (int v : order) {
      int pv = part[static_cast<size_t>(v)]; std::map<int, double> conn; double internal = 0;
      for (auto& [u, w] : g.adj[static_cast<size_t>(v)]) { int pu = part[static_cast<size_t>(u)]; if (pu == pv) internal += w; else conn[pu] += w; }
      if (conn.empty()) continue;
      int best = -1; double bestgain = 0;
      for (auto& [p, w] : conn) { double gain = w - internal; bool fits = pw[static_cast<size_t>(p)] + g.vw[static_cast<size_t>(v)] <= maxw; bool balances = pw[static_cast<size_t>(p)] + g.vw[static_cast<size_t>(v)] < pw[static_cast<size_t>(pv)];
        if ((gain > bestgain && fits) || (gain == bestgain && gain > 0 && fits && balances)) { bestgain = gain; best = p; } }
      if (best >= 0) { pw[static_cast<size_t>(pv)] -= g.vw[static_cast<size_t>(v)]; pw[static_cast<size_t>(best)] += g.vw[static_cast<size_t>(v)]; part[static_cast<size_t>(v)] = best; moves++; }
    }
    if (moves == 0) break;
  }
  // balance repair: move lightest-connected vertices out of overweight parts
  for (int it = 0; it < g.n; ++it) {
    int heavy = static_cast<int>(std::max_element(pw.begin(), pw.end()) - pw.begin()); if (pw[static_cast<size_t>(heavy)] <= maxw) break;
    int bestv = -1; double bestcost = 1e300; int target = static_cast<int>(std::min_element(pw.begin(), pw.end()) - pw.begin());
    double deficit = pw[static_cast<size_t>(heavy)] - pw[static_cast<size_t>(target)];
    for (int v = 0; v < g.n; ++v) {
      if (part[static_cast<size_t>(v)] != heavy) continue;
      if (g.vw[static_cast<size_t>(v)] >= deficit) continue;                       // moving it would not reduce the maximum
      double cost = 0; for (auto& [u, w] : g.adj[static_cast<size_t>(v)]) { if (part[static_cast<size_t>(u)] == heavy) cost += w; else if (part[static_cast<size_t>(u)] == target) cost -= w; }
      if (cost < bestcost) { bestcost = cost; bestv = v; }
    }
    if (bestv < 0) break;                                                        // only immovable (giant) vertices remain: inherent imbalance
    pw[static_cast<size_t>(heavy)] -= g.vw[static_cast<size_t>(bestv)]; pw[static_cast<size_t>(target)] += g.vw[static_cast<size_t>(bestv)]; part[static_cast<size_t>(bestv)] = target;
  }
}

std::vector<int> initial_partition(const Graph& g, int k, double maxw, std::mt19937& rng) {
  // greedy region growing from the heaviest unassigned vertices, then LPT for leftovers
  std::vector<int> part(static_cast<size_t>(g.n), -1); std::vector<double> pw(static_cast<size_t>(k), 0.0);
  double target = std::accumulate(g.vw.begin(), g.vw.end(), 0.0) / k;
  std::vector<int> byw(static_cast<size_t>(g.n)); std::iota(byw.begin(), byw.end(), 0);
  std::sort(byw.begin(), byw.end(), [&](int a, int b) { return g.vw[static_cast<size_t>(a)] > g.vw[static_cast<size_t>(b)]; });
  for (int p = 0; p < k; ++p) {
    int seed = -1; for (int v : byw) if (part[static_cast<size_t>(v)] < 0) { seed = v; break; }
    if (seed < 0) break;
    std::vector<int> frontier{seed}; part[static_cast<size_t>(seed)] = p; pw[static_cast<size_t>(p)] += g.vw[static_cast<size_t>(seed)];
    while (!frontier.empty() && pw[static_cast<size_t>(p)] < target) {
      // pick the unassigned neighbour with max connection to part p
      std::map<int, double> cand;
      for (int f : frontier) for (auto& [u, w] : g.adj[static_cast<size_t>(f)]) if (part[static_cast<size_t>(u)] < 0) cand[u] += w;
      if (cand.empty()) break;
      int best = -1; double bw = -1; for (auto& [u, w] : cand) if (w > bw) { bw = w; best = u; }
      part[static_cast<size_t>(best)] = p; pw[static_cast<size_t>(p)] += g.vw[static_cast<size_t>(best)]; frontier.push_back(best);
      if (frontier.size() > 64) frontier.erase(frontier.begin());
    }
  }
  for (int v : byw) if (part[static_cast<size_t>(v)] < 0) { int best = static_cast<int>(std::min_element(pw.begin(), pw.end()) - pw.begin()); part[static_cast<size_t>(v)] = best; pw[static_cast<size_t>(best)] += g.vw[static_cast<size_t>(v)]; }
  refine(g, part, k, maxw, 8, rng);
  return part;
}
}  // namespace

std::vector<int> mincut_partition(const Graph& g0, int k, double balance, unsigned seed, int refine_passes) {
  if (k < 1) throw std::invalid_argument("k >= 1");
  std::mt19937 rng(seed);
  double total = std::accumulate(g0.vw.begin(), g0.vw.end(), 0.0), maxw = balance * total / k;
  std::vector<Level> levels; levels.push_back({g0, {}});
  while (levels.back().g.n > std::max(60, 8 * k)) {
    int nc = 0; auto match = hem(levels.back().g, maxw * 0.5, rng, nc);
    if (nc > levels.back().g.n * 0.9) break;
    Graph c = contract(levels.back().g, match, nc); levels.back().map = match; levels.push_back({std::move(c), {}});
  }
  std::vector<int> part = initial_partition(levels.back().g, k, maxw, rng);
  for (int li = static_cast<int>(levels.size()) - 2; li >= 0; --li) {
    const Level& fine = levels[static_cast<size_t>(li)];
    std::vector<int> fp(static_cast<size_t>(fine.g.n));
    for (int v = 0; v < fine.g.n; ++v) fp[static_cast<size_t>(v)] = part[static_cast<size_t>(fine.map[static_cast<size_t>(v)])];
    part = std::move(fp); refine(fine.g, part, k, maxw, refine_passes, rng);
  }
  return part;
}

}  // namespace opticc::domains
