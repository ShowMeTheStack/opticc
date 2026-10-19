// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/elf/prelink.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace opticc::elf {

nlohmann::json PrelinkStats::json() const {
  return {{"input", input.json()}, {"residual", residual.json()}, {"applied_pcrel", applied_pcrel},
          {"residual_cut_pcrel", residual_cut_pcrel}, {"residual_internal_pcrel", residual_internal_pcrel},
          {"sections_in", sections_in}, {"sections_dropped_dead", sections_dropped_dead}, {"sections_dropped_comdat", sections_dropped_comdat},
          {"sections_out", sections_out}, {"symbols_in", symbols_in}, {"symbols_out", symbols_out}, {"localised", localised},
          {"stripped", stripped}, {"fdes_dropped", fdes_dropped}, {"alloc_bytes_in", alloc_bytes_in}, {"alloc_bytes_out", alloc_bytes_out}};
}

std::string output_section_name(const Section& s, bool keep_function_sections, bool fold_data) {
  const std::string& n = s.name;
  if (keep_function_sections || (s.flags & SHF_MERGE) || (s.flags & SHF_GROUP) || (s.flags & SHF_LINK_ORDER)) return n;
  if (n.find("..") != std::string::npos) return n;                       // kernel special sections (.data..percpu etc.)
  auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
  auto fold = [&](const char* base) { std::string b(base); if (n == b || starts((b + ".").c_str())) return b; return std::string(); };
  if (n == ".text" || starts(".text.")) {
    for (const char* k : {".text.unlikely", ".text.hot", ".text.startup", ".text.exit", ".text.split"}) { auto f = fold(k); if (!f.empty()) return f; }
    if (starts(".text.sorted.")) return n;
    return ".text";
  }
  if (fold_data)
    for (const char* k : {".data.rel.ro.local", ".data.rel.ro", ".data.rel.local", ".data.rel", ".data", ".rodata", ".bss", ".tbss", ".tdata"}) { auto f = fold(k); if (!f.empty()) return f; }
  return n;
}

namespace {

struct InSec {
  bool present = false;        // index >= 1
  bool dead = false;           // dropped by gc
  bool discarded = false;      // duplicate COMDAT
  bool always_drop = false;    // .llvm_addrsig etc.
  int group = -1;              // index into kept-group list, or -1
  int out = -1;                // output section index
  uint64_t off = 0;            // offset inside the output section
  bool kept() const { return present && !dead && !discarded && !always_drop; }
};

struct Builder {
  ObjectSet d;                              // mutable copy of the domain
  const PrelinkOptions& opt;
  const std::set<SecRef>* live;
  PrelinkStats st;
  GlobalTable globals;
  std::vector<std::vector<InSec>> in;       // [obj][sec]
  struct KeptGroup { int obj; size_t gidx; };
  std::vector<KeptGroup> kept_groups;
  Object out;
  std::map<std::string, int> outkey;
  std::vector<int> secsym;                  // output section index -> section symbol index (0 = not yet)
  std::vector<std::vector<uint32_t>> symmap;  // [obj][sym] -> out symbol (0 = dropped)
  std::vector<uint64_t> refcount;           // per out symbol

  Builder(const ObjectSet& domain, const std::set<SecRef>* l, const PrelinkOptions& o) : d(domain), opt(o), live(l) {}
  [[noreturn]] void fail(const std::string& m) const { throw std::runtime_error("opticc-prelink: " + m); }
  Object& obj(int i) { return d.objs[static_cast<size_t>(i)]; }
  InSec& is(int o, int s) { return in[static_cast<size_t>(o)][static_cast<size_t>(s)]; }
  std::string lab(int o, int s) const { return d.objs[static_cast<size_t>(o)].path + ":(" + d.objs[static_cast<size_t>(o)].sections[static_cast<size_t>(s)].name + ")"; }

  // Input section (obj, sec) holding the definition of symbol (o, si) after global resolution; {-1,-1} if none.
  std::pair<int, int> def_section(int o, uint32_t si, const Symbol** defsym = nullptr) {
    const Symbol& s = obj(o).symbols[si];
    // A global/weak symbol defined in a discarded duplicate COMDAT group resolves to the kept copy (same name).
    bool in_discarded = s.shndx >= 0 && in.size() > static_cast<size_t>(o) && in[static_cast<size_t>(o)].size() > static_cast<size_t>(s.shndx) && is(o, s.shndx).discarded;
    if (s.shndx >= 0 && !(in_discarded && s.bind != STB_LOCAL)) { if (defsym) *defsym = &s; return {o, s.shndx}; }
    if ((s.undefined() || in_discarded) && !s.name.empty()) {
      auto it = globals.defs.find(s.name);
      if (it != globals.defs.end()) { const Symbol& ds = obj(it->second.obj).symbols[it->second.sym]; if (defsym) *defsym = &ds; if (ds.shndx >= 0) return {it->second.obj, ds.shndx}; }
    }
    if (defsym) *defsym = &s;
    return {-1, -1};
  }

  void init() {
    if (d.objs.empty()) fail("no input objects");
    globals = resolve_globals(d);
    if (!globals.duplicates.empty()) { std::string m = "multiple strong definitions of:"; for (auto& [n, v] : globals.duplicates) m += " " + n; fail(m); }
    in.resize(d.objs.size());
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o) {
      in[static_cast<size_t>(o)].resize(obj(o).sections.size());
      for (size_t s = 1; s < obj(o).sections.size(); ++s) {
        InSec& x = in[static_cast<size_t>(o)][s]; x.present = true; st.sections_in++;
        const Section& sec = obj(o).sections[s];
        if (sec.flags & SHF_ALLOC) st.alloc_bytes_in += (sec.type == SHT_NOBITS ? 0 : sec.size);
        if (sec.type == SHT_LLVM_ADDRSIG || sec.type == SHT_LLVM_CALL_GRAPH_PROFILE || sec.name == ".llvm_addrsig" || sec.name == ".llvm.call-graph-profile") x.always_drop = true;
      }
      st.symbols_in += obj(o).symbols.size() - 1;
      for (const auto& [t, v] : obj(o).relocs) for (const Rela& r : v) st.input.add(classify_reloc(r.type));
    }
  }

  void comdat() {
    std::set<std::string> seen;
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o) {
      for (size_t gi = 0; gi < obj(o).groups.size(); ++gi) {
        const Group& g = obj(o).groups[gi];
        bool keep = true;
        if (g.flags & GRP_COMDAT) { const std::string& sig = obj(o).symbols[g.signature].name; if (!seen.insert(sig).second) keep = false; }
        if (keep) { kept_groups.push_back({o, gi}); for (int m : g.members) is(o, m).group = static_cast<int>(kept_groups.size()) - 1; }
        else for (int m : g.members) { is(o, m).discarded = true; st.sections_dropped_comdat++; }
      }
    }
  }

  void mark_dead() {
    if (!opt.gc) return;
    if (!live) fail("gc requested without a live set");
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o)
      for (size_t s = 1; s < obj(o).sections.size(); ++s) {
        InSec& x = in[static_cast<size_t>(o)][s]; if (!x.present || x.discarded || x.always_drop) continue;
        const Section& sec = obj(o).sections[s];
        if ((sec.flags & SHF_ALLOC) && !live->count(SecRef{o, static_cast<int>(s)})) x.dead = true;
      }
    // keep a group whole if any member is live
    for (const KeptGroup& kg : kept_groups) {
      const Group& g = obj(kg.obj).groups[kg.gidx]; bool any = false;
      for (int m : g.members) if (!is(kg.obj, m).dead) any = true;
      for (int m : g.members) if (any) is(kg.obj, m).dead = false;
    }
    for (auto& v : in) for (auto& x : v) if (x.dead) st.sections_dropped_dead++;
  }

  // Remove FDEs of dead functions from every kept .eh_frame section (CIE pointers recomputed, relocations shifted).
  void prune_eh_frame() {
    if (!opt.gc) return;
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o) {
      Object& ob = obj(o);
      for (size_t s = 1; s < ob.sections.size(); ++s) {
        Section& sec = ob.sections[s];
        if (sec.name != ".eh_frame" || !is(o, static_cast<int>(s)).kept()) continue;
        auto rit = ob.relocs.find(static_cast<int>(s)); std::vector<Rela> rels = rit == ob.relocs.end() ? std::vector<Rela>{} : rit->second;
        std::sort(rels.begin(), rels.end(), [](const Rela& a, const Rela& b) { return a.offset < b.offset; });
        std::vector<uint8_t> nd; std::vector<Rela> nr; std::map<uint64_t, uint64_t> newoff;  // old record offset -> new
        const std::vector<uint8_t>& od = sec.data; size_t pos = 0;
        auto rd32 = [&](size_t p) { uint32_t v; std::memcpy(&v, od.data() + p, 4); return v; };
        while (pos + 4 <= od.size()) {
          uint32_t len = rd32(pos); if (len == 0) { newoff[pos] = nd.size(); nd.insert(nd.end(), od.begin() + static_cast<long>(pos), od.begin() + static_cast<long>(pos) + 4); pos += 4; break; }
          if (len == 0xffffffffu) fail(".eh_frame with 64-bit length records is not supported");
          size_t rec = pos, end = pos + 4 + len; if (end > od.size()) fail("truncated .eh_frame in " + lab(o, static_cast<int>(s)));
          uint32_t cie_id = rd32(rec + 4); bool is_fde = cie_id != 0; bool drop = false;
          if (is_fde) {
            // pc_begin relocation sits at rec+8
            for (const Rela& r : rels) if (r.offset == rec + 8) { auto [to, ts] = def_section(o, r.sym); if (to >= 0 && !is(to, ts).kept()) drop = true; break; }
          }
          if (drop) { st.fdes_dropped++; pos = end; continue; }
          newoff[rec] = nd.size();
          nd.insert(nd.end(), od.begin() + static_cast<long>(rec), od.begin() + static_cast<long>(end));
          if (is_fde) {  // recompute CIE pointer: field at rec+4 holds (field_old - cie_old); cie_old = rec+4-cie_id
            uint64_t cie_old = rec + 4 - cie_id; auto it = newoff.find(cie_old); if (it == newoff.end()) fail(".eh_frame FDE refers to a missing CIE");
            uint32_t nid = static_cast<uint32_t>((newoff[rec] + 4) - it->second); std::memcpy(nd.data() + newoff[rec] + 4, &nid, 4);
          }
          for (const Rela& r : rels) if (r.offset >= rec && r.offset < end) { Rela x = r; x.offset = newoff[rec] + (r.offset - rec); nr.push_back(x); }
          pos = end;
        }
        sec.data = std::move(nd); sec.size = sec.data.size();
        if (rit != ob.relocs.end()) rit->second = std::move(nr);
      }
    }
  }

  // Layout preservation: a folded output section (e.g. .text) is a *run* of consecutive foldable inputs. When a
  // non-foldable input that the final linker would place in the same output section interrupts the run (a COMDAT
  // group member such as .text._Z..., or a SHF_LINK_ORDER text section), the run is closed and later foldable inputs
  // start a new output section with the same name. The final link then sees inputs in exactly the original order,
  // so the linked image is byte-identical to a direct link; proximity resolution acts within a run.
  std::map<std::string, int> run_of;
  std::string merge_key(int o, int s, std::string& outname) {
    const Section& sec = obj(o).sections[static_cast<size_t>(s)]; const InSec& x = in[static_cast<size_t>(o)][static_cast<size_t>(s)];
    outname = sec.name;
    if (x.group >= 0 || (sec.flags & SHF_LINK_ORDER)) {
      Section tmp = sec; tmp.flags &= ~static_cast<uint64_t>(SHF_GROUP | SHF_LINK_ORDER);
      std::string final_name = output_section_name(tmp, opt.keep_function_sections, opt.fold_data);
      if (final_name != sec.name && !opt.merge_across_groups) run_of[final_name]++;      // interrupts a foldable run
      if (x.group >= 0) return "G" + std::to_string(x.group) + "#" + std::to_string(s) + "#" + sec.name;
      return "L" + std::to_string(o) + "#" + std::to_string(s);
    }
    if (sec.flags & SHF_MERGE) return "M|" + sec.name + "|" + std::to_string(sec.flags) + "|" + std::to_string(sec.entsize) + "|" + std::to_string(sec.type);
    outname = output_section_name(sec, opt.keep_function_sections, opt.fold_data);
    return "S|" + outname + "|" + std::to_string(sec.flags) + "|" + std::to_string(sec.type) + "|" + std::to_string(sec.entsize) + "|run" + std::to_string(run_of[outname]);
  }

  void plan_sections() {
    out.sections.push_back(Section{});
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o)
      for (int s = 1; s < static_cast<int>(obj(o).sections.size()); ++s) {
        InSec& x = is(o, s); if (!x.kept()) continue;
        const Section& sec = obj(o).sections[static_cast<size_t>(s)];
        std::string name; std::string key = merge_key(o, s, name);
        auto it = outkey.find(key); int oi;
        if (it == outkey.end()) {
          Section ns; ns.name = name; ns.type = sec.type; ns.flags = sec.flags; ns.entsize = sec.entsize; ns.addralign = sec.addralign; ns.info = (sec.flags & SHF_INFO_LINK) ? sec.info : 0; ns.size = 0;
          oi = static_cast<int>(out.sections.size()); out.sections.push_back(std::move(ns)); outkey[key] = oi;
        } else oi = it->second;
        Section& os = out.sections[static_cast<size_t>(oi)];
        uint64_t a = std::max<uint64_t>(1, sec.addralign); os.addralign = std::max(os.addralign, a);
        uint64_t cur = (os.type == SHT_NOBITS) ? os.size : os.data.size();
        if (sec.name == ".eh_frame") {
          // records are self-delimiting and 4-byte-length-prefixed; zero padding between inputs would read as the
          // terminator and make every later record (and relocation) invisible to the final linker
          if (cur % 4 || sec.data.size() % 4) fail(".eh_frame input " + lab(o, s) + " is not a multiple of 4 bytes");
          a = 4;
        }
        uint64_t aligned = (cur + a - 1) / a * a;
        if (os.type != SHT_NOBITS) { os.data.resize(aligned, (os.flags & SHF_EXECINSTR) ? opt.text_fill : 0); os.data.insert(os.data.end(), sec.data.begin(), sec.data.end()); os.size = os.data.size(); }
        else os.size = aligned + sec.size;
        x.out = oi; x.off = aligned;
        if (!os.origin.empty()) os.origin += ","; os.origin += std::to_string(o) + ":" + std::to_string(s);
      }
    // link-order links
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o)
      for (int s = 1; s < static_cast<int>(obj(o).sections.size()); ++s) {
        const InSec& x = is(o, s); if (!x.kept()) continue;
        const Section& sec = obj(o).sections[static_cast<size_t>(s)];
        if (sec.link >= 0) { const InSec& l = is(o, sec.link); if (!l.kept()) fail("SHF_LINK_ORDER section " + lab(o, s) + " links to a dropped section"); out.sections[static_cast<size_t>(x.out)].link = l.out; }
      }
    st.sections_out = out.sections.size() - 1;
    for (size_t i = 1; i < out.sections.size(); ++i) if (out.sections[i].flags & SHF_ALLOC) st.alloc_bytes_out += (out.sections[i].type == SHT_NOBITS ? 0 : out.sections[i].size);
    secsym.assign(out.sections.size(), 0);
  }

  uint32_t add_sym(const Symbol& s) { out.symbols.push_back(s); return static_cast<uint32_t>(out.symbols.size() - 1); }
  uint32_t section_symbol(int oi) {
    if (secsym[static_cast<size_t>(oi)] == 0) { Symbol s; s.name = out.sections[static_cast<size_t>(oi)].name; s.bind = STB_LOCAL; s.type = STT_SECTION; s.shndx = oi; secsym[static_cast<size_t>(oi)] = static_cast<int>(add_sym(s)); }
    return static_cast<uint32_t>(secsym[static_cast<size_t>(oi)]);
  }

  void build_symbols() {
    out.symbols.push_back(Symbol{});
    symmap.resize(d.objs.size());
    std::map<std::string, uint32_t> global_out;       // name -> out index (defined or undefined)
    std::map<std::string, std::vector<std::pair<int, uint32_t>>> undefs;  // name -> references
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o) {
      Object& ob = obj(o); symmap[static_cast<size_t>(o)].assign(ob.symbols.size(), 0);
      for (uint32_t si = 1; si < ob.symbols.size(); ++si) {
        const Symbol& s = ob.symbols[si];
        if (s.type == STT_FILE) { symmap[static_cast<size_t>(o)][si] = add_sym(s); continue; }
        if (s.type == STT_SECTION) { if (s.shndx >= 0 && is(o, s.shndx).kept()) symmap[static_cast<size_t>(o)][si] = section_symbol(is(o, s.shndx).out); continue; }
        if (s.bind == STB_LOCAL) {
          if (s.shndx >= 0) { const InSec& x = is(o, s.shndx); if (!x.kept()) continue; Symbol n = s; n.shndx = x.out; n.value = s.value + x.off; symmap[static_cast<size_t>(o)][si] = add_sym(n); }
          else if (s.special == SHN_ABS) symmap[static_cast<size_t>(o)][si] = add_sym(s);
          continue;
        }
        // global / weak
        if (s.undefined()) { undefs[s.name].push_back({o, si}); continue; }
        auto it = globals.defs.find(s.name);
        bool chosen = it != globals.defs.end() && it->second.obj == o && it->second.sym == si;
        if (!chosen) continue;   // dropped duplicate weak/common; references are redirected through the name below
        Symbol n = s;
        if (s.shndx >= 0) { const InSec& x = is(o, s.shndx); if (x.discarded) fail("chosen definition of '" + s.name + "' lies in a discarded COMDAT group (resolution order bug)"); if (!x.kept()) continue; n.shndx = x.out; n.value = s.value + x.off; }
        global_out[s.name] = add_sym(n);
      }
    }
    // undefined names (not defined anywhere in the domain) and redirects
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o) {
      Object& ob = obj(o);
      for (uint32_t si = 1; si < ob.symbols.size(); ++si) {
        const Symbol& s = ob.symbols[si];
        if (s.bind == STB_LOCAL || s.type == STT_FILE || s.type == STT_SECTION || s.name.empty()) continue;
        auto g = global_out.find(s.name);
        if (g != global_out.end()) { symmap[static_cast<size_t>(o)][si] = g->second; continue; }
        if (!s.undefined()) continue;     // defined in a dropped section: leave unmapped
        Symbol n; n.name = s.name; n.bind = STB_GLOBAL; n.type = s.type; n.other = 0; n.shndx = -1; n.special = SHN_UNDEF;
        bool all_weak = true; uint8_t vis = 0;
        for (auto& [oo, ss] : undefs[s.name]) { const Symbol& u = obj(oo).symbols[ss]; if (u.bind != STB_WEAK) all_weak = false; vis = std::max<uint8_t>(vis, u.visibility()); }
        if (all_weak) n.bind = STB_WEAK; n.other = vis;
        uint32_t idx = add_sym(n); global_out[s.name] = idx; symmap[static_cast<size_t>(o)][si] = idx;
      }
    }
    refcount.assign(out.symbols.size() + 4096, 0);
  }

  static void write_le(std::vector<uint8_t>& data, uint64_t off, int64_t v, int width) {
    for (int i = 0; i < width; ++i) data[off + static_cast<uint64_t>(i)] = static_cast<uint8_t>((static_cast<uint64_t>(v) >> (8 * i)) & 0xff);
  }

  void relocate() {
    for (int o = 0; o < static_cast<int>(d.objs.size()); ++o) {
      Object& ob = obj(o);
      for (auto& [site, vec] : ob.relocs) {
        const InSec& sx = is(o, site); if (!sx.kept()) continue;
        Section& osec = out.sections[static_cast<size_t>(sx.out)];
        bool site_alloc = osec.flags & SHF_ALLOC;
        std::vector<Rela>& outvec = out.relocs[sx.out];
        for (const Rela& r : vec) {
          RelocClass cls = classify_reloc(r.type);
          if (cls == RelocClass::None) continue;
          const Symbol& insym = ob.symbols[r.sym];
          const Symbol* defsym = nullptr; auto [to, ts] = def_section(o, r.sym, &defsym);
          bool target_defined_here = to >= 0;
          bool target_kept = target_defined_here && is(to, ts).kept();
          bool target_dropped = target_defined_here && !target_kept;
          if (target_dropped) {
            if (site_alloc) fail("live section " + lab(o, site) + " references dropped section " + lab(to, ts) + " via " + reloc_name(r.type) + " to '" + insym.name + "' (liveness/COMDAT inconsistency)");
            outvec.push_back(Rela{sx.off + r.offset, 0, R_X86_64_NONE, 0}); st.residual.add(RelocClass::None); continue;
          }
          // proximity resolution
          if (opt.apply_pcrel && cls == RelocClass::PcRel && target_kept && is(to, ts).out == sx.out) {
            bool weak = defsym->bind == STB_WEAK, ifunc = defsym->type == STT_GNU_IFUNC;
            bool preemptible = !opt.static_link && defsym->bind == STB_GLOBAL && defsym->visibility() == STV_DEFAULT;
            if (!weak && !ifunc && !preemptible && osec.type != SHT_NOBITS) {
              int64_t S = static_cast<int64_t>(is(to, ts).off + defsym->value), P = static_cast<int64_t>(sx.off + r.offset);
              int64_t v = S + r.addend - P;
              int width = (r.type == R_X86_64_PC64) ? 8 : (r.type == R_X86_64_PC16) ? 2 : (r.type == R_X86_64_PC8) ? 1 : 4;
              if (width == 4 && (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())) fail("PC32 overflow applying relocation in " + lab(o, site));
              if (width == 2 && (v < -32768 || v > 32767)) fail("PC16 overflow"); if (width == 1 && (v < -128 || v > 127)) fail("PC8 overflow");
              if (sx.off + r.offset + static_cast<uint64_t>(width) > osec.data.size()) fail("relocation offset beyond section in " + lab(o, site));
              write_le(osec.data, sx.off + r.offset, v, width);
              st.applied_pcrel++; continue;
            }
          }
          // keep, rewritten
          Rela n; n.offset = sx.off + r.offset; n.type = r.type; n.addend = r.addend;
          if (insym.type == STT_SECTION) {
            if (insym.shndx < 0 || !is(o, insym.shndx).kept()) { if (site_alloc) fail("relocation against dropped section symbol in " + lab(o, site)); outvec.push_back(Rela{n.offset, 0, R_X86_64_NONE, 0}); st.residual.add(RelocClass::None); continue; }
            n.sym = section_symbol(is(o, insym.shndx).out); n.addend += static_cast<int64_t>(is(o, insym.shndx).off);
          } else {
            uint32_t m = symmap[static_cast<size_t>(o)][r.sym];
            if (m == 0) { if (site_alloc) fail("relocation in " + lab(o, site) + " references dropped symbol '" + insym.name + "'"); outvec.push_back(Rela{n.offset, 0, R_X86_64_NONE, 0}); st.residual.add(RelocClass::None); continue; }
            n.sym = m;
          }
          if (n.sym >= refcount.size()) refcount.resize(n.sym + 1, 0); refcount[n.sym]++;
          outvec.push_back(n); st.residual.add(cls);
          if (cls == RelocClass::PcRel) { if (!target_defined_here && !(defsym && defsym->special == SHN_ABS)) st.residual_cut_pcrel++; else st.residual_internal_pcrel++; }
        }
      }
    }
  }

  void localise_and_strip() {
    if (opt.have_exports)
      for (size_t i = 1; i < out.symbols.size(); ++i) {
        Symbol& s = out.symbols[i];
        if (s.bind == STB_LOCAL || !s.defined() || s.name.empty()) continue;
        if (opt.exports.count(s.name) || opt.entry_symbols.count(s.name)) continue;
        if (s.bind == STB_WEAK) continue;                                                    // a strong definition elsewhere may still override it
        if (s.shndx >= 0 && (out.sections[static_cast<size_t>(s.shndx)].flags & SHF_GROUP)) continue;   // COMDAT members must stay resolvable by name
        s.bind = STB_LOCAL; st.localised++;
      }
    if (!opt.strip_locals) return;
    refcount.resize(out.symbols.size(), 0);
    std::vector<uint32_t> remap(out.symbols.size(), 0); std::vector<Symbol> kept; kept.push_back(out.symbols[0]);
    std::set<uint32_t> sigs; for (auto& g : out.groups) sigs.insert(g.signature);
    for (uint32_t i = 1; i < out.symbols.size(); ++i) {
      const Symbol& s = out.symbols[i];
      bool strip = s.bind == STB_LOCAL && s.type != STT_SECTION && s.type != STT_FILE && refcount[i] == 0 && !sigs.count(i);
      if (strip) { st.stripped++; continue; }
      remap[i] = static_cast<uint32_t>(kept.size()); kept.push_back(s);
    }
    for (auto& [t, v] : out.relocs) for (Rela& r : v) r.sym = remap[r.sym];
    for (auto& g : out.groups) g.signature = remap[g.signature];
    for (size_t i = 0; i < secsym.size(); ++i) if (secsym[i]) secsym[i] = static_cast<int>(remap[static_cast<uint32_t>(secsym[i])]);
    out.symbols = std::move(kept);
  }

  void groups() {
    for (const KeptGroup& kg : kept_groups) {
      const Group& g = obj(kg.obj).groups[kg.gidx]; Group n; n.flags = g.flags; n.name = g.name;
      bool any = false; for (int m : g.members) if (is(kg.obj, m).kept()) { n.members.push_back(is(kg.obj, m).out); any = true; }
      if (!any) continue;
      n.signature = symmap[static_cast<size_t>(kg.obj)][g.signature];
      if (n.signature == 0) { // signature symbol may have been a local or dropped; re-add as a local symbol
        Symbol s = obj(kg.obj).symbols[g.signature]; s.shndx = -1; s.special = SHN_UNDEF; s.bind = STB_LOCAL; s.type = STT_NOTYPE; s.value = 0; n.signature = add_sym(s);
      }
      out.groups.push_back(std::move(n));
    }
  }

  nlohmann::json manifest() {
    nlohmann::json exports = nlohmann::json::array(), imports = nlohmann::json::array(), secs = nlohmann::json::array(), inputs = nlohmann::json::array();
    for (size_t i = 1; i < out.symbols.size(); ++i) {
      const Symbol& s = out.symbols[i];
      if (s.type == STT_SECTION || s.type == STT_FILE || s.name.empty()) continue;
      if (s.bind != STB_LOCAL && s.defined()) exports.push_back({{"sym", s.name}, {"sec", section_name_of(out, s)}, {"bind", s.bind == STB_WEAK ? "weak" : "global"}, {"vis", s.visibility()}, {"type", s.type}, {"size", s.size}});
      else if (s.undefined()) imports.push_back(s.name);
    }
    for (size_t i = 1; i < out.sections.size(); ++i) { const Section& s = out.sections[i]; secs.push_back({{"name", s.name}, {"size", s.size}, {"flags", s.flags}, {"inputs", s.origin}}); }
    for (auto& o : d.objs) inputs.push_back(o.path);
    st.symbols_out = out.symbols.size() - 1;
    return {{"origin", opt.origin_tag}, {"inputs", inputs}, {"exports", exports}, {"imports", imports}, {"stats", st.json()}, {"sections", secs},
            {"alloc_bytes", st.alloc_bytes_out}, {"residual_relocs", st.residual.total}, {"applied_pcrel", st.applied_pcrel}};
  }

  PrelinkResult run() {
    init(); comdat(); mark_dead(); prune_eh_frame(); plan_sections(); build_symbols(); relocate(); groups(); localise_and_strip();
    out.path = "<prelinked>"; out.machine = d.objs[0].machine; out.osabi = d.objs[0].osabi;
    PrelinkResult r; r.manifest = manifest(); r.stats = st; r.out = std::move(out); return r;
  }
};

}  // namespace

PrelinkResult prelink(const ObjectSet& domain, const std::set<SecRef>* live, const PrelinkOptions& opt) {
  Builder b(domain, live, opt);
  return b.run();
}

}  // namespace opticc::elf
