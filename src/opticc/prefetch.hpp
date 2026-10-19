// opticc: header prefetch.
//
// The coordinator knows exactly which content it has shipped to which worker (PREFETCH and BLOBS frames), so it keeps
// a per-node "held" set of shas. Three uses:
//   1. piggyback: right before a job's COMPILE_HP, the headers the node is not known to hold are pushed in a PREFETCH
//      frame on the same FIFO connection, so the worker never answers with NEED (saves one round trip per cold job);
//   2. background warm-up: a rate-limited loop (a configured fraction of the node's measured uplink, default 30%)
//      pushes the most popular headers — ranked by how many closures reference them — that the node lacks, so later
//      jobs find their headers already there and ship only their source;
//   3. warm start: the popularity ranking of the previous build is persisted and seeds (2) at connect time, up to a
//      byte budget per node (default 64 MB), so even the first jobs of a build mostly hit.
// Everything is content-addressed (sha256), so nothing is ever sent twice to a node and stale content cannot be
// served: a header edited since the ranking was saved simply hashes to a new sha. The worker re-verifies every blob.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opticc::prefetch {

// Token bucket: `rate` bytes/s refill, `burst` bytes capacity. take() never blocks; it says how much may be sent now.
class TokenBucket {
 public:
  TokenBucket(double rate = 0, double burst = 0) : rate_(rate), burst_(burst), tokens_(burst) {}
  void set(double rate, double burst) { rate_ = rate; burst_ = burst; if (tokens_ > burst_) tokens_ = burst_; }
  double available(double now);                 // refills to `now` and returns the current tokens
  bool take(double bytes, double now);          // consumes `bytes` if available (after refill); false otherwise
  double rate() const { return rate_; }
 private:
  double rate_, burst_, tokens_, last_ = -1;
};

struct FileEnt { std::string path; uint64_t size = 0; uint64_t refs = 0; };

class Tracker {
 public:
  explicit Tracker(int nodes = 0) { resize(nodes); }
  void resize(int nodes);                                   // node ids 1..nodes (index 0 unused = client)
  // held sets
  bool held(int node, const std::string& sha) const;
  void mark(int node, const std::string& sha, uint64_t bytes);
  void forget_node(int node);                               // worker lost / restarted: assume an empty store
  size_t held_count(int node) const;
  // popularity
  void note(const std::string& sha, const std::string& path, uint64_t size, uint64_t weight = 1);
  // the popular files node lacks, most referenced first, whose sizes fit in `budget_bytes`; at most `max_items`
  std::vector<std::pair<std::string, FileEnt>> pick(int node, uint64_t budget_bytes, size_t max_items) const;
  // the members of `shas` (with sizes) the node is not known to hold
  template <class It> std::vector<std::string> missing(int node, It b, It e) const {
    std::lock_guard<std::mutex> g(m_); std::vector<std::string> out;
    for (; b != e; ++b) if (!held_.at(static_cast<size_t>(node)).count(*b)) out.push_back(*b);
    return out;
  }
  // persistence of the ranking (warm start): top `n` paths, most referenced first, one per line
  std::vector<std::string> top_paths(size_t n) const;
  // the same ranking with each file's sha and size (the stat-validated warm list)
  std::vector<std::pair<std::string, FileEnt>> top(size_t n) const;
  uint64_t pushed_bytes(int node) const;
 private:
  mutable std::mutex m_;
  std::vector<std::unordered_set<std::string>> held_;
  std::vector<uint64_t> pushed_;
  std::unordered_map<std::string, FileEnt> files_;          // sha -> path/size/refs
  // pick() runs every 100 ms per node; without this memo it would scan every known file even when the node holds them
  // all. A node that held every file at keys_gen_ needs no rescan until a new
  // file is noted (keys_gen_ grows) or its held set is cleared (forget_node); mark() only adds held files and a refs
  // change reorders candidates without creating one, so the memo is exact.
  uint64_t keys_gen_ = 1;
  mutable std::vector<uint64_t> all_held_at_;               // per node: keys_gen_ at which it held every file (0 = unknown)
};

}  // namespace opticc::prefetch
