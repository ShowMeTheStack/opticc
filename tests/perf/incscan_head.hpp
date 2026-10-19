// Baseline copy of the cheap include scanner for header-push, kept for the closure microbenchmark.
// Like distcc's include server: a regex-free line scan of `#include "x"` / `<x>` directives, resolved
// transitively through the job's -iquote/-I/-isystem/-idirafter directories and the including file's
// directory, with a per-process cache keyed by (path, mtime, size). Conditional compilation is ignored,
// so the closure is a superset of what the compiler reads (shipping too much costs bytes, never
// correctness; a genuine miss is still caught by the worker's MISSING round). Files inside the
// compiler's system directories are neither shipped nor entered. A directive whose operand is not a
// literal string/angle name (`#include MACRO`) marks the closure *computed*: the caller falls back to
// one `-MM` run for that translation unit only.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "opticc/argscan.hpp"

namespace opticc::incscan_head {

struct Directive { bool angled = false; bool next = false; std::string name; };
struct FileScan { std::vector<Directive> incs; bool computed = false; int64_t mtime_ns = 0; uint64_t size = 0; };

// Parse the directives of one source text (exposed for tests).
FileScan parse_text(const std::string& text);

class Scanner {
 public:
  // Transitive non-system closure of `src_abs` (absolute paths, excluding src itself, in discovery order).
  // `forced` are -include/-imacros files (resolved like quoted includes from cwd). *computed is set when any
  // scanned file has a non-literal include; *files_scanned counts the files read (cache misses + hits).
  std::vector<std::string> closure(const std::string& src_abs, const args::IncludeDirs& dirs,
                                   const std::vector<std::string>& system_dirs, const std::string& cwd,
                                   bool* computed, int* files_scanned = nullptr);
  size_t cache_size() const { std::lock_guard<std::mutex> g(mu_); return cache_.size(); }
 private:
  const FileScan& scan_file(const std::string& abs);
  mutable std::mutex mu_;
  std::unordered_map<std::string, FileScan> cache_;
};

}  // namespace opticc::incscan_head
