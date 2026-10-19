// opticcd: on-disk compile cache (keys in opticc/cachekey.hpp).
// Layout: <dir>/m/<M>.json  manifest: {"entries":[{"closure":[[relpath,sha],...],"shape":S,"obj":O}]}   (header-push)
//         S = shape_hash(view): sha256 of the sorted relative PATHS of the client's shipped closure. An entry matches
//         only if the shape is identical and every used (path, sha) is identical: a header newly added earlier in
//         the include search path (shadowing a used one under a different path) changes the shape -> miss.
//         <dir>/o/<O>       object bytes
//         <dir>/o/<O>.meta  {"depfile_body":..., "stdout":..., "stderr":..., "rc":0}
// LRU by last use over object bytes; the in-memory manifest index answers CACHE_QUERY without touching disk.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "opticc/cachekey.hpp"

namespace opticc::objcache {

struct Hit { std::string object, depfile_body, out, err; int rc = 0; std::string obj_key; };

class ObjectCache {
 public:
  void init(const std::string& dir, uint64_t cap_bytes);
  bool enabled() const { return !dir_.empty(); }
  const std::string& dir() const { return dir_; }
  // Header-push: find an entry of manifest M whose used closure matches the client's view exactly.
  // `view` maps relative path -> sha (the shipped closure + source). Returns true and fills *hit on success.
  bool lookup_manifest(const std::string& mkey, const std::unordered_map<std::string, std::string>& view, Hit* hit);
  static std::string shape_hash(const std::unordered_map<std::string, std::string>& view);
  // As lookup_manifest, but the caller gives the shape and a sha_of(relpath) callback (returns "" if unreadable),
  // so files the compile read outside the shape's view are verified by content on demand.
  bool lookup_manifest_fn(const std::string& mkey, const std::string& shape, const std::function<std::string(const std::string&)>& sha_of, Hit* hit);
  // As above, but the caller verifies an entry's whole closure in one call (batched, stat-validated hashing)
  bool lookup_manifest_batch(const std::string& mkey, const std::string& shape,
                             const std::function<bool(const std::vector<std::pair<std::string, std::string>>&)>& verify, Hit* hit);
  // Direct object lookup (classic key or an O computed by the caller).
  bool lookup_object(const std::string& okey, Hit* hit);
  // Store an object under O and (header-push) append {closure, O} to manifest M.
  bool store(const std::string& okey, const std::string& object, const std::string& depfile_body, const std::string& out, const std::string& err, int rc);   // false: nothing stored
  // fkey: the full content key of the view the entry was stored with (cachekey::full_key). The worker advertises
  // it in its Bloom filter, so a client's affinity probe hits only for the exact same closure content
  void add_manifest_entry(const std::string& mkey, const cachekey::Closure& used_rel, const std::string& okey, const std::string& shape,
                          const std::string& fkey = "");
  std::vector<std::string> full_keys() const;   // every entry's fkey (for the Bloom filter)
  bool has_manifest(const std::string& mkey) const { std::lock_guard<std::mutex> g(mu_); return manifests_.count(mkey) > 0; }
  // every manifest key (for the HELLO_ACK Bloom filter, opticc/bloom.hpp)
  std::vector<std::string> manifest_keys() const { std::lock_guard<std::mutex> g(mu_); std::vector<std::string> v; v.reserve(manifests_.size()); for (auto& [k, m] : manifests_) v.push_back(k); return v; }
  bool has_object(const std::string& okey) const { std::lock_guard<std::mutex> g(mu_); return objects_.count(okey) > 0; }
  uint64_t total_bytes() const { std::lock_guard<std::mutex> g(mu_); return total_; }
  size_t object_count() const { std::lock_guard<std::mutex> g(mu_); return objects_.size(); }
  size_t manifest_count() const { std::lock_guard<std::mutex> g(mu_); return manifests_.size(); }
  uint64_t hits = 0, misses = 0, stores = 0;   // counters (informational; updated under mu_)
 private:
  struct Ent { uint64_t size; uint64_t last; };
  std::string opath(const std::string& o) const { return dir_ + "/o/" + o; }
  std::string mpath(const std::string& m) const { return dir_ + "/m/" + m + ".json"; }
  void evict_locked();
  std::string dir_; uint64_t cap_ = 0, total_ = 0, tick_ = 0;
  std::unordered_map<std::string, Ent> objects_;
  std::unordered_map<std::string, nlohmann::json> manifests_;   // M -> {"entries":[...]}
  // parsed, immutable snapshots of each manifest for lock-free lookups (rebuilt when an entry is added)
  struct MEntry { std::string shape, obj; std::vector<std::pair<std::string, std::string>> closure; };
  std::unordered_map<std::string, std::shared_ptr<const std::vector<MEntry>>> mnat_;
  static std::shared_ptr<const std::vector<MEntry>> to_native(const nlohmann::json& m);
  mutable std::mutex mu_;
};

}  // namespace opticc::objcache
