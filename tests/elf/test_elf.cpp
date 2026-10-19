// SPDX-License-Identifier: GPL-2.0-only
#include "test_harness.hpp"
#include "elf_test_util.hpp"
#include "opticc/elf/manifest.hpp"

#include <algorithm>
#include <set>

using namespace opticc::elf;
using namespace elftest;

namespace {
// Content signature of a relocatable object independent of section/symbol indices and ordering.
std::string obj_signature(const Object& o) {
  std::vector<std::string> lines;
  for (size_t i = 1; i < o.sections.size(); ++i) {
    const Section& s = o.sections[i]; std::string d; for (uint8_t c : s.data) { char b[3]; std::snprintf(b, sizeof b, "%02x", c); d += b; }
    lines.push_back("S " + s.name + " t=" + std::to_string(s.type) + " f=" + std::to_string(s.flags) + " sz=" + std::to_string(s.size) + " al=" + std::to_string(s.addralign) + " es=" + std::to_string(s.entsize) + " link=" + (s.link >= 0 ? o.sections[static_cast<size_t>(s.link)].name : "-") + " " + d);
  }
  for (size_t i = 1; i < o.symbols.size(); ++i) { const Symbol& s = o.symbols[i]; lines.push_back("Y " + s.name + " b=" + std::to_string(s.bind) + " t=" + std::to_string(s.type) + " o=" + std::to_string(s.other) + " sec=" + section_name_of(o, s) + " v=" + std::to_string(s.value) + " sz=" + std::to_string(s.size)); }
  for (auto& [t, v] : o.relocs) for (const Rela& r : v) lines.push_back("R " + o.sections[static_cast<size_t>(t)].name + " off=" + std::to_string(r.offset) + " " + reloc_name(r.type) + " sym=" + o.symbols[r.sym].name + " a=" + std::to_string(r.addend));
  for (const Group& g : o.groups) { std::string m; for (int x : g.members) m += o.sections[static_cast<size_t>(x)].name + ","; lines.push_back("G " + o.symbols[g.signature].name + " f=" + std::to_string(g.flags) + " " + m); }
  std::sort(lines.begin(), lines.end()); std::string out; for (auto& l : lines) out += l + "\n"; return out;
}
}  // namespace

OPTICC_TEST(elf_roundtrip_content_identical) {
  std::string dir = tmpdir("opticc-elf-rt-"); Program P = gen_program(dir);
  int n = 0;
  for (auto& obj : P.objs) {
    Object a = read_object(obj); std::string rt = obj + ".rt.o"; write_object_file(a, rt); Object b = read_object(rt);
    CHECK(obj_signature(a) == obj_signature(b));
    // llvm-readelf must accept it and agree on symbol/relocation listings (modulo indices)
    Run ra = run(tool("llvm-readelf") + " -s -r -W " + obj + " | awk '{$1=\"\"; print}' | grep -v 'Symbol table\\|Relocation section\\|Num:\\|Offset' | sort");
    Run rb = run(tool("llvm-readelf") + " -s -r -W " + rt + " | awk '{$1=\"\"; print}' | grep -v 'Symbol table\\|Relocation section\\|Num:\\|Offset' | sort");
    CHECK(ra.rc == 0 && rb.rc == 0);
    // relocation lines reference symbol *names*; section symbols print the section name on both sides
    std::set<std::string> la, lb; { std::istringstream s(ra.out); std::string l; while (std::getline(s, l)) if (l.find("R_X86_64") != std::string::npos) la.insert(l); }
    { std::istringstream s(rb.out); std::string l; while (std::getline(s, l)) if (l.find("R_X86_64") != std::string::npos) lb.insert(l); }
    CHECK(la == lb);
    n++;
  }
  CHECK(n >= 15);
  std::printf("  roundtripped %d objects\n", n);
}

OPTICC_TEST(elf_roundtrip_links_identically) {
  std::string dir = tmpdir("opticc-elf-rtl-"); Program P = gen_program(dir);
  std::vector<std::string> rts;
  for (auto& obj : P.objs) { std::string rt = obj + ".rt.o"; write_object_file(read_object(obj), rt); rts.push_back(rt); }
  Run a = lld_link(P.objs, dir + "/a.out", false); Run b = lld_link(rts, dir + "/b.out", false);
  CHECK(a.rc == 0); CHECK(b.rc == 0);
  if (a.rc || b.rc) std::printf("%s\n%s\n", a.out.c_str(), b.out.c_str());
  CHECK(read(dir + "/a.out") == read(dir + "/b.out"));
  // ld.lld -r must accept the round-tripped objects too
  Run r = run(tool("ld.lld") + " -r -o " + dir + "/r.o " + join(rts)); CHECK(r.rc == 0);
}

OPTICC_TEST(manifest_classifies_relocs) {
  std::string dir = tmpdir("opticc-elf-man-"); Program P = gen_program(dir);
  ObjectSet set = ObjectSet::load(P.objs); SectionGraph g = build_section_graph(set);
  auto j = manifest_json(set, g);
  auto rel = j["relocs"];
  CHECK(rel["pcrel"].get<uint64_t>() > 0); CHECK(rel["abs"].get<uint64_t>() > 0); CHECK(rel["got"].get<uint64_t>() > 0);
  CHECK(rel["total"] == rel["pcrel"].get<uint64_t>() + rel["abs"].get<uint64_t>() + rel["got"].get<uint64_t>() + rel["tls"].get<uint64_t>() + rel["other"].get<uint64_t>() + rel["none"].get<uint64_t>());
  std::set<std::string> imports; for (auto& i : j["imports"]) imports.insert(i.get<std::string>());
  CHECK(imports.count("__start_mytab") && imports.count("__stop_mytab"));   // only linker-defined symbols are unresolved
  CHECK(imports.size() == 2);
  CHECK(j["duplicate_strong_definitions"].empty());
  bool has_f0 = false; for (auto& e : j["exports"]) if (e["sym"] == "f0") has_f0 = true; CHECK(has_f0);
  // the start/stop references were recorded against the entry section
  bool ss = false; for (auto& [site, names] : g.start_stop_refs) if (names.count("mytab")) ss = true; CHECK(ss);
  std::printf("  relocs: %s\n", rel.dump().c_str());
}
