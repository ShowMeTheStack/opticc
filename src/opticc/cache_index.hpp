// opticc coordinator: compile-cache index and affinity glue.
//
// The coordinator learns which worker holds which cache key from two sources:
//   * RESULT frames: cache == "hit" or "store" means the worker now holds that manifest key (M, header-push) or
//     classic key (C);
//   * CACHE_QUERY/CACHE_HAVE batches (proto::encode_cache_query / decode_cache_have), e.g. at coordinator start to
//     warm the index from caches that survived a previous build.
// Before a job is decided the coordinator computes the job's key exactly as the worker will (client_manifest_key),
// asks the index for holders and calls Scheduler::set_affinity(job, holders, hit_cost_s). ECT then prices a hit on a
// holding node as source-upload + lookup + object download, with no worker core (sched.cpp, predicted_completion).
//
// How src/opticc/coordinator.cpp uses it:
//   1. after the job's header-push request is built (source bytes + argv + cwd known), before run_decide_locked():
//        std::string key = cacheidx::client_manifest_key(argv, scan.source_index, cwd, sha256(source));
//        job.cache_key = key;
//        sched->set_affinity(job_id, index.holders(key), cacheidx::kDefaultHitCost);
//      (classic mode: cacheidx::client_classic_key(argv, scan.source_index, sha256(preprocessed)) after -E.)
//   2. on every RESULT from node k:   index.record_result(res.cache_key, k, res.cache);
//      and count res.cache == "hit" in the summary (field "cache": "hit|miss|store|off" per job record).
//   3. on worker loss / disable_node(k):  index.forget_node(k);
//   4. optional warm start: for each connected worker k, send proto::encode_cache_query({keys}) for the keys of the
//      jobs known so far and feed the CACHE_HAVE reply to index.record_have(k, keys, have.have).
//   5. the worker must be started with --object-cache DIR, otherwise every RESULT reports cache "off".
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opticc::cacheidx {

constexpr double kDefaultHitCost = 0.002;   // worker-side lookup + object read (s); transfers are added by the scheduler

// Keys computed on the client exactly as opticcd computes them (src/opticcd/main.cpp, opticc/cachekey.hpp).
// cc_identity: the compiler whose identity keys the entry (the wrapper-resolved path); "" = argv[0] resolved here
std::string client_manifest_key(const std::vector<std::string>& argv, int source_index, const std::string& cwd, const std::string& src_sha,
                                const std::string& cc_identity = std::string());
std::string client_classic_key(const std::vector<std::string>& argv, int source_index, const std::string& preprocessed_sha);

class CacheIndex {
 public:
  // cache_status as reported in RESULT: "hit" | "store" mean node holds key; "miss" | "off" say nothing new.
  void record_result(const std::string& key, int node, const std::string& cache_status);
  void record_have(int node, const std::vector<std::string>& keys, const std::vector<bool>& have);
  std::vector<int> holders(const std::string& key) const;      // sorted, possibly empty
  void forget_node(int node);
  void invalidate(const std::string& key);
  size_t size() const { std::lock_guard<std::mutex> g(mu_); return by_key_.size(); }
  uint64_t hits_seen() const { std::lock_guard<std::mutex> g(mu_); return hits_; }
 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unordered_set<int>> by_key_;
  uint64_t hits_ = 0;
};

}  // namespace opticc::cacheidx
