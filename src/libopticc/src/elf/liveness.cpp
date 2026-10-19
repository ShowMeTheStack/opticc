// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/elf/liveness.hpp"

#include <deque>
#include <functional>

namespace opticc::elf {

bool glob_match(const std::string& p, const std::string& s) {
  size_t pi = 0, si = 0, star = std::string::npos, mark = 0;
  while (si < s.size()) {
    if (pi < p.size() && (p[pi] == s[si])) { ++pi; ++si; }
    else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = si; }
    else if (star != std::string::npos) { pi = star + 1; si = ++mark; }
    else return false;
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}

static bool reserved_name(const std::string& n) {
  static const char* pre[] = {".init", ".fini", ".ctors", ".dtors", ".jcr", ".init_array", ".fini_array", ".preinit_array"};
  for (const char* p : pre) if (n.rfind(p, 0) == 0) return true;
  return false;
}

bool is_root_section(const Section& s, const Roots& r) {
  if (!(s.flags & SHF_ALLOC)) return false;
  if (s.flags & SHF_GNU_RETAIN) return true;
  if (r.reserved_names && reserved_name(s.name)) return true;
  for (auto& g : r.keep_globs) if (glob_match(g, s.name)) return true;
  return false;
}

namespace {
// Shared propagation engine. `follow_import` is called for relocations to symbols undefined in `set`.
struct Engine {
  const ObjectSet& set; const SectionGraph& g; const Roots& roots;
  std::map<SecRef, std::vector<SecRef>> adj, dependents, groupmates;
  std::map<std::string, std::vector<SecRef>> named;
  std::set<SecRef> eh_sections;                                   // always live, relocations handled per record
  std::map<SecRef, std::vector<SecRef>> eh_fde_deps;              // function section -> LSDA/other targets of its FDEs
  std::vector<SecRef> eh_cie_targets;                             // personality routines etc. (live whenever .eh_frame is)
  Engine(const ObjectSet& os, const SectionGraph& gg, const Roots& r) : set(os), g(gg), roots(r) {
    // .eh_frame: never dropped as a whole (the linker prunes FDEs); its arcs are not followed generically
    for (int oi = 0; oi < static_cast<int>(set.objs.size()); ++oi) {
      const Object& o = set.objs[static_cast<size_t>(oi)];
      for (int si = 1; si < static_cast<int>(o.sections.size()); ++si) {
        const Section& sec = o.sections[static_cast<size_t>(si)];
        if (sec.name != ".eh_frame" || (sec.type != SHT_PROGBITS && sec.type != SHT_X86_64_UNWIND)) continue;   // clang emits SHT_X86_64_UNWIND
        eh_sections.insert({oi, si});
        auto rit = o.relocs.find(si); static const std::vector<Rela> none;
        for (const EhRecord& rec : parse_eh_frame(sec, rit == o.relocs.end() ? none : rit->second)) {
          if (!rec.fde) { for (const Rela* x : rec.others) { SecRef t = g.target_of(set, oi, x->sym); if (t.valid()) eh_cie_targets.push_back(t); } continue; }
          SecRef fn = rec.pc_begin ? g.target_of(set, oi, rec.pc_begin->sym) : SecRef{};
          for (const Rela* x : rec.others) { SecRef t = g.target_of(set, oi, x->sym); if (t.valid() && fn.valid()) eh_fde_deps[fn].push_back(t); }
        }
      }
    }
    for (auto& [k, c] : g.arcs) if (!eh_sections.count(k.first)) adj[k.first].push_back(k.second);
    for (int oi = 0; oi < static_cast<int>(set.objs.size()); ++oi) {
      const Object& o = set.objs[static_cast<size_t>(oi)];
      for (int si = 1; si < static_cast<int>(o.sections.size()); ++si) {
        const Section& s = o.sections[static_cast<size_t>(si)];
        if (s.link >= 0 && (s.flags & SHF_LINK_ORDER)) dependents[{oi, s.link}].push_back({oi, si});
        if ((s.flags & SHF_ALLOC) && is_c_identifier(s.name)) named[s.name].push_back({oi, si});
      }
      if (roots.group_members_together)
        for (const Group& gr : o.groups) for (int a : gr.members) for (int b : gr.members) if (a != b) groupmates[{oi, a}].push_back({oi, b});
    }
  }
  bool alloc(SecRef r) const { return set.sec(r).flags & SHF_ALLOC; }
  // BFS from `seeds`; `on_import(site, name)` and `on_named(name)` let the caller extend across domains.
  std::set<SecRef> run(const std::vector<SecRef>& seeds, const std::function<void(SecRef, const std::string&)>& on_import,
                       const std::function<bool(const std::string&)>& on_named) const {
    std::set<SecRef> live; std::deque<SecRef> q;
    auto enqueue = [&](SecRef r) { if (!r.valid() || !alloc(r)) return; if (live.insert(r).second) q.push_back(r); };
    for (SecRef s : seeds) enqueue(s);
    if (!seeds.empty()) { for (SecRef s : eh_sections) enqueue(s); for (SecRef t : eh_cie_targets) enqueue(t); }
    while (!q.empty()) {
      SecRef cur = q.front(); q.pop_front();
      if (auto it = adj.find(cur); it != adj.end()) for (SecRef t : it->second) enqueue(t);
      if (auto it = eh_fde_deps.find(cur); it != eh_fde_deps.end()) for (SecRef t : it->second) enqueue(t);
      if (auto it = dependents.find(cur); it != dependents.end()) for (SecRef t : it->second) enqueue(t);
      if (auto it = groupmates.find(cur); it != groupmates.end()) for (SecRef t : it->second) enqueue(t);
      if (auto it = g.imports.find(cur); it != g.imports.end()) for (auto& [name, c] : it->second) on_import(cur, name);
      if (auto it = g.start_stop_refs.find(cur); it != g.start_stop_refs.end())
        for (auto& name : it->second) { if (on_named(name)) continue; if (auto n = named.find(name); n != named.end()) for (SecRef t : n->second) enqueue(t); }
    }
    return live;
  }
  std::vector<SecRef> root_sections() const {
    std::vector<SecRef> r;
    for (int oi = 0; oi < static_cast<int>(set.objs.size()); ++oi) {
      const Object& o = set.objs[static_cast<size_t>(oi)];
      for (int si = 1; si < static_cast<int>(o.sections.size()); ++si) if (is_root_section(o.sections[static_cast<size_t>(si)], roots)) r.push_back({oi, si});
    }
    for (auto& e : roots.entry_symbols) { auto it = g.globals.defs.find(e); if (it != g.globals.defs.end()) { const Symbol& s = set.objs[static_cast<size_t>(it->second.obj)].symbols[it->second.sym]; if (s.shndx >= 0) r.push_back({it->second.obj, s.shndx}); } }
    return r;
  }
};
Liveness finish(const ObjectSet& set, std::set<SecRef> live) {
  Liveness L; L.live = std::move(live);
  for (int oi = 0; oi < static_cast<int>(set.objs.size()); ++oi) {
    const Object& o = set.objs[static_cast<size_t>(oi)];
    for (int si = 1; si < static_cast<int>(o.sections.size()); ++si) { SecRef r{oi, si}; if ((o.sections[static_cast<size_t>(si)].flags & SHF_ALLOC) && !L.live.count(r)) L.removed.push_back(r); }
  }
  return L;
}
}  // namespace

Liveness compute_liveness(const ObjectSet& set, const SectionGraph& g, const Roots& roots) {
  Engine e(set, g, roots);
  auto live = e.run(e.root_sections(), [](SecRef, const std::string&) {}, [](const std::string&) { return false; });
  return finish(set, std::move(live));
}

DomainManifest condense_domain(const ObjectSet& local, const Roots& roots, int domain_id) {
  SectionGraph g = build_section_graph(local);
  Engine e(local, g, roots);
  DomainManifest m; m.domain = domain_id;
  for (SecRef r : e.root_sections()) m.roots.insert(r);
  for (int oi = 0; oi < static_cast<int>(local.objs.size()); ++oi) {
    const Object& o = local.objs[static_cast<size_t>(oi)];
    for (uint32_t si = 1; si < o.symbols.size(); ++si) {
      const Symbol& s = o.symbols[si];
      if (s.bind == STB_LOCAL || !s.defined() || s.shndx < 0 || s.name.empty()) continue;
      auto it = g.globals.defs.find(s.name);
      if (it != g.globals.defs.end() && it->second.obj == oi && it->second.sym == si) { m.exports[s.name] = {oi, s.shndx}; m.entries.insert({oi, s.shndx}); }
    }
    for (int si = 1; si < static_cast<int>(o.sections.size()); ++si) {
      const Section& s = o.sections[static_cast<size_t>(si)];
      if ((s.flags & SHF_ALLOC) && is_c_identifier(s.name)) { m.named_sections[s.name].push_back({oi, si}); m.entries.insert({oi, si}); }
    }
  }
  for (auto& [site, syms] : g.imports) for (auto& [name, c] : syms) m.imports[site].insert(name);
  for (auto& [site, names] : g.start_stop_refs) m.start_stop[site] = names;
  std::set<SecRef> starts = m.roots; starts.insert(m.entries.begin(), m.entries.end());
  for (SecRef s : starts) {
    std::set<SecRef> reach = e.run({s}, [](SecRef, const std::string&) {}, [](const std::string&) { return false; });
    std::set<SecRef> exits;
    for (SecRef r : reach) if (m.imports.count(r) || m.start_stop.count(r)) exits.insert(r);
    m.arcs[s] = exits;
  }
  return m;
}

std::map<int, std::set<SecRef>> resolve_condensed(const std::vector<DomainManifest>& ms) {
  std::map<std::string, std::pair<int, SecRef>> exp;          // symbol -> (domain, section)
  std::map<std::string, std::vector<std::pair<int, SecRef>>> named;
  for (const auto& m : ms) {
    for (auto& [s, r] : m.exports) exp.emplace(s, std::make_pair(m.domain, r));
    for (auto& [n, v] : m.named_sections) for (SecRef r : v) named[n].push_back({m.domain, r});
  }
  std::map<int, const DomainManifest*> byid; for (auto& m : ms) byid[m.domain] = &m;
  std::map<int, std::set<SecRef>> live; std::deque<std::pair<int, SecRef>> q;
  auto enqueue = [&](int d, SecRef r) { if (live[d].insert(r).second) q.push_back({d, r}); };
  for (const auto& m : ms) for (SecRef r : m.roots) enqueue(m.domain, r);
  while (!q.empty()) {
    auto [d, e] = q.front(); q.pop_front();
    const DomainManifest& m = *byid[d];
    auto it = m.arcs.find(e); if (it == m.arcs.end()) continue;
    for (SecRef x : it->second) {
      if (auto im = m.imports.find(x); im != m.imports.end())
        for (auto& sym : im->second) {
          if (auto ex = exp.find(sym); ex != exp.end()) enqueue(ex->second.first, ex->second.second);
          else if (sym.rfind("__start_", 0) == 0 || sym.rfind("__stop_", 0) == 0) { std::string n = sym.substr(sym[2] == 's' && sym[3] == 't' && sym[4] == 'a' ? 8 : 7); for (auto& [dd, r] : named[n]) enqueue(dd, r); }
        }
      if (auto ss = m.start_stop.find(x); ss != m.start_stop.end()) for (auto& n : ss->second) for (auto& [dd, r] : named[n]) enqueue(dd, r);
    }
  }
  return live;
}

Liveness expand_domain(const ObjectSet& local, const Roots& roots, const std::set<SecRef>& live_entries) {
  SectionGraph g = build_section_graph(local);
  Engine e(local, g, roots);
  std::vector<SecRef> seeds(live_entries.begin(), live_entries.end());
  for (SecRef r : e.root_sections()) seeds.push_back(r);
  // Inside a domain, __start_/__stop_ references to locally named sections are resolved locally; cross-domain ones were
  // handled by resolve_condensed (which marked the named sections as live entries).
  auto live = e.run(seeds, [](SecRef, const std::string&) {}, [](const std::string&) { return false; });
  return finish(local, std::move(live));
}

nlohmann::json DomainManifest::json(const ObjectSet& local) const {
  auto lab = [&](SecRef r) { return local.label(r); };
  nlohmann::json j; j["domain"] = domain;
  for (SecRef r : roots) j["roots"].push_back(lab(r));
  for (SecRef r : entries) j["entries"].push_back(lab(r));
  for (auto& [e, xs] : arcs) { nlohmann::json a; for (SecRef x : xs) a.push_back(lab(x)); j["arcs"][lab(e)] = a; }
  for (auto& [x, syms] : imports) j["imports"][lab(x)] = syms;
  for (auto& [s, r] : exports) j["exports"][s] = lab(r);
  j["arc_count"] = arc_count();
  return j;
}

}  // namespace opticc::elf
