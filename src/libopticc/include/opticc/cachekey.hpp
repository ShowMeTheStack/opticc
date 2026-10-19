// opticc: distributed compile cache keys.
//
// Unlike ccache, the key never needs the preprocessed output (which costs p_j on the client, the usual bottleneck).
// It is computed from what the header-push path already has:
//   compiler id   sha256(compiler binary bytes) + first line of `--version` (cached per path/mtime/size)
//   argv          normalised: -o/-MF/-MT/-MQ/-MD/-MMD/-Wp,-M*/-H and their values dropped; every path kept as
//                 spelled (__FILE__ expands to the spelling), the
//                 source as "$SRC=<spelling>"; cwd appended when debug info is on (DW_AT_comp_dir); order kept
//   source        sha256 of the source bytes
//   closure       sorted (relative path, sha256) pairs of the headers the compile read
//
// Two-level scheme (as ccache's "manifest"/depend mode, but on headers the worker already holds):
//   manifest key  M = sha256(compiler id, argv, source sha)            — known BEFORE compiling
//   object  key   O = sha256(M, used closure)                           — known after the first compile (depfile)
// The worker stores under M a list of {used closure, O}; a lookup matches an entry when every (path, sha) of its
// used closure equals the client's current view (the shipped closure, a superset) AND the view's path set ("shape",
// opticcd/objcache.hpp) equals the stored one.  Correctness: an entry matches only if every non-system header the
// compile read is byte-identical, the source is identical, the compiler is the same binary (which pins its own system
// headers only as far as the image does: system headers are not hashed — all pool nodes run one image), and the flags
// are the same; a header added earlier in the include search path (shadowing a used one under another path) changes
// the shape, so it misses. Edits to shipped-but-unused headers keep hitting (the scanner's superset is not a penalty).
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opticc::cachekey {

using Closure = std::vector<std::pair<std::string, std::string>>;   // (path, sha256)

// Path relative to cwd when under it (so keys are location-independent across clients with the same layout).
std::string relativise(const std::string& path, const std::string& cwd);

// Canonical argv (see header comment). `source_index` is the position of the source operand (from args::scan).
std::vector<std::string> normalise_argv(const std::vector<std::string>& argv, int source_index, const std::string& cwd);

// Compiler identity for a compiler name/path as the worker will execute it (PATH lookup, symlinks resolved);
// cached per (resolved path, mtime, size). Falls back to the name + version text if the binary cannot be read.
// resolved path of a compiler name (PATH lookup, symlinks followed)
std::string resolve_compiler(const std::string& compiler);
std::string compiler_id(const std::string& compiler);
// The identity part for cc1/cc1plus/as and the driver's shared libraries (contents); exposed for tests
std::string toolchain_parts(const std::string& resolved_driver, bool* ok = nullptr);   // *ok: every helper query answered
// True if `content` has an assembler file directive (.incbin/.include) that reads a file
// outside every key and dependency file — assembler input: any; C/C++ (header or preprocessed text): inline asm
bool file_directive(const std::string& content, bool assembler_input);

// Manifest key M.
std::string manifest_key(const std::string& compiler_id, const std::vector<std::string>& norm_argv, const std::string& src_sha);
// The FULL content key of a header-push compile: the manifest key plus every (relative path, sha) of the view the
// worker matches against (source, shipped closure, "@sysinc"). Client and worker compute it identically; a worker
// advertises the full keys of its entries, so an affinity probe hits only when the whole closure content is equal.
std::string full_key(const std::string& mkey, const std::vector<std::pair<std::string, std::string>>& view);
// Object key O (closure paths are relativised against cwd and sorted inside).
std::string object_key(const std::string& manifest_key, Closure closure, const std::string& cwd);
// Classic mode: the preprocessed text replaces source + closure.
std::string classic_key(const std::string& compiler_id, const std::vector<std::string>& norm_argv, const std::string& preprocessed_sha);

// Comment-insensitive content hash for C/C++ sources and headers, safe for exact object reproduction.
// Every comment byte becomes a space (newlines stay newlines), so each token keeps its exact line AND column (debug
// info, __LINE__, diagnostics unchanged); whitespace-only lines at end of file are dropped (an appended comment is
// invisible). Falls back to the raw SHA-256 (returned with *normalised = false) on anything it does not model exactly:
// raw string literals, trigraphs, a backslash line splice touching a comment, an unterminated comment or literal.
// Prefix "n:" distinguishes normalised hashes from raw ones in keys.
std::string content_key_sha(const std::string& content, bool* normalised = nullptr);
// argv flags under which the normalised hash must not be used: comments survive preprocessing (-C, -CC), or debug
// info is emitted (any -g except -g0; DWARF 5 embeds source-file MD5s).
bool keeps_comments(const std::vector<std::string>& argv);
// The text uses __DATE__, __TIME__ or __TIMESTAMP__ outside comments and literals. A TU reading such a
// file is never stored under a content key: its object depends on the clock or on a file time, not on the bytes.
bool uses_time_macros(const std::string& text);
// Some -D value of argv uses a time macro (-DSTAMP=__TIME__): such a compile is never content-keyed
bool argv_uses_time_macros(const std::vector<std::string>& argv);
// The compile's dependency file lists system headers too (-MD / -M, also via -Wp,; -MMD / -MM do not)
bool dep_lists_system_headers(const std::vector<std::string>& argv);
// Any debug info (-g, -gline-tables-only, -gdwarf-N …; -g0 last turns it off): the object records cwd
bool debug_info(const std::vector<std::string>& argv);

}  // namespace opticc::cachekey
