// SPDX-License-Identifier: GPL-2.0-only
// Manifests and the section reference graph.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "opticc/elf/elf.hpp"

namespace opticc::elf {

struct SecRef {
  int obj = -1, sec = -1;
  bool operator<(const SecRef& o) const { return obj != o.obj ? obj < o.obj : sec < o.sec; }
  bool operator==(const SecRef& o) const { return obj == o.obj && sec == o.sec; }
  bool valid() const { return obj >= 0 && sec > 0; }
};

enum class RelocClass { None, PcRel, Abs, Got, Tls, Other };
RelocClass classify_reloc(uint32_t type);
const char* reloc_class_name(RelocClass c);

struct ClassCounts {
  uint64_t total = 0, pcrel = 0, abs = 0, got = 0, tls = 0, other = 0, none = 0;
  void add(RelocClass c, uint64_t n = 1);
  nlohmann::json json() const;
};

struct ObjectSet {
  std::vector<Object> objs;
  static ObjectSet load(const std::vector<std::string>& paths);
  const Section& sec(SecRef r) const { return objs[static_cast<size_t>(r.obj)].sections[static_cast<size_t>(r.sec)]; }
  std::string label(SecRef r) const;   // "path:(.text.foo)"
};

// Global symbol resolution over a set: name -> defining (obj, symbol index), strong preferred over weak,
// first definition wins among equals (duplicate strong definitions are reported, not resolved).
struct GlobalTable {
  struct Def { int obj; uint32_t sym; };
  std::map<std::string, Def> defs;
  std::map<std::string, std::vector<Def>> duplicates;  // strong multiple definitions
  std::set<std::string> undefined;                       // referenced but not defined anywhere in the set
};
GlobalTable resolve_globals(const ObjectSet& set);

// Section-level reference graph: arc (site section -> target section) per relocation, classified.
struct SectionGraph {
  std::map<std::pair<SecRef, SecRef>, ClassCounts> arcs;
  std::map<SecRef, std::map<std::string, ClassCounts>> imports;   // site section -> undefined symbol -> counts
  std::map<SecRef, std::set<std::string>> start_stop_refs;        // site section -> section names referenced via __start_/__stop_
  GlobalTable globals;
  // Target section of a symbol reference from object `obj` (resolving undefined names through `globals`); invalid if none.
  SecRef target_of(const ObjectSet& set, int obj, uint32_t symidx) const;
};
SectionGraph build_section_graph(const ObjectSet& set);

// Per-object or per-set manifest: exports, imports, relocation census, sections, bytes.
nlohmann::json manifest_json(const ObjectSet& set, const SectionGraph& g);

bool is_c_identifier(const std::string& s);

// .eh_frame records (32-bit length encoding only). For each record: byte range, CIE/FDE, the relocation at
// pc_begin (FDE, record offset + 8) and the other relocations inside the record (personality for CIEs, LSDA for FDEs).
struct EhRecord { uint64_t off = 0, end = 0; bool fde = false; const Rela* pc_begin = nullptr; std::vector<const Rela*> others; };
std::vector<EhRecord> parse_eh_frame(const Section& sec, const std::vector<Rela>& relocs);

}  // namespace opticc::elf
