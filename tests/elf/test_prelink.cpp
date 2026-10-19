// SPDX-License-Identifier: GPL-2.0-only
#include "test_harness.hpp"
#include "elf_test_util.hpp"
#include "opticc/elf/liveness.hpp"
#include "opticc/elf/prelink.hpp"

#include <random>

using namespace opticc::elf;
using namespace elftest;

namespace {
struct Domains { std::map<int, std::vector<int>> members; std::vector<int> of; };
Domains random_domains(int nobj, int k, unsigned seed) {
  Domains d; d.of.resize(static_cast<size_t>(nobj)); std::mt19937 rng(seed);
  for (int o = 0; o < nobj; ++o) { int p = static_cast<int>(rng() % static_cast<unsigned>(k)); d.of[static_cast<size_t>(o)] = p; d.members[p].push_back(o); }
  return d;
}
// Prelink every domain of `set` (gc with the full liveness, proximity resolution on); returns domain object paths in domain order
// and the original objects reordered the same way.
struct PrelinkedProgram { std::vector<std::string> domain_objs, originals_in_domain_order; std::vector<PrelinkResult> results; };
PrelinkedProgram prelink_all(const ObjectSet& set, const Domains& d, const Roots& roots, const std::string& dir, bool gc, bool apply, bool exports) {
  SectionGraph g = build_section_graph(set); Liveness L = compute_liveness(set, g, roots);
  // exports per domain: symbols imported by any other domain (what the coordinator derives from the manifest union)
  std::map<int, std::set<std::string>> needed;
  for (auto& [dom, vs] : d.members) {
    std::set<int> mine(vs.begin(), vs.end());
    for (int o : vs) { const Object& ob = set.objs[static_cast<size_t>(o)]; for (auto& s : ob.symbols) if (s.undefined() && !s.name.empty()) { auto it = g.globals.defs.find(s.name); if (it != g.globals.defs.end() && !mine.count(it->second.obj)) needed[d.of[static_cast<size_t>(it->second.obj)]].insert(s.name); } }
  }
  PrelinkedProgram out;
  for (auto& [dom, vs] : d.members) {
    ObjectSet loc; std::set<SecRef> live;
    for (size_t i = 0; i < vs.size(); ++i) { loc.objs.push_back(set.objs[static_cast<size_t>(vs[i])]); out.originals_in_domain_order.push_back(set.objs[static_cast<size_t>(vs[i])].path);
      for (SecRef r : L.live) if (r.obj == vs[i]) live.insert({static_cast<int>(i), r.sec}); }
    PrelinkOptions po; po.gc = gc; po.apply_pcrel = apply; po.have_exports = exports; po.exports = needed[dom]; po.entry_symbols = {"_start"}; po.origin_tag = "d" + std::to_string(dom);
    PrelinkResult r = prelink(loc, gc ? &live : nullptr, po);
    std::string p = dir + "/domain" + std::to_string(dom) + ".o"; write_object_file(r.out, p);
    out.domain_objs.push_back(p); out.results.push_back(std::move(r));
  }
  return out;
}
}  // namespace

OPTICC_TEST(prelink_residual_equals_cut) {
  std::string dir = tmpdir("opticc-pl-cut-"); Program P = gen_program(dir);
  ObjectSet set = ObjectSet::load(P.objs); SectionGraph g = build_section_graph(set);
  Roots roots; roots.entry_symbols = {"_start"};
  Domains d = random_domains(static_cast<int>(set.objs.size()), 3, 7);
  PrelinkedProgram pp = prelink_all(set, d, roots, dir, /*gc*/ false, /*apply*/ true, /*exports*/ false);
  // Independent count of cut PC-relative relocations: pcrel relocations in a domain whose target is in another domain or undefined
  std::map<int, uint64_t> cut;
  for (auto& [key, c] : g.arcs) { int ds = d.of[static_cast<size_t>(key.first.obj)], dt = d.of[static_cast<size_t>(key.second.obj)]; if (ds != dt) cut[ds] += c.pcrel; }
  for (auto& [site, syms] : g.imports) for (auto& [name, c] : syms) cut[d.of[static_cast<size_t>(site.obj)]] += c.pcrel;
  size_t i = 0; uint64_t applied_total = 0;
  for (auto& [dom, vs] : d.members) {
    const PrelinkStats& s = pp.results[i++].stats;
    CHECK(s.residual_cut_pcrel == cut[dom]);
    CHECK(s.applied_pcrel + s.residual.pcrel == s.input.pcrel);     // conservation: every PC-relative relocation is applied or kept
    CHECK(s.residual.abs == s.input.abs); CHECK(s.residual.got == s.input.got);   // absolute/GOT relocations are never applied
    CHECK(s.applied_pcrel > 0);
    applied_total += s.applied_pcrel;
    std::printf("  domain %d: %zu objs, pcrel in %llu applied %llu residual cut %llu internal %llu, abs %llu got %llu\n", dom, vs.size(),
                (unsigned long long)s.input.pcrel, (unsigned long long)s.applied_pcrel, (unsigned long long)s.residual_cut_pcrel, (unsigned long long)s.residual_internal_pcrel, (unsigned long long)s.residual.abs, (unsigned long long)s.residual.got);
  }
  CHECK(applied_total > 0);
  // manifest tool agrees: residual count reported == kept relocations in the written object
  Object d0 = read_object(pp.domain_objs[0]); uint64_t kept = 0; for (auto& [t, v] : d0.relocs) kept += v.size();
  CHECK(kept == pp.results[0].stats.residual.total);
}

OPTICC_TEST(prelink_differential_20_seeds) {
  for (bool eh : {false, true}) {
    int seeds = eh ? 3 : 20;
    std::string dir = tmpdir("opticc-pl-diff-"); Program P = gen_program(dir, eh);
    ObjectSet set = ObjectSet::load(P.objs);
    Roots roots; roots.entry_symbols = {"_start"};
    int ok = 0;
    for (unsigned seed = 1; seed <= static_cast<unsigned>(seeds); ++seed) {
      std::string sd = dir + "/s" + std::to_string(seed); mkdir(sd.c_str(), 0755);
      Domains d = random_domains(static_cast<int>(set.objs.size()), 3, seed);
      PrelinkedProgram pp = prelink_all(set, d, roots, sd, true, true, true);
      Run a = lld_link(pp.originals_in_domain_order, sd + "/orig.out", true);
      Run b = lld_link(pp.domain_objs, sd + "/dom.out", true);
      if (a.rc || b.rc) { std::printf("  seed %u link failed:\n%s\n%s\n", seed, a.out.c_str(), b.out.c_str()); CHECK(false); continue; }
      std::string sa = link_signature(sd + "/orig.out", sd), sb = link_signature(sd + "/dom.out", sd), why;
      if (!signatures_equal(sa, sb, &why)) { std::printf("  seed %u: MISMATCH: %s\n", seed, why.c_str()); CHECK(false); } else ok++;
      // the prelinked domains also survive a further `ld.lld -r` (relocatable output stays well-formed)
      Run r = run(tool("ld.lld") + " -r -o " + sd + "/all.o " + join(pp.domain_objs)); CHECK(r.rc == 0);
    }
    uint64_t applied = 0, residual = 0;
    std::printf("  eh=%d: %d/%d seeds byte-identical (alloc sections, symbol addresses, disassembly)\n", eh, ok, seeds);
    (void)applied; (void)residual;
  }
}

OPTICC_TEST(prelink_fails_loudly) {
  std::string dir = tmpdir("opticc-pl-fail-"); Program P = gen_program(dir);
  ObjectSet set = ObjectSet::load(P.objs);
  Roots roots; roots.entry_symbols = {"_start"};
  // (1) gc without a live set
  { PrelinkOptions po; po.gc = true; bool threw = false; try { prelink(set, nullptr, po); } catch (const std::exception& e) { threw = std::string(e.what()).find("live set") != std::string::npos; } CHECK(threw); }
  // (2) duplicate strong definitions
  { ObjectSet dup; dup.objs.push_back(set.objs[1]); dup.objs.push_back(set.objs[1]); PrelinkOptions po; po.gc = false; bool threw = false;
    try { prelink(dup, nullptr, po); } catch (const std::exception& e) { threw = std::string(e.what()).find("multiple strong definitions") != std::string::npos; } CHECK(threw); }
  // (3) a live set that drops a section still referenced from a live one
  { SectionGraph g = build_section_graph(set); Liveness L = compute_liveness(set, g, roots); std::set<SecRef> live = L.live;
    // remove f1's section (referenced by f0 and cppA, which are live)
    for (auto it = live.begin(); it != live.end();) { if (set.sec(*it).name == ".text.f1") it = live.erase(it); else ++it; }
    PrelinkOptions po; po.gc = true; bool threw = false;
    try { prelink(set, &live, po); } catch (const std::exception& e) { threw = std::string(e.what()).find("references dropped section") != std::string::npos; if (!threw) std::printf("  msg: %s\n", e.what()); } CHECK(threw); }
  // (4) the tool refuses a non-ELF / 32-bit input
  { write(dir + "/junk.o", "not an elf"); bool threw = false; try { read_object(dir + "/junk.o"); } catch (const std::exception&) { threw = true; } CHECK(threw); }
}

OPTICC_TEST(prelink_cli_smoke) {
  std::string dir = tmpdir("opticc-pl-cli-"); Program P = gen_program(dir);
  write(dir + "/roots.json", "{\"entry\":[\"_start\"]}");
  std::string objs = join(P.objs);
  Run m = run(std::string(OPTICC_PRELINK_BIN) + " manifest --roots " + dir + "/roots.json --domain 0 --out " + dir + "/m.json " + objs); CHECK(m.rc == 0);
  Run l = run(std::string(OPTICC_PRELINK_BIN) + " liveness --roots " + dir + "/roots.json --out " + dir + "/live.json " + objs); CHECK(l.rc == 0);
  // condensed path through the CLI must give the same live set
  std::string dj = "{"; for (size_t i = 0; i < P.objs.size(); ++i) dj += (i ? "," : "") + std::string("\"") + P.objs[i] + "\":" + std::to_string(i % 3); dj += "}"; write(dir + "/domains.json", dj);
  Run lc = run(std::string(OPTICC_PRELINK_BIN) + " liveness --roots " + dir + "/roots.json --domains " + dir + "/domains.json --out " + dir + "/live-c.json " + objs); CHECK(lc.rc == 0);
  nlohmann::json a = nlohmann::json::parse(read(dir + "/live.json")), b = nlohmann::json::parse(read(dir + "/live-c.json"));
  CHECK(a["live"] == b["live"]);
  std::vector<std::string> doms;
  for (int d = 0; d < 3; ++d) {
    std::string in; for (size_t i = 0; i < P.objs.size(); ++i) if (i % 3 == static_cast<size_t>(d)) in += " " + P.objs[i];
    Run pr = run(std::string(OPTICC_PRELINK_BIN) + " prelink --domain " + std::to_string(d) + " --roots " + dir + "/roots.json --live " + dir + "/live.json -o " + dir + "/d" + std::to_string(d) + ".o --manifest " + dir + "/d" + std::to_string(d) + ".json" + in);
    if (pr.rc) std::printf("%s\n", pr.out.c_str()); CHECK(pr.rc == 0); doms.push_back(dir + "/d" + std::to_string(d) + ".o");
    nlohmann::json mj = nlohmann::json::parse(read(dir + "/d" + std::to_string(d) + ".json")); CHECK(mj["applied_pcrel"].get<int>() >= 0); CHECK(mj.contains("exports") && mj.contains("imports"));
  }
  Run link = lld_link(doms, dir + "/cli.out", true); CHECK(link.rc == 0);
  Run dm = run(std::string(OPTICC_DOMAINS_BIN) + " --graph " + dir + "/nonexistent.json --parts 2"); CHECK(dm.rc != 0);   // fails loudly on a missing graph
}
