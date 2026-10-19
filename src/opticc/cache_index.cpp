// SPDX-License-Identifier: GPL-2.0-only
#include "cache_index.hpp"

#include <algorithm>

#include "opticc/cachekey.hpp"

namespace opticc::cacheidx {

std::string client_manifest_key(const std::vector<std::string>& argv, int source_index, const std::string& cwd, const std::string& src_sha,
                                const std::string& cc_identity) {
  std::string cid = cachekey::compiler_id(!cc_identity.empty() ? cc_identity : (argv.empty() ? "" : argv[0]));
  if (cid.empty()) return {};   // no compiler identity, no key (no cache for this job)
  return cachekey::manifest_key(cid, cachekey::normalise_argv(argv, source_index, cwd), src_sha);
}
std::string client_classic_key(const std::vector<std::string>& argv, int source_index, const std::string& pre_sha) {
  std::string cid = cachekey::compiler_id(argv.empty() ? "" : argv[0]);
  if (cid.empty()) return {};
  return cachekey::classic_key(cid, cachekey::normalise_argv(argv, source_index, ""), pre_sha);
}

void CacheIndex::record_result(const std::string& key, int node, const std::string& st) {
  if (key.empty() || node <= 0) return;
  std::lock_guard<std::mutex> g(mu_);
  if (st == "hit") { by_key_[key].insert(node); ++hits_; }
  else if (st == "store") by_key_[key].insert(node);
}
void CacheIndex::record_have(int node, const std::vector<std::string>& keys, const std::vector<bool>& have) {
  if (node <= 0) return;
  std::lock_guard<std::mutex> g(mu_);
  for (size_t i = 0; i < keys.size() && i < have.size(); ++i) {
    if (have[i]) by_key_[keys[i]].insert(node);
    else { auto it = by_key_.find(keys[i]); if (it != by_key_.end()) { it->second.erase(node); if (it->second.empty()) by_key_.erase(it); } }
  }
}
std::vector<int> CacheIndex::holders(const std::string& key) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = by_key_.find(key);
  if (it == by_key_.end()) return {};
  std::vector<int> v(it->second.begin(), it->second.end()); std::sort(v.begin(), v.end()); return v;
}
void CacheIndex::forget_node(int node) {
  std::lock_guard<std::mutex> g(mu_);
  for (auto it = by_key_.begin(); it != by_key_.end();) { it->second.erase(node); if (it->second.empty()) it = by_key_.erase(it); else ++it; }
}
void CacheIndex::invalidate(const std::string& key) { std::lock_guard<std::mutex> g(mu_); by_key_.erase(key); }

}  // namespace opticc::cacheidx
