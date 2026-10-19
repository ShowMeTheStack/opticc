// SPDX-License-Identifier: GPL-2.0-only
// Domain prelink with proximity resolution.
//
// Pipeline for one domain: COMDAT dedup → drop dead sections (live set) → merge input sections into output
// sections by the rule table below → rebuild the symbol table → for every relocation: apply-and-drop if it is
// PC-relative to a defined, non-preemptible, non-weak symbol in the SAME output section (its value S+A-P is
// invariant under final placement because the final linker treats an input section atomically), otherwise keep
// it with offsets/symbols rewritten for the merged layout → localise symbols that other domains do not import.
//
// Merge rule table (non-group, non-SHF_MERGE, non-SHF_LINK_ORDER sections; names containing ".." are kernel
// special sections and are never renamed):
//   .text, .text.<fn>                      -> .text            (.text.unlikely[.*] -> .text.unlikely, .text.hot[.*] -> .text.hot,
//                                                               .text.startup[.*] -> .text.startup, .text.exit[.*] -> .text.exit,
//                                                               .text.split[.*] -> .text.split; .text.sorted.* kept)
//   everything else: exact name (same-name inputs are concatenated, as ld -r does). Data/rodata/bss are NOT folded by
//     default: proximity resolution only acts on text-to-text references, and folding .rodata.<x> into .rodata would
//     change the interleaving of mergeable string blobs and non-mergeable pieces inside the final .rodata (so the final
//     layout would differ from a direct link). --fold-data enables folding of .rodata.*/.data[.rel[.ro[.local]]].*/.bss.*/
//     .tbss.*/.tdata.* for users who want fewer sections and accept a different (equally valid) layout.
//   dropped always: .llvm_addrsig, .llvm.call-graph-profile (symbol-index tables that are meaningless after a symtab rewrite)
// --keep-function-sections disables the renaming rows (concatenation by exact name only).
// Layout preservation: a folded run is closed whenever a COMDAT-group or SHF_LINK_ORDER section that the final linker
// would place in the same output section interrupts it; the next foldable input opens a new section of the same name.
// With this, the final link of the prelinked domains is byte-identical to a direct link of the original objects
// (verified by tests/elf/test_prelink.cpp). --merge-across-groups disables the splitting (more applications, different layout).
// Padding between concatenated inputs: 0xCC in SHF_EXECINSTR sections (ld.lld's x86 trap fill), 0 elsewhere.
//
// Relocation handling table:
//   R_X86_64_NONE                                   dropped
//   PC32/PLT32/PC64/PC16/PC8 to a defined symbol in the same output section, binding LOCAL, or GLOBAL with
//     non-default visibility, or GLOBAL default when --static (final link is static/executable, so nothing
//     is preemptible); never WEAK (another domain may hold the strong definition), never IFUNC:
//                                                   APPLIED (S+A-P written, range-checked) and dropped
//     PLT32 is treated as PC32 here: in a static/relocatable link a PLT32 against a defined non-preemptible
//     symbol is resolved by every linker as a direct PC-relative reference (no PLT exists).
//   all other relocations (absolute, GOT*, TLS, cross-section PC-relative, undefined targets, weak targets):
//                                                   kept; r_offset += merge offset of the site; symbol index
//                                                   remapped; section-symbol targets become the output section
//                                                   symbol with r_addend += merge offset of the target
//   relocation in a kept ALLOC section to a dropped section: hard error (liveness bug); in a non-alloc
//     section: tombstoned to R_X86_64_NONE/sym 0 (what final linkers do for debug references)
//   .eh_frame: FDEs whose function section was dropped are removed and CIE pointers recomputed.
#pragma once
#include <set>
#include <string>

#include "opticc/elf/liveness.hpp"
#include "opticc/elf/manifest.hpp"

namespace opticc::elf {

struct PrelinkOptions {
  bool gc = true;                        // drop sections not in the live set (needs `live`)
  bool keep_function_sections = false;
  bool fold_data = false;                // also fold .rodata.*/.data.*/.bss.* (see rule table)
  bool merge_across_groups = false;      // keep one .text run across COMDAT/link-order interruptions (changes final layout; still correct)
  bool apply_pcrel = true;               // proximity resolution (off = plain ld -r-like merge, for ablation)
  bool static_link = true;               // default-visibility globals are non-preemptible
  bool strip_locals = false;             // remove unreferenced local symbols after localisation
  bool have_exports = false;             // if true, defined globals not in `exports` and not entry symbols are localised
  std::set<std::string> exports;
  std::set<std::string> entry_symbols;   // never localised
  uint8_t text_fill = 0xCC;
  std::string origin_tag;                // recorded in the manifest
};

struct PrelinkStats {
  ClassCounts input, residual;
  uint64_t applied_pcrel = 0;
  uint64_t residual_cut_pcrel = 0;       // PC-relative kept because the target is outside the domain (undefined here)
  uint64_t residual_internal_pcrel = 0;  // PC-relative kept for another reason (cross-output-section, weak, preemptible, tombstone)
  uint64_t sections_in = 0, sections_dropped_dead = 0, sections_dropped_comdat = 0, sections_out = 0;
  uint64_t symbols_in = 0, symbols_out = 0, localised = 0, stripped = 0, fdes_dropped = 0;
  uint64_t alloc_bytes_in = 0, alloc_bytes_out = 0;
  nlohmann::json json() const;
};

struct PrelinkResult {
  Object out;
  PrelinkStats stats;
  nlohmann::json manifest;               // exports/imports/residual census/sections/origin
};

// `live`: set of live SecRefs over `domain` (local indices), required when opt.gc.
PrelinkResult prelink(const ObjectSet& domain, const std::set<SecRef>* live, const PrelinkOptions& opt);

// Exposed for tests.
std::string output_section_name(const Section& s, bool keep_function_sections, bool fold_data = false);

}  // namespace opticc::elf
