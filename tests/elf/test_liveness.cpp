// SPDX-License-Identifier: GPL-2.0-only
#include "test_harness.hpp"
#include "elf_test_util.hpp"
#include "opticc/elf/liveness.hpp"

#include <random>

using namespace opticc::elf;
using namespace elftest;

namespace {
// lld's removed set: lines "removing unused section <obj>:(<sec>)"
std::set<std::string> lld_removed(const Program& P, const std::string& out) {
  Run r = lld_link(P.objs, out, true, "--print-gc-sections"); if (r.rc) throw std::runtime_error("lld gc link failed: " + r.out);
  std::set<std::string> s; std::istringstream is(r.out); std::string l;
  while (std::getline(is, l)) { auto p = l.find("removing unused section "); if (p == std::string::npos) continue; s.insert(l.substr(p + 24)); }
  return s;
}
std::set<std::string> ours_removed(const ObjectSet& set, const Liveness& L) { std::set<std::string> s; for (SecRef r : L.removed) s.insert(set.label(r)); return s; }
void diff(const std::set<std::string>& a, const std::set<std::string>& b, const char* an, const char* bn) {
  for (auto& x : a) if (!b.count(x)) std::printf("  only in %s: %s\n", an, x.c_str());
  for (auto& x : b) if (!a.count(x)) std::printf("  only in %s: %s\n", bn, x.c_str());
}
}  // namespace

OPTICC_TEST(liveness_matches_lld_gc_sections) {
  for (bool eh : {false, true}) {
    std::string dir = tmpdir("opticc-live-"); Program P = gen_program(dir, eh);
    ObjectSet set = ObjectSet::load(P.objs); SectionGraph g = build_section_graph(set);
    Roots roots; roots.entry_symbols = {"_start"};
    Liveness L = compute_liveness(set, g, roots);
    auto lld = lld_removed(P, dir + "/gc.out"), ours = ours_removed(set, L);
    // lld does not list .eh_frame pieces as sections; compare everything else
    std::set<std::string> ours_cmp; for (auto& x : ours) if (x.find("(.eh_frame)") == std::string::npos) ours_cmp.insert(x);
    if (lld != ours_cmp) diff(lld, ours_cmp, "lld", "ours");
    CHECK(lld == ours_cmp);
    CHECK(!ours.empty());   // dead functions exist by construction
    bool dead0 = false; for (auto& x : ours) if (x.find("(.text.deadglob0)") != std::string::npos) dead0 = true; CHECK(dead0);
    std::printf("  eh=%d: lld removed %zu, ours %zu, live %zu\n", eh, lld.size(), ours_cmp.size(), L.live.size());
  }
}

OPTICC_TEST(liveness_comdat_and_start_stop) {
  std::string dir = tmpdir("opticc-live2-"); Program P = gen_program(dir);
  ObjectSet set = ObjectSet::load(P.objs); SectionGraph g = build_section_graph(set);
  Roots roots; roots.entry_symbols = {"_start"};
  Liveness L = compute_liveness(set, g, roots);
  auto ours = ours_removed(set, L);
  // every "mytab" section is live because _start references __start_mytab/__stop_mytab
  for (SecRef r : L.live) (void)r;
  int mytab_live = 0, mytab_total = 0;
  for (int o = 0; o < static_cast<int>(set.objs.size()); ++o) for (int s = 1; s < static_cast<int>(set.objs[static_cast<size_t>(o)].sections.size()); ++s)
    if (set.objs[static_cast<size_t>(o)].sections[static_cast<size_t>(s)].name == "mytab") { mytab_total++; if (L.live.count({o, s})) mytab_live++; }
  CHECK(mytab_total == 3 && mytab_live == 3);
  // the unused COMDAT inline (unused_inl) is dead, the shared one (inl_shared) is live in exactly the copy lld keeps
  bool unused_dead = false, shared_live = false;
  for (auto& x : ours) if (x.find("unused_inl") != std::string::npos) unused_dead = true;
  for (SecRef r : L.live) if (set.sec(r).name.find("inl_shared") != std::string::npos) shared_live = true;
  CHECK(unused_dead); CHECK(shared_live);
  auto lld = lld_removed(P, dir + "/gc.out");
  // lld discards the duplicate COMDAT copy before gc, so it reports it as removed only if the kept copy is dead too;
  // our per-section liveness marks both copies live when referenced; the prelink's COMDAT dedup discards the duplicate.
  std::set<std::string> lld_cmp, ours_cmp;
  for (auto& x : lld) if (x.find("inl_shared") == std::string::npos && x.find("tmpl") == std::string::npos) lld_cmp.insert(x);
  for (auto& x : ours) if (x.find("inl_shared") == std::string::npos && x.find("tmpl") == std::string::npos) ours_cmp.insert(x);
  if (lld_cmp != ours_cmp) diff(lld_cmp, ours_cmp, "lld", "ours");
  CHECK(lld_cmp == ours_cmp);
}

OPTICC_TEST(liveness_condensed_equals_full) {
  std::string dir = tmpdir("opticc-live3-"); Program P = gen_program(dir);
  ObjectSet set = ObjectSet::load(P.objs); SectionGraph g = build_section_graph(set);
  Roots roots; roots.entry_symbols = {"_start"};
  Liveness full = compute_liveness(set, g, roots);
  std::set<std::string> full_live; for (SecRef r : full.live) full_live.insert(set.label(r));
  for (unsigned seed = 1; seed <= 5; ++seed) {
    std::mt19937 rng(seed); std::map<int, std::vector<int>> members;
    for (int o = 0; o < static_cast<int>(set.objs.size()); ++o) members[static_cast<int>(rng() % 3)].push_back(o);
    std::vector<DomainManifest> ms; std::map<int, ObjectSet> locals; size_t arcs = 0;
    for (auto& [d, vs] : members) { ObjectSet loc; for (int o : vs) loc.objs.push_back(set.objs[static_cast<size_t>(o)]); DomainManifest m = condense_domain(loc, roots, d); arcs += m.arc_count(); ms.push_back(m); locals[d] = std::move(loc); }
    auto live_entries = resolve_condensed(ms);
    std::set<std::string> cond_live;
    for (auto& [d, vs] : members) { Liveness L = expand_domain(locals[d], roots, live_entries[d]); for (SecRef r : L.live) cond_live.insert(locals[d].label(r)); }
    if (cond_live != full_live) diff(full_live, cond_live, "full", "condensed");
    CHECK(cond_live == full_live);
    std::printf("  seed %u: %zu domains, %zu condensed arcs, %zu live sections (full %zu)\n", seed, members.size(), arcs, cond_live.size(), full_live.size());
  }
}


#include "opticc/elf/wire.hpp"
// The condensed manifest must survive the PRELINK wire format unchanged (coordinator <-> worker, distributed form).
OPTICC_TEST(liveness_wire_roundtrip) {
  std::string dir = tmpdir("opticc-wire");
  Program P = gen_program(dir, false, 6);
  ObjectSet set = ObjectSet::load(P.objs);
  Roots roots; roots.entry_symbols = {"_start"};
  // three random domains, as the pipeline would form them
  std::vector<DomainManifest> direct, viawire;
  for (int d = 0; d < 3; ++d) {
    std::vector<std::string> ps; for (size_t i = 0; i < P.objs.size(); ++i) if (static_cast<int>(i % 3) == d) ps.push_back(P.objs[i]);
    ObjectSet local = ObjectSet::load(ps);
    DomainManifest m = condense_domain(local, roots, d);
    direct.push_back(m);
    nlohmann::json wj = domain_manifest_wire(m);
    std::printf("  wire keys: %s named=%s exports=%zu\n", [&]{ std::string k; for (auto& [kk, vv] : wj.items()) k += kk + " "; return k; }().c_str(), wj["named_sections"].dump().substr(0, 120).c_str(), wj["exports"].size());
    DomainManifest back = domain_manifest_from_wire(wj);
    std::printf("  back: exports=%zu named=%zu\n", back.exports.size(), back.named_sections.size());
    CHECK(back.domain == m.domain); CHECK(back.roots == m.roots); CHECK(back.entries == m.entries); CHECK(back.arcs == m.arcs);
    CHECK(back.imports == m.imports); CHECK(back.exports == m.exports); CHECK(back.start_stop == m.start_stop); CHECK(back.named_sections == m.named_sections);
    if (back.named_sections != m.named_sections) {
      std::printf("  named_sections: direct %zu entries, wire %zu entries\n", m.named_sections.size(), back.named_sections.size());
      for (auto& [n, v] : m.named_sections) { auto it = back.named_sections.find(n); std::printf("    '%s' direct=%zu wire=%s\n", n.c_str(), v.size(), it == back.named_sections.end() ? "MISSING" : std::to_string(it->second.size()).c_str()); }
      for (auto& [n, v] : back.named_sections) if (!m.named_sections.count(n)) std::printf("    extra in wire: '%s' (%zu)\n", n.c_str(), v.size());
    }
    viawire.push_back(back);
  }
  auto a = resolve_condensed(direct), b = resolve_condensed(viawire);
  CHECK(a == b);
  size_t live = 0; for (auto& [d, s] : a) live += s.size();
  CHECK(live > 0);
  std::printf("  wire roundtrip: %zu live entries across 3 domains\n", live);
}
