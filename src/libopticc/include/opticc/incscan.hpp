// opticc: cheap include scanner for header-push.
// Like distcc's include server: a regex-free line scan of `#include "x"` / `<x>` directives, resolved
// transitively through the job's -iquote/-I/-isystem/-idirafter directories and the including file's
// directory. Conditional compilation is ignored, so the closure is a superset of what the compiler reads
// (shipping too much costs bytes, never correctness; a genuine miss is caught by the worker's MISSING
// round). Files inside the compiler's system directories are neither shipped nor entered. A directive
// whose operand is not a literal string/angle name (`#include MACRO`) marks the closure *computed*.
//
// Performance design (a naive scan costs about 0.3 CPU-s per kernel TU):
//   * every file is parsed once per coordinator lifetime (re-validated by stat at most every kValidateTtl s);
//   * include resolution is memoised per (search configuration, includer directory, directive) — the kbuild
//     graph has ~700 files × ~8 directives × 9 search dirs per TU, i.e. ~50,000 stat() calls per TU without it;
//   * the resolved adjacency of each file is memoised per search configuration, so a TU closure is a DFS over
//     small integer ids with no syscalls once the graph is warm.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <mutex>
#include <atomic>
#include <deque>
#include <shared_mutex>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

#include "opticc/argscan.hpp"
#include "opticc/direct.hpp"

namespace opticc::incscan {

struct Directive { bool angled = false; bool next = false; std::string name; };
// has_include_computed: an #if/#elif uses __has_include(MACRO) -- a file the worker cannot know to
// need, and whose absence silently flips the condition there (no "file not found" to trigger a MISSING round).
// Literal __has_include("x") / (<x>) names are added to incs (shipped when they resolve on the client).
struct FileScan { std::vector<Directive> incs; bool computed = false; bool has_include_computed = false; int64_t mtime_ns = 0; uint64_t size = 0;
                  int64_t ctime_ns = 0; uint64_t ino = 0;   // parse validated on (mtime, size, inode, ctime)
                  double parsed_wall = 0; };   // wall-clock time of the parse (racy-file guard for persistence)

// Parse the directives of one source text (exposed for tests).
FileScan parse_text(const std::string& text);

class Scanner {
 public:
  static constexpr double kValidateTtl = 5.0;   // seconds between re-stat()s of a cached file
  // Transitive non-system closure of `src_abs` (absolute paths, excluding src itself, in discovery order).
  // `forced` are -include/-imacros files (resolved like quoted includes from cwd). *computed is set when any
  // scanned file has a non-literal include; *files_scanned counts the files visited (src included).
  std::vector<std::string> closure(const std::string& src_abs, const args::IncludeDirs& dirs,
                                   const std::vector<std::string>& system_dirs, const std::string& cwd,
                                   bool* computed, int* files_scanned = nullptr, bool* has_include_computed = nullptr);
  // For the direct cache: everything the include graph of a compile depends on, beyond closure():
  //   *extra:    the files reachable from `src_abs` / `closure_files` (the result of closure() for the same arguments)
  //              that closure() does not list — system headers (closure() does not enter them) and any non-system
  //              file reached only through them
  //   *neg:      every include-search candidate probed ABSENT while resolving any directive of those files (system
  //              ones included; #include_next: every directory), as (directory id, name id) — see negative_groups()
  //   *computed: the files of the graph with a non-literal #include (the caller must account for what they reached)
  //   *has_include_computed: some file has __has_include(MACRO)
  // Memoised like closure() (adjacency with system files, per configuration); the memoised parses may be up to
  // kValidateTtl old, so the caller must check every file it relies on with parse_matches().
  void direct_inputs(const std::string& src_abs, const std::vector<std::string>& closure_files,
                     const args::IncludeDirs& dirs, const std::vector<std::string>& system_dirs, const std::string& cwd,
                     std::vector<std::string>* extra, std::vector<std::pair<int, int>>* neg, std::vector<std::string>* computed,
                     bool* has_include_computed);
  // The memoised parse of `path` describes the file whose current stat is `st` (false: not parsed, or changed).
  bool parse_matches(const std::string& path, const direct::FStat& st) const;
  // batch form (one shared lock): index of the first path in `check` whose memoised parse is stale, or -1
  long first_stale_parse(const std::vector<std::string>& paths, const std::vector<direct::FStat>& st, const std::vector<char>& check) const;
  // Absent candidates given as paths (e.g. computed-include candidates) → (directory id, name id) pairs.
  void intern_candidates(const std::vector<std::string>& paths, std::vector<std::pair<int, int>>* out);
  // The direct entry's NEGATIVE groups for `neg`: per directory its stat (taken before any name is probed) and the
  // names, all verified absent now (a directory snapshot from earlier closures spares re-probing names already
  // verified). VOLATILE when the directory changed (ctime) at or after t_start_wall - kRacyS. Returns false when a
  // candidate exists now (the caller must not record).
  bool negative_groups(std::vector<std::pair<int, int>>& neg, double t_start_wall, std::vector<direct::NegGroup>* out);
  size_t cache_size() const { std::shared_lock<std::shared_mutex> g(mu_); return files_.size(); }
  struct Stats { uint64_t parses = 0, stats = 0, resolve_probes = 0, resolve_hits = 0, adj_hits = 0, state_hits = 0, shadow_invalidations = 0; };
  Stats stats() const { std::shared_lock<std::shared_mutex> g(mu_); Stats s = st_; s.adj_hits += adj_hits_.load(); return s; }
  // Build-epoch boundary: cached files are re-stat()ed before their next use; after an idle gap > 1 s include
  // resolutions are re-probed as well (see incscan.cpp).
  void new_epoch(double idle_gap_s);
  // Persistent parse memo (survives coordinator restarts). export_state() serialises every parsed file whose
  // parse happened >= 2 s after its last modification (racy-file guard); import_state() preloads records that are used
  // only when a fresh stat() matches their (mtime_ns, size, inode, ctime_ns) exactly (format "SCN4"; other files are ignored) — otherwise the file is read and parsed as usual.
  std::string export_state() const;
  void import_state(const std::string& blob);

 private:
  struct Config { std::vector<std::string> quote_extra, bracket_search, system_dirs; };
  struct File { std::string path; FileScan scan; double validated_at = 0; bool parsed = false; };
  int file_id_locked(const std::string& abs);               // mu_ held
  void ensure_parsed(int id);                               // takes mu_ internally; I/O outside it
  int resolve(int cfg, const Directive& d, const std::string& includer_dir);   // -1 none, -2 system
  // One memoised adjacency: the resolved non-system includes and every include-search candidate that was probed
  // ABSENT while resolving them, as (directory id, name id) — a file created there would shadow the resolution.
  struct Adj { std::vector<int> ids; std::vector<std::pair<int, int>> neg; };
  Adj adjacency_full(int cfg, int id);
  Adj adjacency_all(int cfg, int id);   // As adjacency_full, system files included (ids), for direct_inputs
  void absent_pairs_locked(const std::string& key, std::vector<std::pair<int, int>>* out);   // mu_ held (exclusive)
  // Exact shadowing: true when every candidate in `neg` is still absent. Per directory: one stat(); a directory
  // whose stat is unchanged since a snapshot taken before its names were verified absent (and that had not changed
  // within kRacyS of that snapshot) cannot have gained an entry, so its verified names are not probed again.
  // On false, *found is the candidate that exists now.
  bool verify_absent(std::vector<std::pair<int, int>>& neg, std::pair<int, int>* found);
  void invalidate_candidate(const std::pair<int, int>& cand);   // drop every memo that relied on its absence
  std::vector<std::string> closure_once(int cfg, const std::string& src_abs, const args::IncludeDirs& dirs, const std::string& cwd,
                                        bool* computed, int* files_scanned, bool* has_include_computed, std::vector<std::pair<int, int>>* neg);
  int config_id(const args::IncludeDirs& dirs, const std::vector<std::string>& system_dirs, const std::string& cwd);
  static std::string res_key(int cfg, const Directive& d, const std::string& includer_dir);
  std::vector<int> adjacency(int cfg, int id);              // memoised resolved, non-system includes (copy: memo may be erased concurrently)

  mutable std::shared_mutex mu_;   // shared for the warm DFS, exclusive for every mutation
  std::atomic<uint64_t> adj_hits_{0};
 public:
  std::atomic<uint64_t> verify_ns{0}, verify_calls{0}, direct_inputs_ns{0};   // profiling counters
 private:
  std::unordered_map<std::string, int> cfg_ids_; std::vector<Config> cfgs_;
  std::unordered_map<std::string, int> id_of_; std::vector<File> files_;
  std::unordered_map<std::string, int> res_;                // (cfg, kind, includer_dir, name) → id / -1 (none) / -2 (system)
  std::unordered_map<std::string, double> neg_at_;
  std::unordered_map<std::string, std::vector<std::string>> absent_;   // res_ key → candidates probed and absent
  std::unordered_map<std::string, std::string> sys_found_;             // res_ key → path, for system (-2) results
  std::unordered_map<std::string, uint64_t> res_gen_; uint64_t res_gen_cur_ = 0;   // resolution generation per key
  double epoch_ = 0;                                         // validations before this instant are not trusted
  bool fresh_locked(double validated_at, double tnow) const;
  bool res_epoch_ok_locked(const std::string& key) const;          // when a negative answer was recorded (expires after kValidateTtl)
  std::unordered_map<uint64_t, Adj> adj_;                   // (cfg<<32 | id) → resolved include ids + absent candidates
  std::unordered_map<uint64_t, Adj> adjx_;                  // the same with system files included (direct_inputs)
  // directory / name interning and per-directory absence snapshots (dmu_; never held together with mu_ exclusive)
  struct DirSnap { std::string path; bool valid = false, vol = false; int64_t mtime_ns = 0, ctime_ns = 0; uint64_t dev = 0, ino = 0;
                   bool exists = false; std::unordered_set<int> ok; };
  mutable std::mutex dmu_;
  std::unordered_map<std::string, int> dir_id_; std::deque<DirSnap> dirs_;
  std::unordered_map<std::string, int> name_id_; std::deque<std::string> names_;
  std::unordered_map<uint64_t, std::vector<uint64_t>> cand_adj_;   // (dir id<<32 | name id) → adj_ keys relying on it (mu_)
  int intern_dir_locked(const std::string& d); int intern_name_locked(const std::string& n);   // dmu_ held
  std::vector<char> is_system_;                             // per file id
  Stats st_;
  std::unordered_map<std::string, FileScan> preloaded_;   // path -> persisted parse (mu_)
};

}  // namespace opticc::incscan
