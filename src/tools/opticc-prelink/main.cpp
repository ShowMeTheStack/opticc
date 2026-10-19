// SPDX-License-Identifier: GPL-2.0-only
// opticc-prelink: manifests, liveness and domain prelink with proximity resolution.
//
//   opticc-prelink manifest  [--roots r.json] [--domain N] --out m.json objs...     per-set manifest (+ condensed domain manifest)
//   opticc-prelink liveness  --roots r.json --out live.json objs...                 full-graph liveness (reference)
//   opticc-prelink liveness  --roots r.json --domains d.json --out live.json objs... condensed path (manifests → resolve → expand)
//   opticc-prelink prelink   --domain N [--roots r.json] (--live live.json | --no-gc) [--exports e.json] [--keep-function-sections] [--fold-data]
//                            [--no-apply] [--strip-locals] [--shared] [--origin TAG] -o out.o --manifest m.json objs...
//   opticc-prelink roundtrip in.o out.o
// roots.json: {"entry":["_start"], "keep":[".init*"], "group_members_together":false}
// live.json:  {"live":["<objpath>:(<section>)", ...]}    domains.json: {"<objpath>": N, ...}    exports.json: {"exports":["sym",...]}
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "opticc/elf/liveness.hpp"
#include "opticc/elf/prelink.hpp"

using namespace opticc::elf;
using nlohmann::json;

namespace {
int usage() { std::fprintf(stderr, "usage: opticc-prelink manifest|liveness|prelink|roundtrip ... (see header comment)\n"); return 2; }
json read_json(const std::string& p) { std::ifstream f(p); if (!f) throw std::runtime_error("cannot read " + p); json j; f >> j; return j; }
void write_json(const std::string& p, const json& j) { std::ofstream f(p); if (!f) throw std::runtime_error("cannot write " + p); f << j.dump(1) << "\n"; }
Roots roots_from(const json& j) {
  Roots r;
  if (j.contains("entry")) for (auto& e : j["entry"]) r.entry_symbols.push_back(e.get<std::string>());
  if (j.contains("keep")) for (auto& e : j["keep"]) r.keep_globs.push_back(e.get<std::string>());
  r.group_members_together = j.value("group_members_together", false);
  r.reserved_names = j.value("reserved_names", true);
  return r;
}
std::map<std::string, SecRef> label_index(const ObjectSet& set) {
  std::map<std::string, SecRef> m;
  for (int o = 0; o < static_cast<int>(set.objs.size()); ++o) for (int s = 1; s < static_cast<int>(set.objs[static_cast<size_t>(o)].sections.size()); ++s) m[set.label({o, s})] = {o, s};
  return m;
}
}  // namespace

int main(int argc, char** argv) try {
  if (argc < 2) return usage();
  std::string cmd = argv[1];
  std::vector<std::string> objs; std::map<std::string, std::string> opt; std::vector<std::string> flags;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--", 0) == 0 || a == "-o") {
      if (a == "--no-gc" || a == "--keep-function-sections" || a == "--no-apply" || a == "--strip-locals" || a == "--shared" || a == "--fold-data" || a == "--merge-across-groups") { flags.push_back(a); continue; }
      if (i + 1 >= argc) return usage(); opt[a] = argv[++i];
    } else objs.push_back(a);
  }
  auto has = [&](const char* f) { for (auto& x : flags) if (x == f) return true; return false; };
  if (cmd == "roundtrip") { if (objs.size() != 2) return usage(); write_object_file(read_object(objs[0]), objs[1]); return 0; }
  if (objs.empty()) return usage();
  Roots roots = opt.count("--roots") ? roots_from(read_json(opt["--roots"])) : Roots{};

  if (cmd == "manifest") {
    ObjectSet set = ObjectSet::load(objs); SectionGraph g = build_section_graph(set);
    json j = manifest_json(set, g);
    if (opt.count("--domain")) { DomainManifest dm = condense_domain(set, roots, std::stoi(opt["--domain"])); j["condensed"] = dm.json(set); }
    write_json(opt.count("--out") ? opt["--out"] : "manifest.json", j); return 0;
  }
  if (cmd == "liveness") {
    ObjectSet set = ObjectSet::load(objs);
    json out; std::vector<std::string> live, removed;
    if (!opt.count("--domains")) {
      SectionGraph g = build_section_graph(set); Liveness L = compute_liveness(set, g, roots);
      for (SecRef r : L.live) live.push_back(set.label(r)); for (SecRef r : L.removed) removed.push_back(set.label(r));
    } else {
      json dj = read_json(opt["--domains"]);
      std::map<int, std::vector<int>> members;
      for (int o = 0; o < static_cast<int>(set.objs.size()); ++o) { const std::string& p = set.objs[static_cast<size_t>(o)].path; if (!dj.contains(p)) throw std::runtime_error("no domain for " + p); members[dj[p].get<int>()].push_back(o); }
      std::vector<DomainManifest> ms; std::map<int, ObjectSet> locals; size_t arcs = 0;
      for (auto& [d, vs] : members) { ObjectSet loc; for (int o : vs) loc.objs.push_back(set.objs[static_cast<size_t>(o)]); DomainManifest m = condense_domain(loc, roots, d); m.objs = vs; arcs += m.arc_count(); ms.push_back(m); locals[d] = std::move(loc); }
      auto live_entries = resolve_condensed(ms);
      for (auto& [d, vs] : members) {
        Liveness L = expand_domain(locals[d], roots, live_entries[d]);
        for (SecRef r : L.live) live.push_back(locals[d].label(r)); for (SecRef r : L.removed) removed.push_back(locals[d].label(r));
      }
      out["condensed_arcs"] = arcs; out["domains"] = members.size();
    }
    std::sort(live.begin(), live.end()); std::sort(removed.begin(), removed.end());
    out["live"] = live; out["removed"] = removed; write_json(opt.count("--out") ? opt["--out"] : "live.json", out); return 0;
  }
  if (cmd == "prelink") {
    ObjectSet set = ObjectSet::load(objs);
    PrelinkOptions po; po.gc = !has("--no-gc"); po.keep_function_sections = has("--keep-function-sections"); po.fold_data = has("--fold-data"); po.merge_across_groups = has("--merge-across-groups"); po.apply_pcrel = !has("--no-apply");
    po.strip_locals = has("--strip-locals"); po.static_link = !has("--shared"); po.origin_tag = opt.count("--origin") ? opt["--origin"] : (opt.count("--domain") ? "domain-" + opt["--domain"] : "domain");
    for (auto& e : roots.entry_symbols) po.entry_symbols.insert(e);
    if (opt.count("--exports")) { po.have_exports = true; for (auto& e : read_json(opt["--exports"])["exports"]) po.exports.insert(e.get<std::string>()); }
    std::set<SecRef> live;
    if (po.gc) {
      if (!opt.count("--live")) throw std::runtime_error("prelink: --live live.json or --no-gc required");
      auto idx = label_index(set); json lj = read_json(opt["--live"]);
      for (auto& l : lj["live"]) { auto it = idx.find(l.get<std::string>()); if (it != idx.end()) live.insert(it->second); }
    }
    PrelinkResult r = prelink(set, po.gc ? &live : nullptr, po);
    if (!opt.count("-o")) throw std::runtime_error("prelink: -o required");
    write_object_file(r.out, opt["-o"]);
    if (opt.count("--manifest")) write_json(opt["--manifest"], r.manifest);
    std::fprintf(stderr, "opticc-prelink: %zu inputs -> %s: applied %llu PC-relative relocations, residual %llu (cut pcrel %llu, abs %llu), sections %llu->%llu, dropped dead %llu comdat %llu, fdes dropped %llu\n",
                 objs.size(), opt["-o"].c_str(), static_cast<unsigned long long>(r.stats.applied_pcrel), static_cast<unsigned long long>(r.stats.residual.total),
                 static_cast<unsigned long long>(r.stats.residual_cut_pcrel), static_cast<unsigned long long>(r.stats.residual.abs), static_cast<unsigned long long>(r.stats.sections_in),
                 static_cast<unsigned long long>(r.stats.sections_out), static_cast<unsigned long long>(r.stats.sections_dropped_dead), static_cast<unsigned long long>(r.stats.sections_dropped_comdat), static_cast<unsigned long long>(r.stats.fdes_dropped));
    return 0;
  }
  return usage();
} catch (const std::exception& e) { std::fprintf(stderr, "opticc-prelink: error: %s\n", e.what()); return 1; }
