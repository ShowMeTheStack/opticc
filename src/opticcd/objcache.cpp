// SPDX-License-Identifier: GPL-2.0-only
#include "objcache.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <tuple>
#include <vector>

#include "opticc/util.hpp"

namespace opticc::objcache {
namespace fs = std::filesystem;
using json = nlohmann::json;

void ObjectCache::init(const std::string& dir, uint64_t cap) {
  dir_ = dir; cap_ = cap; util::mkdir_p(dir_ + "/o"); util::mkdir_p(dir_ + "/m");
  std::error_code ec;
  // least recently used first, by the object file's mtime (the client's direct path bumps it on every hit, which
  // the in-memory ticks never see)
  std::vector<std::tuple<fs::file_time_type, std::string, uint64_t>> found;
  for (auto& e : fs::directory_iterator(dir_ + "/o", ec)) {
    if (!e.is_regular_file()) continue;
    std::string name = e.path().filename().string();
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".meta") == 0) continue;
    if (name.find(".tmp.") != std::string::npos) continue;
    std::error_code e2; auto t = e.last_write_time(e2);
    found.emplace_back(t, std::move(name), e.file_size());
  }
  std::sort(found.begin(), found.end());
  for (auto& [t, name, sz] : found) { objects_[name] = {sz, ++tick_}; total_ += sz; }
  for (auto& e : fs::directory_iterator(dir_ + "/m", ec)) {
    if (!e.is_regular_file()) continue;
    std::string name = e.path().filename().string();
    if (name.size() <= 5 || name.compare(name.size() - 5, 5, ".json") != 0) continue;   // skip leftover .tmp. files
    try { std::string k = name.substr(0, name.size() - 5); manifests_[k] = json::parse(util::read_file(e.path().string())); mnat_[k] = to_native(manifests_[k]); } catch (...) {}
  }
}

std::shared_ptr<const std::vector<ObjectCache::MEntry>> ObjectCache::to_native(const json& m) {
  auto v = std::make_shared<std::vector<MEntry>>();
  for (auto& e : m.value("entries", json::array())) {
    MEntry me; me.shape = e.value("shape", ""); me.obj = e.value("obj", "");
    for (auto& pr : e.value("closure", json::array())) if (pr.is_array() && pr.size() == 2) me.closure.emplace_back(pr[0].get<std::string>(), pr[1].get<std::string>());
    v->push_back(std::move(me));
  }
  return v;
}

bool ObjectCache::lookup_object(const std::string& okey, Hit* hit) {
  {   // only the index is touched under the lock; files are read outside it so hits do not serialise
    std::lock_guard<std::mutex> g(mu_);
    auto it = objects_.find(okey);
    if (it == objects_.end()) { ++misses; return false; }
    it->second.last = ++tick_;
  }
  try {
    // an entry is valid only with its meta and an object of the recorded size. A crash between the renames, a
    // power loss before the data reached the disk (zero-length or short file) or an entry without a recorded size
    // is a miss and is dropped from the index
    json meta = json::parse(util::read_file(opath(okey) + ".meta"));
    hit->object = util::read_file(opath(okey));
    if (!meta.is_object() || !meta.contains("size") || meta.value("size", uint64_t(0)) != hit->object.size()) throw std::runtime_error("bad entry");
    // and the bytes must match the stored XXH64: a same-size corruption is a miss as well
    if (meta.value("xxh", std::string()) != util::xxh64_hex(hit->object)) throw std::runtime_error("bad entry");
    hit->depfile_body = meta.value("depfile_body", ""); hit->out = meta.value("stdout", ""); hit->err = meta.value("stderr", ""); hit->rc = meta.value("rc", 0);
    hit->obj_key = okey;
    std::lock_guard<std::mutex> g(mu_); ++hits; return true;
  } catch (...) {
    // the dropped entry's bytes leave the total too (a later re-store counts them again; otherwise the total
    // over-counts and good entries are evicted early)
    std::lock_guard<std::mutex> g(mu_);
    auto it = objects_.find(okey);
    if (it != objects_.end()) { total_ -= std::min(total_, it->second.size); objects_.erase(it); }
    ++misses; return false;
  }
}

std::string ObjectCache::shape_hash(const std::unordered_map<std::string, std::string>& view) {
  std::vector<std::string> paths; paths.reserve(view.size());
  for (auto& kv : view) paths.push_back(kv.first);
  std::sort(paths.begin(), paths.end());
  std::string s = "opticc-cache-v1|S";
  for (auto& p : paths) { s.push_back('\0'); s += p; }
  return util::sha256_hex(s);
}

bool ObjectCache::lookup_manifest(const std::string& mkey, const std::unordered_map<std::string, std::string>& view, Hit* hit) {
  std::string shape = shape_hash(view);
  json entries;
  { std::lock_guard<std::mutex> g(mu_); auto it = manifests_.find(mkey); if (it == manifests_.end()) { ++misses; return false; } entries = it->second.value("entries", json::array()); }
  for (auto& e : entries) {
    if (e.value("shape", "") != shape) continue;
    bool match = true;
    for (auto& pr : e.value("closure", json::array())) {
      if (!pr.is_array() || pr.size() != 2) { match = false; break; }
      auto v = view.find(pr[0].get<std::string>());
      if (v == view.end() || v->second != pr[1].get<std::string>()) { match = false; break; }
    }
    if (match && lookup_object(e.value("obj", ""), hit)) return true;
  }
  std::lock_guard<std::mutex> g(mu_); ++misses; return false;
}

bool ObjectCache::lookup_manifest_fn(const std::string& mkey, const std::string& shape, const std::function<std::string(const std::string&)>& sha_of, Hit* hit) {
  std::shared_ptr<const std::vector<MEntry>> ents;
  { std::lock_guard<std::mutex> g(mu_); auto it = mnat_.find(mkey); if (it == mnat_.end()) { ++misses; return false; } ents = it->second; }
  for (auto it = ents->rbegin(); it != ents->rend(); ++it) {   // newest first
    if (it->shape != shape) continue;
    bool match = true;
    for (auto& [rel, sha] : it->closure) { std::string now = sha_of(rel); if (now.empty() || now != sha) { match = false; break; } }
    if (match && lookup_object(it->obj, hit)) return true;
  }
  std::lock_guard<std::mutex> g(mu_); ++misses; return false;
}

bool ObjectCache::lookup_manifest_batch(const std::string& mkey, const std::string& shape,
                                       const std::function<bool(const std::vector<std::pair<std::string, std::string>>&)>& verify, Hit* hit) {
  std::shared_ptr<const std::vector<MEntry>> ents;
  { std::lock_guard<std::mutex> g(mu_); auto it = mnat_.find(mkey); if (it == mnat_.end()) { ++misses; return false; } ents = it->second; }
  for (auto it = ents->rbegin(); it != ents->rend(); ++it) {
    if (it->shape != shape) continue;
    if (verify(it->closure) && lookup_object(it->obj, hit)) return true;
  }
  std::lock_guard<std::mutex> g(mu_); ++misses; return false;
}

bool ObjectCache::store(const std::string& okey, const std::string& object, const std::string& depfile_body, const std::string& out, const std::string& err, int rc) {
  // The meta records the object's size and is written first, the object last; each through its own temporary file
  // (unique per process and call, so two stores of one key never share a name) renamed into place. A failed write or
  // close removes the temporary file and stores nothing (false). Every reader (lookup_object, the wrapper's direct
  // path) requires the meta and the recorded size, so an interrupted store, or one whose data never reached the
  // disk, is a miss. No fsync: a lost entry costs a recompile; the check on read rejects every torn state.
  static std::atomic<uint64_t> seq{0};
  const std::string tmp = opath(okey) + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(++seq);
  std::error_code ec;
  try {
    json meta{{"depfile_body", depfile_body}, {"stdout", out}, {"stderr", err}, {"rc", rc}, {"size", object.size()}, {"xxh", util::xxh64_hex(object)}};
    util::write_file(tmp + ".meta", meta.dump());
  } catch (...) { fs::remove(tmp + ".meta", ec); return false; }
  fs::rename(tmp + ".meta", opath(okey) + ".meta", ec); if (ec) { fs::remove(tmp + ".meta", ec); return false; }
  try { util::write_file(tmp, object); } catch (...) { fs::remove(tmp, ec); return false; }
  fs::rename(tmp, opath(okey), ec); if (ec) { fs::remove(tmp, ec); return false; }
  std::lock_guard<std::mutex> g(mu_);
  if (!objects_.count(okey)) { objects_[okey] = {object.size(), ++tick_}; total_ += object.size(); }
  ++stores; evict_locked();
  return true;
}

std::vector<std::string> ObjectCache::full_keys() const {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<std::string> v;
  for (auto& [k, m] : manifests_) for (auto& e : m.value("entries", json::array())) { std::string f = e.value("fkey", ""); if (!f.empty()) v.push_back(f); }
  return v;
}

void ObjectCache::add_manifest_entry(const std::string& mkey, const cachekey::Closure& used_rel, const std::string& okey, const std::string& shape,
                                     const std::string& fkey) {
  json closure = json::array(); for (auto& [p, s] : used_rel) closure.push_back(json::array({p, s}));
  std::lock_guard<std::mutex> g(mu_);
  json& m = manifests_[mkey]; if (!m.is_object()) m = json{{"entries", json::array()}};
  for (auto& e : m["entries"]) if (e.value("obj", "") == okey && e.value("shape", "") == shape) return;
  json e{{"closure", closure}, {"shape", shape}, {"obj", okey}};
  if (!fkey.empty()) e["fkey"] = fkey;
  m["entries"].push_back(std::move(e));
  if (m["entries"].size() > 32) m["entries"].erase(m["entries"].begin());   // bound manifest growth
  mnat_[mkey] = to_native(m);
  {   // replaced atomically, so a crash mid-write cannot leave a truncated manifest
    static std::atomic<uint64_t> mseq{0};
    const std::string tmp = mpath(mkey) + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(++mseq);
    std::error_code ec;
    try { util::write_file(tmp, m.dump()); fs::rename(tmp, mpath(mkey), ec); if (ec) fs::remove(tmp, ec); } catch (...) { fs::remove(tmp, ec); }
  }
}

void ObjectCache::evict_locked() {
  std::error_code ec;
  while (total_ > cap_ && objects_.size() > 1) {
    auto victim = objects_.begin();
    for (auto it = objects_.begin(); it != objects_.end(); ++it) if (it->second.last < victim->second.last) victim = it;
    fs::remove(opath(victim->first), ec); fs::remove(opath(victim->first) + ".meta", ec);
    total_ -= victim->second.size; objects_.erase(victim);
  }
}

}  // namespace opticc::objcache
