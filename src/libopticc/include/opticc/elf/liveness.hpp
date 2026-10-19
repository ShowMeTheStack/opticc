// SPDX-License-Identifier: GPL-2.0-only
// Section liveness. Two computations that must agree:
//   compute_liveness(): reference, full section graph over every object (what the final linker does);
//   condense_domain() + resolve_condensed() + expand_domain(): the manifest-driven distributed form.
// Semantics mirror ld.lld --gc-sections: roots are the entry symbol's section, reserved section names
// (.init*/.fini*/.ctors/.dtors/.jcr/.init_array*/.fini_array*/.preinit_array*), SHF_GNU_RETAIN sections and
// user-supplied KEEP globs; non-SHF_ALLOC sections are kept but their relocations are not followed; a
// SHF_LINK_ORDER section is live iff its linked section is; __start_X/__stop_X references from a live
// section make every section named X live; relocations against section symbols make that section live.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "opticc/elf/manifest.hpp"

namespace opticc::elf {

struct Roots {
  std::vector<std::string> entry_symbols;       // e.g. {"_start"}
  std::vector<std::string> keep_globs;          // section-name globs always kept (KEEP in linker scripts), '*' wildcard
  bool reserved_names = true;                   // lld's isReserved() names
  bool group_members_together = false;          // mark every member of a section group live when one is (set per linker behaviour)
};

bool glob_match(const std::string& pat, const std::string& s);
bool is_root_section(const Section& s, const Roots& r);

struct Liveness {
  std::set<SecRef> live;                        // live SHF_ALLOC sections
  std::vector<SecRef> removed;                  // SHF_ALLOC sections that are not live (what lld --print-gc-sections lists)
};

Liveness compute_liveness(const ObjectSet& set, const SectionGraph& g, const Roots& roots);

// --- condensed form ---
struct DomainManifest {
  int domain = 0;
  std::vector<int> objs;                                  // indices into the global ObjectSet (bookkeeping for tests)
  std::set<SecRef> roots;                                 // local SecRefs (obj index local to the domain's ObjectSet)
  std::set<SecRef> entries;                               // sections defining global/weak symbols, or C-identifier-named alloc sections
  std::map<SecRef, std::set<SecRef>> arcs;                // entry/root -> reachable exits (local reachability)
  std::map<SecRef, std::set<std::string>> imports;        // exit -> symbols not defined in this domain
  std::map<SecRef, std::set<std::string>> start_stop;     // exit -> section names referenced via __start_/__stop_
  std::map<std::string, SecRef> exports;                  // symbol -> defining section
  std::map<std::string, std::vector<SecRef>> named_sections;
  size_t arc_count() const { size_t n = 0; for (auto& [e, xs] : arcs) n += xs.size(); return n; }
  nlohmann::json json(const ObjectSet& local) const;
};

DomainManifest condense_domain(const ObjectSet& local, const Roots& roots, int domain_id);
// Global fixpoint over condensed manifests: returns, per domain, the live entry/root sections.
std::map<int, std::set<SecRef>> resolve_condensed(const std::vector<DomainManifest>& ms);
// Local expansion inside one domain from its live entries/roots.
Liveness expand_domain(const ObjectSet& local, const Roots& roots, const std::set<SecRef>& live_entries);

}  // namespace opticc::elf
