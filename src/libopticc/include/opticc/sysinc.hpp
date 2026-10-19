// SPDX-License-Identifier: GPL-2.0-only
// Identity of a compiler's SYSTEM include directories.
//
// Problem. Every opticc cache keeps system headers out of its keys: the dependency file of a -MMD compile omits them,
// the client view and the worker's closure skip /usr/..., and header-push never ships them. A system-header upgrade
// (an apt/dnf update of libc6-dev or libstdc++) would leave every cached object reachable under its old key: a
// stale hit. The worker cache would also file objects uploaded by a client (write-back) without knowing which
// system headers the client compiled against.
//
// Fix. A fingerprint of the system include directories the compiler searches (its `-E -v` search list for C and C++,
// recursively, regular files) enters every cache key that omits system headers:
//   - stat signature  S = H(path, size, mtime_ns, ctime_ns, ino of every file, and of every directory) — cheap (one
//     lstat per file), used where only THIS machine matters (client cache, direct path, worker's own compiles);
//   - content fingerprint F = H(path, sha256(content) of every file) — machine-independent, used to decide whether a
//     result compiled on ANOTHER machine (a client's upload) is valid here: accepted only on equal F.
// Freshness. A Tracker re-walks every `refresh` seconds in a background thread (S only; F is recomputed — re-hashing
// only files whose stat changed, memoised on disk — when S changes) and bumps a generation counter. Stores compare the
// generation at compile start and at store time and refuse on a change. Residual assumption:
// system headers are not modified while a build that stores into the cache is running, or are detected within
// `refresh` seconds (default 10).
#pragma once
#include <atomic>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opticc::sysinc {

// System include directories of `compiler` for C and C++ (union, de-duplicated, canonical, existing).
std::vector<std::string> dirs_for(const std::string& compiler);

struct FileSig { int64_t size = 0, mtime_ns = 0, ctime_ns = 0; uint64_t ino = 0; std::string sha; };
// Walk `dirs` (regular files and directories, recursively, symlinks not followed); returns the stat signature and fills
// `files` (path -> stat, sha left empty).
std::string stat_signature(const std::vector<std::string>& dirs, std::map<std::string, FileSig>* files = nullptr);
// Content fingerprint; `memo` (path -> FileSig with sha) is reused for files whose stat is unchanged and updated.
std::string content_fingerprint(const std::vector<std::string>& dirs, std::map<std::string, FileSig>* memo);

// time_macro_files: the fingerprinted headers that use __DATE__/__TIME__/__TIMESTAMP__
// (cachekey::uses_time_macros; "T:" in the memo), known once content_fp is. A TU that may read one is not stored under a
// content key (time_macro_input); other TUs of the compiler are unaffected.
struct State { std::string stat_sig, content_fp; uint64_t generation = 0; bool ready = false; std::vector<std::string> time_macro_files; };

// The time-macro header (of `tfiles`) the compile of `argv` in `cwd` may have read, or "". With a
// dependency file that lists system headers (`dep_lists_system`, -MD) exactly its inputs `dep_abs` are compared (by real
// path); otherwise every such header under a directory the compile searches: the compiler's own (`compiler_dirs`) unless
// -nostdinc, plus every -I / -isystem / -idirafter / -iquote directory and -include / -imacros file of argv. Arguments
// that change the system search (--sysroot, -isysroot, -target, ...) count every such header (conservative).
// The directories argv adds as SYSTEM directories (-isystem, -idirafter, -isystem-after), absolute.
// A -MMD dependency file omits every header found through them; those outside the fingerprinted directories must
// still enter a key (a project's own -isystem headers).
std::vector<std::string> explicit_system_dirs(const std::vector<std::string>& argv, const std::string& cwd);
std::string time_macro_input(const std::vector<std::string>& tfiles, const std::vector<std::string>& compiler_dirs,
                             const std::vector<std::string>& argv, const std::string& cwd,
                             const std::vector<std::string>& dep_abs, bool dep_lists_system);

class Tracker {
 public:
  // memo_dir: where per-compiler content memos are kept ("" = no persistence); refresh_s: background re-walk period.
  Tracker(std::string memo_dir, double refresh_s);
  ~Tracker();
  Tracker(const Tracker&) = delete; Tracker& operator=(const Tracker&) = delete;
  // State for `compiler` (resolved path). Computes it synchronously on first use (stat signature; the content
  // fingerprint only if `need_content`). Thread-safe.
  State get(const std::string& compiler, bool need_content);
  // Non-blocking: the state if it is already computed (with the content fingerprint if `need_content`), else
  // State{ready=false} after queueing the computation on the background thread (callers then do not cache).
  State peek(const std::string& compiler, bool need_content);
  // Called with (compiler, state) whenever a refresh changes a compiler's stat signature (e.g. to rewrite a file).
  void on_change(std::function<void(const std::string&, const State&)> f);
 private:
  struct Ent { std::vector<std::string> dirs; State st; std::map<std::string, FileSig> memo; bool content_done = false; };
  void loop();
  void load_memo(const std::string& compiler, Ent& e);
  void save_memo(const std::string& compiler, const Ent& e);
  std::string memo_dir_; double refresh_s_;
  std::mutex mu_; std::map<std::string, std::shared_ptr<Ent>> ents_;
  std::function<void(const std::string&, const State&)> cb_;
  std::vector<std::pair<std::string, bool>> todo_;   // requested by peek(): (compiler, need_content)
  std::atomic<bool> stop_{false}; std::thread thr_;
};

}  // namespace opticc::sysinc
