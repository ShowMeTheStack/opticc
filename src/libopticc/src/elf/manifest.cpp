// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/elf/manifest.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace opticc::elf {

RelocClass classify_reloc(uint32_t t) {
  if (t == R_X86_64_NONE) return RelocClass::None;
  if (is_pcrel_type(t)) return RelocClass::PcRel;
  if (t == R_X86_64_64 || t == R_X86_64_32 || t == R_X86_64_32S || t == R_X86_64_16 || t == R_X86_64_8) return RelocClass::Abs;
  if (t >= 16 && t <= 23) return RelocClass::Tls;          // DTPMOD64 .. TPOFF32
  if (is_got_type(t)) return RelocClass::Got;
  if (t == 34 || t == 35 || t == 36 || t == 37) return RelocClass::Tls;   // GOTPC32_TLSDESC, TLSDESC_CALL, TLSDESC, IRELATIVE(abs-ish)
  return RelocClass::Other;
}
const char* reloc_class_name(RelocClass c) {
  switch (c) { case RelocClass::None: return "none"; case RelocClass::PcRel: return "pcrel"; case RelocClass::Abs: return "abs";
               case RelocClass::Got: return "got"; case RelocClass::Tls: return "tls"; default: return "other"; }
}
void ClassCounts::add(RelocClass c, uint64_t n) {
  total += n;
  switch (c) { case RelocClass::None: none += n; break; case RelocClass::PcRel: pcrel += n; break; case RelocClass::Abs: abs += n; break;
               case RelocClass::Got: got += n; break; case RelocClass::Tls: tls += n; break; default: other += n; }
}
nlohmann::json ClassCounts::json() const { return {{"total", total}, {"pcrel", pcrel}, {"abs", abs}, {"got", got}, {"tls", tls}, {"other", other}, {"none", none}}; }

ObjectSet ObjectSet::load(const std::vector<std::string>& paths) { ObjectSet s; for (auto& p : paths) s.objs.push_back(read_object(p)); return s; }
std::string ObjectSet::label(SecRef r) const { return objs[static_cast<size_t>(r.obj)].path + ":(" + sec(r).name + ")"; }

bool is_c_identifier(const std::string& s) {
  if (s.empty()) return false;
  for (size_t i = 0; i < s.size(); ++i) { char c = s[i]; bool ok = (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i > 0 && c >= '0' && c <= '9'); if (!ok) return false; }
  return true;
}

std::vector<EhRecord> parse_eh_frame(const Section& sec, const std::vector<Rela>& relocs) {
  std::vector<EhRecord> out; const auto& d = sec.data; size_t pos = 0;
  auto rd32 = [&](size_t p) { uint32_t v; std::memcpy(&v, d.data() + p, 4); return v; };
  while (pos + 4 <= d.size()) {
    uint32_t len = rd32(pos); if (len == 0) break;
    if (len == 0xffffffffu) throw std::runtime_error(".eh_frame with 64-bit length records is not supported");
    EhRecord r; r.off = pos; r.end = pos + 4 + len; if (r.end > d.size()) throw std::runtime_error("truncated .eh_frame");
    r.fde = rd32(pos + 4) != 0;
    for (const Rela& x : relocs) if (x.offset >= r.off && x.offset < r.end) { if (r.fde && x.offset == r.off + 8) r.pc_begin = &x; else r.others.push_back(&x); }
    out.push_back(r); pos = r.end;
  }
  return out;
}

GlobalTable resolve_globals(const ObjectSet& set) {
  GlobalTable t;
  for (int oi = 0; oi < static_cast<int>(set.objs.size()); ++oi) {
    const Object& o = set.objs[static_cast<size_t>(oi)];
    for (uint32_t si = 1; si < o.symbols.size(); ++si) {
      const Symbol& s = o.symbols[si];
      if (s.bind == STB_LOCAL || s.name.empty()) continue;
      if (!s.defined()) continue;
      auto it = t.defs.find(s.name);
      if (it == t.defs.end()) { t.defs[s.name] = {oi, si}; continue; }
      const Symbol& prev = set.objs[static_cast<size_t>(it->second.obj)].symbols[it->second.sym];
      bool prev_strong = prev.bind == STB_GLOBAL && prev.special != SHN_COMMON, cur_strong = s.bind == STB_GLOBAL && s.special != SHN_COMMON;
      if (cur_strong && !prev_strong) it->second = {oi, si};
      else if (cur_strong && prev_strong) { auto& d = t.duplicates[s.name]; if (d.empty()) d.push_back(it->second); d.push_back({oi, si}); }
      else if (!cur_strong && !prev_strong && prev.special == SHN_COMMON && s.special == SHN_COMMON && s.size > prev.size) it->second = {oi, si};
    }
  }
  for (const Object& o : set.objs)
    for (uint32_t si = 1; si < o.symbols.size(); ++si) { const Symbol& s = o.symbols[si]; if (s.undefined() && !s.name.empty() && !t.defs.count(s.name)) t.undefined.insert(s.name); }
  return t;
}

SecRef SectionGraph::target_of(const ObjectSet& set, int obj, uint32_t symidx) const {
  const Symbol& s = set.objs[static_cast<size_t>(obj)].symbols[symidx];
  if (s.shndx >= 0) return {obj, s.shndx};
  if (s.undefined() && !s.name.empty()) { auto it = globals.defs.find(s.name); if (it != globals.defs.end()) { const Symbol& d = set.objs[static_cast<size_t>(it->second.obj)].symbols[it->second.sym]; if (d.shndx >= 0) return {it->second.obj, d.shndx}; } }
  return {};
}

SectionGraph build_section_graph(const ObjectSet& set) {
  SectionGraph g; g.globals = resolve_globals(set);
  for (int oi = 0; oi < static_cast<int>(set.objs.size()); ++oi) {
    const Object& o = set.objs[static_cast<size_t>(oi)];
    for (const auto& [target, vec] : o.relocs) {
      SecRef site{oi, target};
      for (const Rela& r : vec) {
        RelocClass c = classify_reloc(r.type);
        const Symbol& s = o.symbols[r.sym];
        SecRef t = g.target_of(set, oi, r.sym);
        if (t.valid()) g.arcs[{site, t}].add(c);
        else if (s.undefined() && !s.name.empty()) {
          g.imports[site][s.name].add(c);
          if (s.name.rfind("__start_", 0) == 0) g.start_stop_refs[site].insert(s.name.substr(8));
          else if (s.name.rfind("__stop_", 0) == 0) g.start_stop_refs[site].insert(s.name.substr(7));
        }
        // ABS/COMMON targets: no section arc
      }
    }
  }
  return g;
}

nlohmann::json manifest_json(const ObjectSet& set, const SectionGraph& g) {
  nlohmann::json exports = nlohmann::json::array(), imports = nlohmann::json::array(), secs = nlohmann::json::array();
  ClassCounts census; uint64_t bytes = 0;
  std::set<std::string> imp;
  for (int oi = 0; oi < static_cast<int>(set.objs.size()); ++oi) {
    const Object& o = set.objs[static_cast<size_t>(oi)];
    for (uint32_t si = 1; si < o.symbols.size(); ++si) {
      const Symbol& s = o.symbols[si];
      if (s.bind == STB_LOCAL || s.name.empty()) continue;
      if (s.defined()) {
        auto it = g.globals.defs.find(s.name);
        if (it != g.globals.defs.end() && it->second.obj == oi && it->second.sym == si)
          exports.push_back({{"sym", s.name}, {"obj", o.path}, {"sec", section_name_of(o, s)}, {"bind", s.bind == STB_WEAK ? "weak" : "global"},
                             {"vis", s.visibility()}, {"type", s.type}, {"size", s.size}});
      } else if (g.globals.undefined.count(s.name)) imp.insert(s.name);
    }
    for (size_t k = 1; k < o.sections.size(); ++k) { const Section& s = o.sections[k]; if (s.flags & SHF_ALLOC) bytes += (s.type == SHT_NOBITS ? 0 : s.size); secs.push_back({{"obj", o.path}, {"name", s.name}, {"size", s.size}, {"flags", s.flags}}); }
    for (const auto& [target, vec] : o.relocs) for (const Rela& r : vec) census.add(classify_reloc(r.type));
  }
  for (auto& n : imp) imports.push_back(n);
  nlohmann::json dups = nlohmann::json::array(); for (auto& [n, v] : g.globals.duplicates) dups.push_back(n);
  return {{"exports", exports}, {"imports", imports}, {"relocs", census.json()}, {"sections", secs}, {"alloc_bytes", bytes}, {"duplicate_strong_definitions", dups}};
}

}  // namespace opticc::elf
