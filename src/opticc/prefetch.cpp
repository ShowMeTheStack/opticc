// SPDX-License-Identifier: GPL-2.0-only
#include "prefetch.hpp"

#include <algorithm>

namespace opticc::prefetch {

double TokenBucket::available(double now) {
  if (last_ < 0) last_ = now;
  if (now > last_) { tokens_ = std::min(burst_, tokens_ + (now - last_) * rate_); last_ = now; }
  return tokens_;
}

bool TokenBucket::take(double bytes, double now) {
  if (available(now) < bytes) return false;
  tokens_ -= bytes; return true;
}

void Tracker::resize(int nodes) {
  std::lock_guard<std::mutex> g(m_);
  held_.resize(static_cast<size_t>(std::max(nodes, 0) + 1)); pushed_.resize(held_.size(), 0); all_held_at_.resize(held_.size(), 0);
}

bool Tracker::held(int node, const std::string& sha) const {
  std::lock_guard<std::mutex> g(m_);
  return node >= 0 && static_cast<size_t>(node) < held_.size() && held_[static_cast<size_t>(node)].count(sha) > 0;
}

void Tracker::mark(int node, const std::string& sha, uint64_t bytes) {
  std::lock_guard<std::mutex> g(m_);
  if (node < 0 || static_cast<size_t>(node) >= held_.size()) return;
  if (held_[static_cast<size_t>(node)].insert(sha).second) pushed_[static_cast<size_t>(node)] += bytes;
}

void Tracker::forget_node(int node) {
  std::lock_guard<std::mutex> g(m_);
  if (node >= 0 && static_cast<size_t>(node) < held_.size()) { held_[static_cast<size_t>(node)].clear(); all_held_at_[static_cast<size_t>(node)] = 0; }
}

size_t Tracker::held_count(int node) const {
  std::lock_guard<std::mutex> g(m_);
  return node >= 0 && static_cast<size_t>(node) < held_.size() ? held_[static_cast<size_t>(node)].size() : 0;
}

uint64_t Tracker::pushed_bytes(int node) const {
  std::lock_guard<std::mutex> g(m_);
  return node >= 0 && static_cast<size_t>(node) < pushed_.size() ? pushed_[static_cast<size_t>(node)] : 0;
}

void Tracker::note(const std::string& sha, const std::string& path, uint64_t size, uint64_t weight) {
  std::lock_guard<std::mutex> g(m_);
  auto ins = files_.try_emplace(sha); if (ins.second) ++keys_gen_;   // a new file: every node's all-held memo is stale
  auto& f = ins.first->second; f.path = path; f.size = size; f.refs += weight;
}

std::vector<std::pair<std::string, FileEnt>> Tracker::pick(int node, uint64_t budget_bytes, size_t max_items) const {
  std::lock_guard<std::mutex> g(m_);
  std::vector<std::pair<std::string, FileEnt>> cand;
  if (node < 0 || static_cast<size_t>(node) >= held_.size()) return cand;
  if (all_held_at_[static_cast<size_t>(node)] == keys_gen_) return cand;   // nothing new since it held everything
  const auto& h = held_[static_cast<size_t>(node)];
  for (auto& [sha, f] : files_) if (!h.count(sha)) cand.emplace_back(sha, f);
  if (cand.empty()) { all_held_at_[static_cast<size_t>(node)] = keys_gen_; return cand; }
  // most referenced first; ties: smaller first (more files per byte), then sha for determinism
  std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
    if (a.second.refs != b.second.refs) return a.second.refs > b.second.refs;
    if (a.second.size != b.second.size) return a.second.size < b.second.size;
    return a.first < b.first;
  });
  std::vector<std::pair<std::string, FileEnt>> out; uint64_t used = 0;
  for (auto& c : cand) {
    if (out.size() >= max_items) break;
    if (used + c.second.size > budget_bytes) continue;   // skip files that do not fit; smaller ones may
    used += c.second.size; out.push_back(c);
  }
  return out;
}

std::vector<std::pair<std::string, FileEnt>> Tracker::top(size_t n) const {
  std::lock_guard<std::mutex> g(m_);
  std::vector<std::pair<std::string, FileEnt>> v(files_.begin(), files_.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.refs != b.second.refs ? a.second.refs > b.second.refs : a.second.path < b.second.path; });
  if (v.size() > n) v.resize(n);
  return v;
}

std::vector<std::string> Tracker::top_paths(size_t n) const {
  std::lock_guard<std::mutex> g(m_);
  std::vector<const FileEnt*> v; v.reserve(files_.size());
  for (auto& [sha, f] : files_) v.push_back(&f);
  std::sort(v.begin(), v.end(), [](const FileEnt* a, const FileEnt* b) { return a->refs != b->refs ? a->refs > b->refs : a->path < b->path; });
  std::vector<std::string> out;
  for (size_t i = 0; i < v.size() && i < n; ++i) out.push_back(v[i]->path);
  return out;
}

}  // namespace opticc::prefetch
