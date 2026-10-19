// opticc: SHARED DIRECT MANIFESTS ("remote direct"): the fresh-client fast path.
//
// Problem: a NEW client (empty client cache) against warm workers would turn every TU into a coordinator job:
// include-scan the whole closure, hash it, ship the header-push request, let the affinity worker look up its object,
// transfer it back and store it, whereas ccache with shared remote storage fetches one manifest and one object by key.
// The worker tier already holds every object; what the fresh client lacks is the knowledge of WHICH files a TU reads,
// which is exactly what a direct manifest records (opticc/direct.hpp).
//
// Design. The coordinator that records a direct entry (direct.hpp) also PUBLISHES it, in a machine-independent form,
// to one worker (chosen by hash of the portable key, as the write-back chooses): a portable entry keeps every
// recorded input's absolute path, content hash and size (no stat: stat is per machine) and every negative probe
// (directory + absent names), together with the object, its dependency-file body and its output. A fresh client's
// coordinator, on a local miss, asks that worker for the manifest of the TU's portable key, validates an entry against
// ITS OWN files by content (memoised per build: a file's hash is computed once, keyed by exact stat) and by listing the
// probed directories, and on success serves the object without scanning, scheduling or compiling — then records the
// entry locally, so the next build's wrapper answers it by itself.
//
//   portable key  P = sha256("opticc-rdirect-v3" | toolchain fingerprint F | argv exactly as given | cwd |
//                            include-affecting environment | source content hash as in the direct key (raw or key hash))
//   fingerprint   F = sha256(compiler path | compiler identity (cachekey::compiler_id: binary sha256 + version
//                            line) | system-include CONTENT fingerprint (sysinc: every file under the compiler's
//                            default C and C++ system include directories) | the system-search arguments of argv |
//                            language | the search list the compiler reports for exactly those arguments (`-E -v`) |
//                            compiler configuration files). ONE toolchain identity for every opticc cache: the
//                            write-back carries the same compiler_id and sysinc fingerprint. When the dependency file
//                            omits system headers (-MMD), every directory of that search list must lie inside the
//                            sysinc directories (whose content the fingerprint covers) — else the remote path refuses.
//
// Validation of a portable entry on the fetching machine (all must hold):
//   positive: the file at the recorded absolute path exists, is a regular file, and the hash of its CURRENT bytes
//             (raw sha256, or the comment-insensitive key hash for keysha entries) equals the recorded one
//   negative: every probed name is absent now (directory listing, memoised by exact directory stat when settled)
//
// Soundness. Let the recording machine R and the fetching machine Q have
// the same F (same compiler bytes at the same path, the same system search list, and — under -MMD — the same system
// header bytes), and let the compile's argv, cwd and environment be equal (they are in P). The recorded entry lists
// every file R's compile read (scanner closure ∪ dependency file ∪ source; system headers are in F under -MMD) and
// every include-search candidate that did not exist when it ran. If on Q every listed file has the recorded content and
// every candidate is absent, then Q's preprocessor, searching the same directories in the same order (same argv, cwd,
// system list), resolves every #include, #include_next and __has_include (computed ones are never recorded) to the same
// path, reads the same bytes, and produces the same token stream; a deterministic compiler (same binary) then emits
// the same object, diagnostics and dependency file. The stat-based shortcut of direct.hpp is deliberately NOT used for
// portable entries — only content: stat is meaningful only on the machine that took it. The racy rules guard only the
// memos (a hash or a listing is memoised only when the file / directory was settled — ctime < t - margin — and its
// stat did not change while it was read); a memo hit requires the exact same stat.
// Trust: like ccache's and Bazel's shared remote storage, authorised clients are trusted to publish their own
// compiler's output; the worker verifies the object's sha256 against the published one (transport integrity) and keeps
// published objects in a namespace of their own (never served to a COMPILE_HP). --no-cache-uploads refuses publishing.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "opticc/direct.hpp"
#include "opticc/protocol.hpp"

namespace opticc::rdirect {

// fsha: BLAKE2b-256 of the EXACT bytes the entry was recorded with, when the publisher could prove they are the
// recorded ones (its stat still equal to the recorded, settled stat); "" = unknown. Validation compares it first (one
// fast hash, no tokenizing); equal bytes imply an equal sha of either kind. A mismatch falls back to `sha`.
struct PPos { std::string path, sha; uint64_t size = 0; std::string fsha; };
struct PNeg { std::string dir; std::vector<std::string> names; };
struct PEntry {
  std::string obj, obj_sha;   // object key (as on the recording client) and sha256 of the object bytes
  bool keysha = false;
  double recorded_wall = 0;
  std::vector<PPos> pos;
  std::vector<PNeg> neg;
};
constexpr size_t kMaxEntries = 8;

PEntry from_direct(const direct::Entry& e, const std::string& obj_sha);
std::string encode(const std::vector<PEntry>& es);
bool decode(const std::string& blob, std::vector<PEntry>* es);

std::string pkey(const std::string& fingerprint, const std::vector<std::string>& argv, const std::string& cwd,
                 const std::vector<std::string>& env, const std::string& src_sha);
// The environment a portable entry may depend on: only variables that do not change the include search (the others —
// CPATH, C_INCLUDE_PATH, COMPILER_PATH, ... — make the remote path refuse the compile, see env_ok).
bool env_ok(const std::vector<std::string>& env);
// The arguments of argv that change the SYSTEM include search (and therefore the fingerprint), in order.
std::vector<std::string> system_search_args(const std::vector<std::string>& argv);

// ---- client side: content validation with per-machine memos (thread-safe) -------------------------------------
class Validator {
 public:
  // On success, *local (optional) receives a LOCAL direct entry built from the validation (current stats; obj = e.obj)
  // when every positive and probed directory was settled — the caller may record it (direct::record). t0 = start.
  bool valid(const PEntry& e, std::string* why, direct::Entry* local = nullptr);
  enum Kind { Raw = 0, Key = 1, Fast = 2 };   // raw sha256 | comment-insensitive key hash | BLAKE2b-256 (fsha)
  std::string hash_of(const std::string& path, const direct::FStat& st, Kind k);   // "" if unreadable or changing
  std::string content_sha_of(const std::string& path, const direct::FStat& st, bool keysha) { return hash_of(path, st, keysha ? Key : Raw); }
  uint64_t hashed_files() const { return hashed_.load(); }
  uint64_t listed_dirs() const { return listed_.load(); }
  std::atomic<uint64_t> ns_stat{0}, ns_hash{0}, ns_neg{0};   // time split (summary): WALL time, summed over threads
  // THREAD CPU time of the same phases (wall minus CPU = waiting: I/O, locks, run queue)
  std::atomic<uint64_t> cpu_stat{0}, cpu_hash{0}, cpu_neg{0};
  // per hash kind (Raw, Key, Fast): digests computed (memo misses), bytes digested, thread CPU of the read + digest,
  // and wall time of the read + digest; memo hits; fsha absent or not matching (then the content hash is computed)
  std::atomic<uint64_t> n_digest[3]{}, bytes_digest[3]{}, cpu_digest[3]{}, ns_digest[3]{};
  std::atomic<uint64_t> memo_hits{0}, no_fsha{0}, fsha_mismatch{0}, positives{0}, entries{0};
  uint64_t distinct_files() const { std::lock_guard<std::mutex> g(dmu_); return digested_paths_.size(); }   // distinct paths digested
  static uint64_t thread_cpu_ns();
  std::atomic<uint64_t> neg_access{0}, neg_listed{0};   // absent names decided by access() / by a memoised listing
  // access() by reason: 0 directory missing, 1 listing not pinned (unsettled/changing), 2 '.'/'..'/empty component, 3 last component listed
  std::atomic<uint64_t> neg_access_why[4]{};
 private:
  bool names_absent(const std::string& dir, const std::vector<std::string>& names, direct::FStat* dst);
  // 1 = `name` (one component) is in dir's listing, 0 = it is not, -1 = no stable listing (probe instead)
  // (`seen`: the group's directory stats and component answers, so each directory is stat()ed once per group)
  struct GroupSeen { std::unordered_map<std::string, direct::FStat> st; std::unordered_map<std::string, int> ans; };
  int listing_has(const std::string& dir, const std::string& name, GroupSeen* seen);
  bool rel_absent(const std::string& dir, const std::string& rel, GroupSeen* seen);   // a name with '/', component by component
  bool dir_absent_pinned(const std::string& dir, GroupSeen* seen);   // a missing directory, proved from an ancestor's listing
  struct HMemo { direct::FStat st; std::string h[3]; };
  struct LMemo { direct::FStat st; std::vector<std::string> names; };   // sorted
  std::shared_mutex hmu_, lmu_;
  std::unordered_map<std::string, HMemo> hmemo_;
  std::unordered_map<std::string, LMemo> lmemo_;
  std::atomic<uint64_t> hashed_{0}, listed_{0};
  mutable std::mutex dmu_; std::unordered_set<std::string> digested_paths_;
  // digests in progress, so that concurrent memo misses of one settled file digest it once
  struct InFlight { direct::FStat st; std::mutex m; std::condition_variable cv; bool done = false; std::string h; };
  std::mutex fmu_; std::unordered_map<std::string, std::shared_ptr<InFlight>> inflight_[3];
public:
  std::atomic<uint64_t> shared_waits{0};   // memo misses answered by another thread's digest in progress
};

// ---- client side: toolchain fingerprint (memoised per compiler / system-search arguments / language) -----------
class Toolchain {
 public:
  // "" (with *why) when the compile cannot use the remote path. compiler_id / sysinc_fp: the identities of the
  // compiler (cachekey::compiler_id) and of its system include directories (sysinc content fingerprint); sysinc_dirs:
  // those directories (sysinc::dirs_for). Empty identities refuse (not memoised: they may become available).
  std::string fingerprint(const std::string& cc_resolved, const std::vector<std::string>& argv, const std::string& lang,
                          bool deps_list_system_headers, const std::string& compiler_id, const std::string& sysinc_fp,
                          const std::vector<std::string>& sysinc_dirs, std::string* why);
 private:
  std::mutex mu_;
  std::map<std::string, std::string> fp_;      // memo key -> F ("" = refused, reason in why_)
  std::map<std::string, std::string> why_;
  Validator files_;                             // content memo for configuration files
};

// ---- worker side: the store of published manifests (one file per portable key, zstd) --------------------------
class Store {
 public:
  void init(const std::string& dir);
  bool enabled() const { return !dir_.empty(); }
  // Merge `e` into P's manifest (dedup by object key; the newest kMaxEntries entries, newest last).
  bool put(const std::string& pkey, const PEntry& e);
  bool get(const std::string& pkey, std::string* encoded);   // the encoded entries (rdirect::encode)
  bool get_stored(const std::string& pkey, std::string* zstored);   // the stored (compressed) form, sent as is
  std::vector<std::string> keys() const;                     // for the HELLO_ACK Bloom filter
  size_t count() const { std::lock_guard<std::mutex> g(kmu_); return keys_.size(); }
 private:
  std::string path(const std::string& pkey) const { return dir_ + "/" + pkey.substr(0, 2) + "/" + pkey; }
  std::string dir_;
  mutable std::mutex kmu_; std::unordered_set<std::string> keys_;
  std::mutex lock_[64];
};
// Object key of a published object in the worker's object cache (its own namespace: never a COMPILE_HP key). Keyed
// by the object key AND the sha of its bytes, so a later publisher whose compile produced other bytes for the same
// object key does not find the first publisher's object in its place.
std::string worker_object_key(const std::string& okey, const std::string& obj_sha);
// The entry a DIRECT_GET serves: req "" = the newest; "okey" = the newest with that object key; "okey@sha" = the newest
// with that object key and object sha (nullptr: none)
const PEntry* select_for_get(const std::vector<PEntry>& es, const std::string& req);

// ---- wire (protocol.hpp Type::DirectPut / DirectGet / DirectReply) ----------------------------------------------
// DIRECT_PUT  (client -> worker, no reply) header {pkey, okey, obj_sha, rc, stdout, stderr, depfile_body,
//              entry_raw_len, obj_raw_len, zstd_len} + zstd(entry || object)        entry = encode({one PEntry})
// DIRECT_GET  (client -> worker) JSON {id, pkey, okey}  okey "" = "the newest entry's object"
// DIRECT_REPLY (worker -> client) header {id, have, okey, obj_sha, rc, stdout, stderr, depfile_body,
//              entries_zlen, obj_raw_len, zstd_len} + stored(entries) || zstd(object)   object omitted if not held;
//              the entries travel in the store's compressed form ("OPRZ" u32 raw_len zstd), never re-encoded
struct Object { std::string okey, obj_sha, object, depfile_body, out, err; int rc = 0; };
// The KIND of a key travels with PUT and GET; "" (absent: older clients) = a direct
// key, "pp" = a preprocessed-text key, "ppidx" = its text-free index key (no object; it only enters the HELLO filter)
struct PutMsg { std::string pkey; PEntry entry; Object obj; std::string kind; };
struct GetMsg { int64_t id = 0; std::string pkey, okey; std::string kind; };
// An entry fits a kind: direct entries list inputs (positives); pp and ppidx entries list none (the key holds the text)
bool entry_fits_kind(const PEntry& e, const std::string& kind);
// The entries of `es` that fit `kind` (a worker serves only those; a mixed set needs a malformed publisher)
std::vector<PEntry> filter_kind(const std::vector<PEntry>& es, const std::string& kind);
struct ReplyMsg { int64_t id = 0; bool have = false; std::string entries; Object obj; std::string entries_z; };   // entries_z: sender side
proto::Frame encode_put(const PutMsg& m, uint64_t* wire_bytes = nullptr);
PutMsg decode_put(const proto::Frame& f);
proto::Frame encode_get(const GetMsg& m);
GetMsg decode_get(const proto::Frame& f);
proto::Frame encode_reply(const ReplyMsg& m);
ReplyMsg decode_reply(const proto::Frame& f);

}  // namespace opticc::rdirect
