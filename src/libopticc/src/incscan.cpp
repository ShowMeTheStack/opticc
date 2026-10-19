// SPDX-License-Identifier: GPL-2.0-only
#include <chrono>
#include <cstring>
#include <shared_mutex>
#include "opticc/incscan.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>

#include "opticc/closure.hpp"
#include "opticc/direct.hpp"
#include "opticc/util.hpp"

namespace opticc::incscan {
namespace fs = std::filesystem;

FileScan parse_text(const std::string& text) {
  FileScan fsn;
  size_t i = 0, n = text.size();
  bool in_block = false;
  std::string line, clean;
  while (i < n) {
    line.clear();
    while (i < n) {   // one logical line (backslash continuations joined)
      char c = text[i];
      if (c == '\\' && i + 1 < n && text[i + 1] == '\n') { i += 2; continue; }
      if (c == '\\' && i + 2 < n && text[i + 1] == '\r' && text[i + 2] == '\n') { i += 3; continue; }
      if (c == '\n') { ++i; break; }
      line.push_back(c); ++i;
    }
    // cheap pre-filter: a directive line must contain '#'; comment state still has to be tracked across lines
    if (!in_block && line.find('#') == std::string::npos) {
      if (line.find("/*") != std::string::npos) {   // may open a block comment
        for (size_t k = 0; k < line.size(); ++k) {
          if (in_block) { if (line[k] == '*' && k + 1 < line.size() && line[k + 1] == '/') { in_block = false; ++k; } continue; }
          if (line[k] == '/' && k + 1 < line.size() && line[k + 1] == '*') { in_block = true; ++k; continue; }
          if (line[k] == '/' && k + 1 < line.size() && line[k + 1] == '/') break;
        }
      }
      continue;
    }
    clean.clear();
    for (size_t k = 0; k < line.size(); ++k) {   // strip block comments (state carried across lines) and line comments
      if (in_block) { if (line[k] == '*' && k + 1 < line.size() && line[k + 1] == '/') { in_block = false; ++k; } continue; }
      if (line[k] == '/' && k + 1 < line.size() && line[k + 1] == '*') { in_block = true; ++k; continue; }
      if (line[k] == '/' && k + 1 < line.size() && line[k + 1] == '/') break;
      clean.push_back(line[k]);
    }
    size_t p = clean.find_first_not_of(" \t\r");
    if (p == std::string::npos || clean[p] != '#') continue;
    p = clean.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) continue;
    // __has_include / __has_include_next in #if / #elif conditions
    if ((clean.compare(p, 2, "if") == 0 || clean.compare(p, 4, "elif") == 0) && clean.find("__has_include", p) != std::string::npos) {
      size_t q = p;
      while ((q = clean.find("__has_include", q)) != std::string::npos) {
        q += 13; bool hnext = clean.compare(q, 5, "_next") == 0; if (hnext) q += 5;
        size_t a = clean.find_first_not_of(" \t", q);
        if (a == std::string::npos || clean[a] != '(') continue;       // e.g. defined(__has_include)
        a = clean.find_first_not_of(" \t", a + 1);
        if (a == std::string::npos) { fsn.has_include_computed = true; break; }
        char o = clean[a]; char cl = o == '"' ? '"' : o == '<' ? '>' : 0;
        if (!cl) { fsn.has_include_computed = true; continue; }        // __has_include(MACRO)
        size_t e2 = clean.find(cl, a + 1);
        if (e2 == std::string::npos) { fsn.has_include_computed = true; continue; }
        Directive hd; hd.angled = o == '<'; hd.next = hnext; hd.name = clean.substr(a + 1, e2 - a - 1);
        if (!hd.name.empty()) fsn.incs.push_back(std::move(hd));
        q = e2;
      }
      continue;
    }
    bool next = false;
    if (clean.compare(p, 12, "include_next") == 0) { next = true; p += 12; }
    else if (clean.compare(p, 7, "include") == 0 && !(p + 7 < clean.size() && (std::isalnum(static_cast<unsigned char>(clean[p + 7])) || clean[p + 7] == '_'))) p += 7;
    else if (clean.compare(p, 6, "import") == 0) p += 6;
    else continue;
    p = clean.find_first_not_of(" \t", p);
    if (p == std::string::npos) { fsn.computed = true; continue; }
    char open = clean[p]; char close = open == '"' ? '"' : open == '<' ? '>' : 0;
    if (!close) { fsn.computed = true; continue; }          // #include MACRO
    size_t e = clean.find(close, p + 1);
    if (e == std::string::npos) { fsn.computed = true; continue; }
    Directive d; d.angled = open == '<'; d.next = next; d.name = clean.substr(p + 1, e - p - 1);
    if (!d.name.empty()) fsn.incs.push_back(std::move(d));
  }
  return fsn;
}

static bool stat_file(const std::string& abs, int64_t* mt, uint64_t* sz, int64_t* ct = nullptr, uint64_t* ino = nullptr) {
  struct stat st{};
  if (::stat(abs.c_str(), &st) != 0) return false;
  if (!S_ISREG(st.st_mode)) return false;
#ifdef __APPLE__
  *mt = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
  if (ct) *ct = static_cast<int64_t>(st.st_ctimespec.tv_sec) * 1000000000LL + st.st_ctimespec.tv_nsec;
#else
  *mt = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
  if (ct) *ct = static_cast<int64_t>(st.st_ctim.tv_sec) * 1000000000LL + st.st_ctim.tv_nsec;
#endif
  *sz = static_cast<uint64_t>(st.st_size);
  if (ino) *ino = static_cast<uint64_t>(st.st_ino);
  return true;
}

int Scanner::file_id_locked(const std::string& abs) {
  auto it = id_of_.find(abs);
  if (it != id_of_.end()) return it->second;
  int id = static_cast<int>(files_.size());
  files_.push_back(File{abs, {}, 0, false}); is_system_.push_back(0); id_of_.emplace(abs, id);
  return id;
}

void Scanner::ensure_parsed(int id) {
  std::string path; int64_t old_mt = 0, old_ct = 0; uint64_t old_sz = 0, old_ino = 0; bool parsed = false;
  {
    std::lock_guard<std::shared_mutex> g(mu_);
    File& f = files_[static_cast<size_t>(id)];
    if (f.parsed && fresh_locked(f.validated_at, util::now_s())) return;
    path = f.path; old_mt = f.scan.mtime_ns; old_sz = f.scan.size; old_ct = f.scan.ctime_ns; old_ino = f.scan.ino; parsed = f.parsed;
  }
  int64_t mt = 0, ct = 0; uint64_t sz = 0, ino = 0;
  bool ok = stat_file(path, &mt, &sz, &ct, &ino);
  { std::lock_guard<std::shared_mutex> g(mu_); st_.stats++; }
  if (parsed && ok && mt == old_mt && sz == old_sz && ct == old_ct && ino == old_ino) {   // unchanged: just refresh the validation stamp
    std::lock_guard<std::shared_mutex> g(mu_); files_[static_cast<size_t>(id)].validated_at = util::now_s(); return;
  }
  FileScan fsn; bool from_state = false;
  if (ok && !parsed) {   // a persisted parse with the identical (mtime_ns, size) replaces read + parse
    std::lock_guard<std::shared_mutex> g(mu_);
    auto it = preloaded_.find(path);
    if (it != preloaded_.end() && it->second.mtime_ns == mt && it->second.size == sz && it->second.ctime_ns == ct && it->second.ino == ino) { fsn = it->second; from_state = true; }
  }
  if (ok && !from_state) {
    try { fsn = parse_text(util::read_file(path)); } catch (...) {}
    fsn.parsed_wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  }
  fsn.mtime_ns = mt; fsn.size = sz; fsn.ctime_ns = ct; fsn.ino = ino;
  std::lock_guard<std::shared_mutex> g(mu_);
  File& f = files_[static_cast<size_t>(id)];
  f.scan = std::move(fsn); f.validated_at = util::now_s(); f.parsed = true;
  if (from_state) st_.state_hits++; else st_.parses++;
  if (parsed) {   // the file changed: its memoised adjacency is stale in every configuration
    for (size_t c = 0; c < cfgs_.size(); ++c) { adj_.erase((static_cast<uint64_t>(c) << 32) | static_cast<uint64_t>(id)); adjx_.erase((static_cast<uint64_t>(c) << 32) | static_cast<uint64_t>(id)); }   // (cand_adj_ entries for
    // erased keys are left behind: invalidating a missing adjacency is a no-op)
  }
}

std::string Scanner::res_key(int cfg, const Directive& d, const std::string& includer_dir) {
  const bool quoted = !d.angled && !d.next;
  std::string key; key.reserve(includer_dir.size() + d.name.size() + 8);
  key += static_cast<char>('0' + cfg % 10); key += static_cast<char>('0' + cfg / 10); key += d.angled ? '<' : (d.next ? 'n' : '"');
  if (quoted) key += includer_dir;
  key += '\x01'; key += d.name;
  return key;
}

int Scanner::resolve(int cfg, const Directive& d, const std::string& includer_dir) {
  const bool quoted = !d.angled && !d.next;
  const std::string key = res_key(cfg, d, includer_dir);
  {
    std::lock_guard<std::shared_mutex> g(mu_);
    auto it = res_.find(key);
    if (it != res_.end()) {
      // a negative answer expires (kbuild generates headers such as include/generated/asm-offsets.h after the
      // first TUs were scanned); positive answers stay until the file itself changes (validated in ensure_parsed)
      if ((it->second >= 0 || it->second == -2) && res_epoch_ok_locked(key)) { st_.resolve_hits++; return it->second; }
      auto nt = neg_at_.find(key);
      if (nt != neg_at_.end() && fresh_locked(nt->second, util::now_s())) { st_.resolve_hits++; return it->second; }
    }
  }
  Config c; { std::lock_guard<std::shared_mutex> g(mu_); c = cfgs_[static_cast<size_t>(cfg)]; }
  auto in_system = [&](const std::string& abs) { for (auto& sd : c.system_dirs) if (util::path_within(abs, sd)) return true; return false; };
  std::string found;
  std::vector<std::string> absent;   // candidates that do not exist (a later creation would shadow `found`)
  if (!d.name.empty() && d.name[0] == '/') { int64_t mt; uint64_t sz; if (stat_file(d.name, &mt, &sz)) found = d.name; else absent.push_back(d.name); }
  else {
    std::vector<const std::string*> search;
    if (quoted) { search.push_back(&includer_dir); for (auto& q : c.quote_extra) search.push_back(&q); }
    for (auto& b : c.bracket_search) search.push_back(&b);
    std::string cand;
    for (const std::string* dir : search) {
      cand = (fs::path(*dir) / d.name).lexically_normal().string();
      int64_t mt; uint64_t sz;
      { std::lock_guard<std::shared_mutex> g(mu_); st_.resolve_probes++; }
      if (stat_file(cand, &mt, &sz)) {
        if (found.empty()) found = cand;
        if (!d.next) break;   // #include_next starts after the includer's directory, which the scanner does not track:
      } else {                // probe every directory so that every possible shadowing candidate is recorded
        absent.push_back(cand);
      }
    }
  }
  int result = -1;
  std::lock_guard<std::shared_mutex> g(mu_);
  if (!found.empty()) {
    if (in_system(found)) result = -2;
    else { result = file_id_locked(found); }
  }
  res_[key] = result; res_gen_[key] = res_gen_cur_;
  absent_[key] = std::move(absent);
  if (result == -2) sys_found_[key] = found; else sys_found_.erase(key);
  if (result == -1) neg_at_[key] = util::now_s(); else neg_at_.erase(key);
  return result;
}

std::vector<int> Scanner::adjacency(int cfg, int id) { return adjacency_full(cfg, id).ids; }

int Scanner::intern_dir_locked(const std::string& d) {
  auto it = dir_id_.find(d);
  if (it != dir_id_.end()) return it->second;
  int k = static_cast<int>(dirs_.size()); dirs_.emplace_back(); dirs_.back().path = d; dir_id_.emplace(d, k); return k;
}
int Scanner::intern_name_locked(const std::string& n) {
  auto it = name_id_.find(n);
  if (it != name_id_.end()) return it->second;
  int k = static_cast<int>(names_.size()); names_.push_back(n); name_id_.emplace(n, k); return k;
}

void Scanner::absent_pairs_locked(const std::string& key, std::vector<std::pair<int, int>>* out) {
  auto a = absent_.find(key);
  if (a == absent_.end()) return;
  std::lock_guard<std::mutex> g(dmu_);
  for (auto& c : a->second) {
    auto sl = c.rfind('/');
    if (sl == std::string::npos || sl == 0) continue;
    out->emplace_back(intern_dir_locked(c.substr(0, sl)), intern_name_locked(c.substr(sl + 1)));
  }
}

Scanner::Adj Scanner::adjacency_full(int cfg, int id) {
  const uint64_t key = (static_cast<uint64_t>(cfg) << 32) | static_cast<uint64_t>(id);
  {
    std::lock_guard<std::shared_mutex> g(mu_);
    auto it = adj_.find(key);
    if (it != adj_.end() && files_[static_cast<size_t>(id)].parsed && fresh_locked(files_[static_cast<size_t>(id)].validated_at, util::now_s())) { st_.adj_hits++; return it->second; }
  }
  ensure_parsed(id);   // may invalidate adj_ for this id if the file changed
  {
    std::lock_guard<std::shared_mutex> g(mu_);
    auto it = adj_.find(key);
    if (it != adj_.end()) { st_.adj_hits++; return it->second; }
  }
  std::vector<Directive> incs; std::string dir;
  { std::lock_guard<std::shared_mutex> g(mu_); const File& f = files_[static_cast<size_t>(id)]; incs = f.scan.incs; dir = util::dirname(f.path); }
  Adj out; out.ids.reserve(incs.size());
  for (auto& d : incs) { int r = resolve(cfg, d, dir); if (r >= 0) out.ids.push_back(r); }
  std::lock_guard<std::shared_mutex> g(mu_);
  for (auto& d : incs) absent_pairs_locked(res_key(cfg, d, dir), &out.neg);
  std::sort(out.neg.begin(), out.neg.end()); out.neg.erase(std::unique(out.neg.begin(), out.neg.end()), out.neg.end());
  for (auto& pr : out.neg) cand_adj_[(static_cast<uint64_t>(pr.first) << 32) | static_cast<uint64_t>(pr.second)].push_back(key);
  adj_[key] = out;
  return out;
}

bool Scanner::verify_absent(std::vector<std::pair<int, int>>& neg, std::pair<int, int>* found) {
  std::sort(neg.begin(), neg.end()); neg.erase(std::unique(neg.begin(), neg.end()), neg.end());
  std::vector<int> todo; std::vector<std::string> todo_names;
  for (size_t i = 0; i < neg.size();) {
    size_t j = i; const int did = neg[i].first;
    while (j < neg.size() && neg[j].first == did) ++j;
    std::string dpath; { std::lock_guard<std::mutex> g(dmu_); dpath = dirs_[static_cast<size_t>(did)].path; }
    const direct::FStat now = direct::stat_path(dpath);   // BEFORE any name is probed
    bool vol = false; todo.clear(); todo_names.clear();
    {
      std::lock_guard<std::mutex> g(dmu_);
      DirSnap& sn = dirs_[static_cast<size_t>(did)];
      const bool same = sn.valid && sn.exists == now.exists && sn.dev == now.dev && sn.ino == now.ino && sn.mtime_ns == now.mtime_ns && sn.ctime_ns == now.ctime_ns;
      if (!same || sn.vol) {
        const double wall = direct::wall_now();
        sn.valid = true; sn.exists = now.exists; sn.dev = now.dev; sn.ino = now.ino; sn.mtime_ns = now.mtime_ns; sn.ctime_ns = now.ctime_ns;
        sn.vol = now.exists && now.ctime_ns >= static_cast<int64_t>((wall - direct::kRacyS) * 1e9);
        sn.ok.clear();
      }
      vol = sn.vol;
      if (now.exists)
        for (size_t k = i; k < j; ++k) if (vol || !sn.ok.count(neg[k].second)) { todo.push_back(neg[k].second); todo_names.push_back(names_[static_cast<size_t>(neg[k].second)]); }
    }
    for (size_t k = 0; k < todo.size(); ++k) {   // (a missing directory holds no names: nothing to probe)
      if (::access((dpath + "/" + todo_names[k]).c_str(), F_OK) == 0) { if (found) *found = {did, todo[k]}; return false; }
    }
    if (!vol && !todo.empty()) {
      std::lock_guard<std::mutex> g(dmu_);
      DirSnap& sn = dirs_[static_cast<size_t>(did)];
      if (sn.valid && sn.exists == now.exists && sn.ino == now.ino && sn.mtime_ns == now.mtime_ns && sn.ctime_ns == now.ctime_ns)
        for (int n : todo) sn.ok.insert(n);
    }
    i = j;
  }
  return true;
}

void Scanner::invalidate_candidate(const std::pair<int, int>& cand) {
  std::lock_guard<std::shared_mutex> g(mu_);
  auto it = cand_adj_.find((static_cast<uint64_t>(cand.first) << 32) | static_cast<uint64_t>(cand.second));
  if (it == cand_adj_.end()) { res_gen_cur_++; neg_at_.clear(); adj_.clear(); adjx_.clear(); cand_adj_.clear(); return; }   // forced include etc.: everything
  std::vector<uint64_t> keys; keys.swap(it->second); cand_adj_.erase(it);
  for (uint64_t k : keys) {
    const int cfg = static_cast<int>(k >> 32), id = static_cast<int>(k & 0xffffffffu);
    adj_.erase(k); adjx_.erase(k);
    if (static_cast<size_t>(id) >= files_.size()) continue;
    const File& f = files_[static_cast<size_t>(id)];
    const std::string dir = util::dirname(f.path);
    for (auto& d : f.scan.incs) { std::string rk = res_key(cfg, d, dir); res_.erase(rk); absent_.erase(rk); neg_at_.erase(rk); }
  }
}

// search configuration → small id (kbuild uses one configuration per directory; a build has a handful)
int Scanner::config_id(const args::IncludeDirs& dirs, const std::vector<std::string>& system_dirs, const std::string& cwd) {
  Config c;
  for (auto& d : dirs.quote) c.quote_extra.push_back(closure::absolutize(d, cwd));
  for (auto& d : dirs.bracket) c.bracket_search.push_back(closure::absolutize(d, cwd));
  for (auto& d : dirs.system) c.bracket_search.push_back(closure::absolutize(d, cwd));
  for (auto& d : system_dirs) c.bracket_search.push_back(d);
  for (auto& d : dirs.after) c.bracket_search.push_back(closure::absolutize(d, cwd));
  c.system_dirs = system_dirs;
  std::string ckey;
  for (auto& s : c.quote_extra) { ckey += s; ckey += '\x01'; }
  ckey += '\x02'; for (auto& s : c.bracket_search) { ckey += s; ckey += '\x01'; }
  ckey += '\x02'; for (auto& s : c.system_dirs) { ckey += s; ckey += '\x01'; }
  std::lock_guard<std::shared_mutex> g(mu_);
  auto it = cfg_ids_.find(ckey);
  if (it != cfg_ids_.end()) return it->second;
  int cfg = static_cast<int>(cfgs_.size()); cfgs_.push_back(std::move(c)); cfg_ids_.emplace(ckey, cfg);
  return cfg;
}

Scanner::Adj Scanner::adjacency_all(int cfg, int id) {
  const uint64_t key = (static_cast<uint64_t>(cfg) << 32) | static_cast<uint64_t>(id);
  {
    std::lock_guard<std::shared_mutex> g(mu_);
    auto it = adjx_.find(key);
    if (it != adjx_.end() && files_[static_cast<size_t>(id)].parsed && fresh_locked(files_[static_cast<size_t>(id)].validated_at, util::now_s())) return it->second;
  }
  ensure_parsed(id);   // may invalidate adj_/adjx_ for this id if the file changed
  {
    std::lock_guard<std::shared_mutex> g(mu_);
    auto it = adjx_.find(key);
    if (it != adjx_.end()) return it->second;
  }
  std::vector<Directive> incs; std::string dir;
  { std::lock_guard<std::shared_mutex> g(mu_); const File& f = files_[static_cast<size_t>(id)]; incs = f.scan.incs; dir = util::dirname(f.path); }
  std::vector<int> rs; rs.reserve(incs.size());
  for (auto& d : incs) rs.push_back(resolve(cfg, d, dir));
  Adj out;
  std::lock_guard<std::shared_mutex> g(mu_);
  for (size_t k = 0; k < incs.size(); ++k) {
    const std::string rk = res_key(cfg, incs[k], dir);
    if (rs[k] >= 0) out.ids.push_back(rs[k]);
    else if (rs[k] == -2) {
      auto f = sys_found_.find(rk);
      if (f != sys_found_.end()) { int sid = file_id_locked(f->second); is_system_[static_cast<size_t>(sid)] = 1; out.ids.push_back(sid); }
    }
    absent_pairs_locked(rk, &out.neg);
  }
  std::sort(out.neg.begin(), out.neg.end()); out.neg.erase(std::unique(out.neg.begin(), out.neg.end()), out.neg.end());
  for (auto& pr : out.neg) cand_adj_[(static_cast<uint64_t>(pr.first) << 32) | static_cast<uint64_t>(pr.second)].push_back(key);
  adjx_[key] = out;
  return out;
}

void Scanner::direct_inputs(const std::string& src_abs, const std::vector<std::string>& closure_files,
                            const args::IncludeDirs& dirs, const std::vector<std::string>& system_dirs, const std::string& cwd,
                            std::vector<std::string>* extra, std::vector<std::pair<int, int>>* neg, std::vector<std::string>* computed,
                            bool* has_include_computed) {
  const int cfg = config_id(dirs, system_dirs, cwd);
  std::vector<int> stack; std::vector<char> seen;
  auto mark = [&](int id) { if (static_cast<size_t>(id) >= seen.size()) seen.resize(static_cast<size_t>(id) + 1024, 0); if (seen[static_cast<size_t>(id)]) return false; seen[static_cast<size_t>(id)] = 1; return true; };
  {
    std::lock_guard<std::shared_mutex> g(mu_);
    seen.resize(files_.size() + closure_files.size() + 1024, 0);
    int sid = file_id_locked(src_abs); mark(sid); stack.push_back(sid);
    for (auto& p : closure_files) { int id = file_id_locked(p); if (mark(id)) stack.push_back(id); }
  }
  std::vector<int> found_extra;
  for (auto& f : dirs.forced) {
    Directive d; d.name = f; const std::string idir = closure::absolutize(".", cwd);
    int r = resolve(cfg, d, idir);
    std::lock_guard<std::shared_mutex> g(mu_);
    const std::string rk = res_key(cfg, d, idir);
    absent_pairs_locked(rk, neg);
    if (r == -2) { auto it = sys_found_.find(rk); r = it == sys_found_.end() ? -1 : file_id_locked(it->second); }
    if (r >= 0 && mark(r)) { stack.push_back(r); found_extra.push_back(r); }
  }
  bool hic = false;
  std::vector<int> comp_ids;
  std::shared_lock<std::shared_mutex> lk(mu_);
  const double tnow = util::now_s();
  while (!stack.empty()) {
    const int cur = stack.back(); stack.pop_back();
    const uint64_t akey = (static_cast<uint64_t>(cfg) << 32) | static_cast<uint64_t>(cur);
    auto it = adjx_.find(akey);
    const File& f = files_[static_cast<size_t>(cur)];
    if (it != adjx_.end() && f.parsed && fresh_locked(f.validated_at, tnow)) {
      if (f.scan.computed) comp_ids.push_back(cur);
      if (f.scan.has_include_computed) hic = true;
      for (int r : it->second.ids) if (mark(r)) { stack.push_back(r); found_extra.push_back(r); }
      neg->insert(neg->end(), it->second.neg.begin(), it->second.neg.end());
      continue;
    }
    lk.unlock();
    const Adj a = adjacency_all(cfg, cur);
    lk.lock();
    if (files_[static_cast<size_t>(cur)].scan.computed) comp_ids.push_back(cur);
    if (files_[static_cast<size_t>(cur)].scan.has_include_computed) hic = true;
    for (int r : a.ids) if (mark(r)) { stack.push_back(r); found_extra.push_back(r); }
    neg->insert(neg->end(), a.neg.begin(), a.neg.end());
  }
  for (int id : found_extra) extra->push_back(files_[static_cast<size_t>(id)].path);
  for (int id : comp_ids) computed->push_back(files_[static_cast<size_t>(id)].path);
  lk.unlock();
  std::sort(neg->begin(), neg->end()); neg->erase(std::unique(neg->begin(), neg->end()), neg->end());
  if (has_include_computed) *has_include_computed = hic;
}

bool Scanner::parse_matches(const std::string& path, const direct::FStat& st) const {
  std::shared_lock<std::shared_mutex> g(mu_);
  auto it = id_of_.find(path);
  if (it == id_of_.end()) return false;
  const File& f = files_[static_cast<size_t>(it->second)];
  return f.parsed && st.exists && f.scan.mtime_ns == st.mtime_ns && f.scan.size == st.size && f.scan.ctime_ns == st.ctime_ns && f.scan.ino == st.ino;
}

long Scanner::first_stale_parse(const std::vector<std::string>& paths, const std::vector<direct::FStat>& st, const std::vector<char>& check) const {
  std::shared_lock<std::shared_mutex> g(mu_);
  for (size_t i = 0; i < paths.size(); ++i) {
    if (!check[i]) continue;
    auto it = id_of_.find(paths[i]);
    if (it == id_of_.end()) return static_cast<long>(i);
    const File& f = files_[static_cast<size_t>(it->second)];
    const direct::FStat& s = st[i];
    if (!(f.parsed && s.exists && f.scan.mtime_ns == s.mtime_ns && f.scan.size == s.size && f.scan.ctime_ns == s.ctime_ns && f.scan.ino == s.ino)) return static_cast<long>(i);
  }
  return -1;
}

void Scanner::intern_candidates(const std::vector<std::string>& paths, std::vector<std::pair<int, int>>* out) {
  std::lock_guard<std::mutex> g(dmu_);
  for (auto& c : paths) {
    auto sl = c.rfind('/');
    if (sl == std::string::npos || sl == 0) continue;
    out->emplace_back(intern_dir_locked(c.substr(0, sl)), intern_name_locked(c.substr(sl + 1)));
  }
}

bool Scanner::negative_groups(std::vector<std::pair<int, int>>& neg, double t_start_wall, std::vector<direct::NegGroup>* out) {
  std::sort(neg.begin(), neg.end()); neg.erase(std::unique(neg.begin(), neg.end()), neg.end());
  const int64_t racy_ns = static_cast<int64_t>((t_start_wall - direct::kRacyS) * 1e9);
  std::vector<int> todo; std::vector<std::string> todo_names, all_names;
  for (size_t i = 0; i < neg.size();) {
    size_t j = i; const int did = neg[i].first;
    while (j < neg.size() && neg[j].first == did) ++j;
    std::string dpath; { std::lock_guard<std::mutex> g(dmu_); dpath = dirs_[static_cast<size_t>(did)].path; }
    const direct::FStat now = direct::stat_path(dpath);   // BEFORE any name is probed
    bool vol = false; todo.clear(); todo_names.clear(); all_names.clear();
    {
      std::lock_guard<std::mutex> g(dmu_);
      DirSnap& sn = dirs_[static_cast<size_t>(did)];
      const bool same = sn.valid && sn.exists == now.exists && sn.dev == now.dev && sn.ino == now.ino && sn.mtime_ns == now.mtime_ns && sn.ctime_ns == now.ctime_ns;
      if (!same || sn.vol) {
        const double wall = direct::wall_now();
        sn.valid = true; sn.exists = now.exists; sn.dev = now.dev; sn.ino = now.ino; sn.mtime_ns = now.mtime_ns; sn.ctime_ns = now.ctime_ns;
        sn.vol = now.exists && now.ctime_ns >= static_cast<int64_t>((wall - direct::kRacyS) * 1e9);
        sn.ok.clear();
      }
      vol = sn.vol;
      for (size_t k = i; k < j; ++k) {
        const std::string& nm = names_[static_cast<size_t>(neg[k].second)];
        all_names.push_back(nm);
        if (now.exists && (vol || !sn.ok.count(neg[k].second))) { todo.push_back(neg[k].second); todo_names.push_back(nm); }
      }
    }
    for (size_t k = 0; k < todo.size(); ++k)
      if (::access((dpath + "/" + todo_names[k]).c_str(), F_OK) == 0) return false;
    if (!vol && !todo.empty()) {
      std::lock_guard<std::mutex> g(dmu_);
      DirSnap& sn = dirs_[static_cast<size_t>(did)];
      if (sn.valid && sn.exists == now.exists && sn.ino == now.ino && sn.mtime_ns == now.mtime_ns && sn.ctime_ns == now.ctime_ns)
        for (int n : todo) sn.ok.insert(n);
    }
    direct::NegGroup grp; grp.dir = dpath; grp.st = now; grp.names = std::move(all_names);
    grp.volatile_dir = now.exists && now.ctime_ns >= racy_ns;
    out->push_back(std::move(grp));
    i = j;
  }
  return true;
}

// The closure is EXACT with respect to shadowing. A memoised resolution alone would stay valid until the including
// file changed, missing a header created earlier in the search path (between two builds less than a second apart, or
// during a build): the client cache could then answer with, and header-push could ship, the shadowed header. So every
// closure verifies that each candidate probed absent for it is still absent (verify_absent: about one stat() per
// directory); a candidate that exists now drops the memos that relied on its absence and the closure is recomputed.
std::vector<std::string> Scanner::closure(const std::string& src_abs, const args::IncludeDirs& dirs,
                                          const std::vector<std::string>& system_dirs, const std::string& cwd,
                                          bool* computed, int* files_scanned, bool* has_include_computed) {
  const int cfg = config_id(dirs, system_dirs, cwd);
  for (int round = 0;; ++round) {
    std::vector<std::pair<int, int>> neg; std::pair<int, int> found{-1, -1};
    auto out = closure_once(cfg, src_abs, dirs, cwd, computed, files_scanned, has_include_computed, &neg);
    const auto tv0 = std::chrono::steady_clock::now();
    const bool ok = verify_absent(neg, &found);
    verify_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tv0).count()); verify_calls++;
    if (ok) return out;
    { std::lock_guard<std::shared_mutex> g(mu_); st_.shadow_invalidations++; }
    if (round >= 3) { std::lock_guard<std::shared_mutex> g(mu_); res_gen_cur_++; neg_at_.clear(); adj_.clear(); adjx_.clear(); cand_adj_.clear(); }
    else invalidate_candidate(found);
    if (round >= 4) return out;   // unreachable in practice: after a full reset every resolution is probed afresh
  }
}

std::vector<std::string> Scanner::closure_once(int cfg, const std::string& src_abs, const args::IncludeDirs& dirs, const std::string& cwd,
                                               bool* computed, int* files_scanned, bool* has_include_computed,
                                               std::vector<std::pair<int, int>>* neg) {
  int src_id; size_t nfiles = 0;
  {
    std::lock_guard<std::shared_mutex> g(mu_);
    src_id = file_id_locked(src_abs);
    nfiles = files_.size();
  }
  std::vector<int> order; std::vector<char> seen; seen.resize(nfiles + 16, 0);
  auto mark = [&](int id) { if (static_cast<size_t>(id) >= seen.size()) seen.resize(static_cast<size_t>(id) + 1024, 0); if (seen[static_cast<size_t>(id)]) return false; seen[static_cast<size_t>(id)] = 1; return true; };
  std::vector<int> stack; mark(src_id); stack.push_back(src_id);
  for (auto& f : dirs.forced) {   // -include/-imacros: resolved like quoted includes from cwd
    Directive d; d.name = f; const std::string idir = closure::absolutize(".", cwd); int r = resolve(cfg, d, idir);
    { std::lock_guard<std::shared_mutex> g(mu_); absent_pairs_locked(res_key(cfg, d, idir), neg); }
    if (r >= 0 && mark(r)) { order.push_back(r); stack.push_back(r); }
  }
  bool comp = false, hic = false; int scanned = 0;
  // Warm path: one lock acquisition for the whole DFS; the lock is dropped only for files whose adjacency is not
  // memoised or whose validation expired (parse / stat / resolve happen outside the lock); per-node
  // locking would cause a lock convoy under concurrent TUs (see tests/perf). The lock is SHARED: the warm DFS only
  // reads the memo, so concurrent TUs do not serialise on the scanner; parsing/resolution (adjacency) still take the lock exclusively, outside this one.
  std::shared_lock<std::shared_mutex> lk(mu_);
  const double tnow = util::now_s();
  while (!stack.empty()) {
    int cur = stack.back(); stack.pop_back();
    ++scanned;
    const uint64_t akey = (static_cast<uint64_t>(cfg) << 32) | static_cast<uint64_t>(cur);
    const File& f = files_[static_cast<size_t>(cur)];
    auto it = adj_.find(akey);
    if (it != adj_.end() && f.parsed && fresh_locked(f.validated_at, tnow)) {
      adj_hits_.fetch_add(1, std::memory_order_relaxed);
      if (f.scan.computed) comp = true;
      if (f.scan.has_include_computed) hic = true;
      for (int r : it->second.ids) if (mark(r)) { order.push_back(r); stack.push_back(r); }
      neg->insert(neg->end(), it->second.neg.begin(), it->second.neg.end());
      continue;
    }
    lk.unlock();
    const Adj adj = adjacency_full(cfg, cur);
    lk.lock();
    if (files_[static_cast<size_t>(cur)].scan.computed) comp = true;
    if (files_[static_cast<size_t>(cur)].scan.has_include_computed) hic = true;
    for (int r : adj.ids) if (mark(r)) { order.push_back(r); stack.push_back(r); }
    neg->insert(neg->end(), adj.neg.begin(), adj.neg.end());
  }
  lk.unlock();
  std::vector<std::string> out; out.reserve(order.size());
  { std::shared_lock<std::shared_mutex> g(mu_); for (int id : order) out.push_back(files_[static_cast<size_t>(id)].path); }
  if (computed) *computed = comp;
  if (has_include_computed) *has_include_computed = hic;
  if (files_scanned) *files_scanned = scanned;
  return out;
}


// Build-epoch boundary (called by the coordinator when a request arrives with no job in flight, i.e. between builds
// or between the steps of a sequential build). Every cached file is re-validated by stat() before its next use, so an
// edit made between two builds is always seen, however quickly the second build starts (the e2e "edit a shared header
// and rebuild" case). If the boundary follows an idle gap of more than 1 s, include resolutions are re-probed too,
// so a newly created header that shadows an earlier match in the search path is also picked up.
void Scanner::new_epoch(double idle_gap_s) {
  std::lock_guard<std::shared_mutex> g(mu_);
  epoch_ = util::now_s();
  if (idle_gap_s > 1.0) res_gen_cur_++;
}
bool Scanner::fresh_locked(double validated_at, double tnow) const { return validated_at >= epoch_ && tnow - validated_at < kValidateTtl; }
bool Scanner::res_epoch_ok_locked(const std::string& key) const { auto it = res_gen_.find(key); return it != res_gen_.end() && it->second == res_gen_cur_; }

std::string Scanner::export_state() const {
  std::string out = "SCN4";
  auto put = [&](const void* p, size_t n) { out.append(static_cast<const char*>(p), n); };
  std::lock_guard<std::shared_mutex> g(mu_);
  for (auto& f : files_) {
    if (!f.parsed || f.scan.parsed_wall <= 0) continue;
    if (f.scan.parsed_wall - static_cast<double>(std::max(f.scan.mtime_ns, f.scan.ctime_ns)) / 1e9 < 2.0) continue;   // racy: never persisted
    uint32_t pl = static_cast<uint32_t>(f.path.size()); put(&pl, 4); out += f.path;
    put(&f.scan.mtime_ns, 8); put(&f.scan.size, 8); put(&f.scan.ctime_ns, 8); put(&f.scan.ino, 8); put(&f.scan.parsed_wall, 8);
    uint8_t fl = (f.scan.computed ? 1 : 0) | (f.scan.has_include_computed ? 2 : 0); put(&fl, 1);
    uint32_t n = static_cast<uint32_t>(f.scan.incs.size()); put(&n, 4);
    for (auto& d : f.scan.incs) {
      uint8_t k = (d.angled ? 1 : 0) | (d.next ? 2 : 0); put(&k, 1);
      uint16_t nl = static_cast<uint16_t>(std::min<size_t>(d.name.size(), 65535)); put(&nl, 2); out.append(d.name, 0, nl);
    }
  }
  for (auto& [path, fs] : preloaded_) {   // keep still-unused persisted records (files not visited this run)
    bool seen = id_of_.count(path) && files_[static_cast<size_t>(id_of_.at(path))].parsed;
    if (seen) continue;
    uint32_t pl = static_cast<uint32_t>(path.size()); put(&pl, 4); out += path;
    put(&fs.mtime_ns, 8); put(&fs.size, 8); put(&fs.ctime_ns, 8); put(&fs.ino, 8); put(&fs.parsed_wall, 8);
    uint8_t fl = (fs.computed ? 1 : 0) | (fs.has_include_computed ? 2 : 0); put(&fl, 1);
    uint32_t n = static_cast<uint32_t>(fs.incs.size()); put(&n, 4);
    for (auto& d : fs.incs) { uint8_t k = (d.angled ? 1 : 0) | (d.next ? 2 : 0); put(&k, 1); uint16_t nl = static_cast<uint16_t>(d.name.size()); put(&nl, 2); out += d.name; }
  }
  return out;
}

void Scanner::import_state(const std::string& b) {
  if (b.size() < 4 || b.compare(0, 4, "SCN4") != 0) return;   // current format only (older memos lack ctime/inode)
  size_t i = 4;
  auto get = [&](void* p, size_t k) { if (i + k > b.size()) return false; std::memcpy(p, b.data() + i, k); i += k; return true; };
  std::unordered_map<std::string, FileScan> m;
  while (i < b.size()) {
    uint32_t pl; if (!get(&pl, 4) || pl > 65536 || i + pl > b.size()) break;
    std::string path = b.substr(i, pl); i += pl;
    FileScan fs; uint8_t fl; uint32_t n;
    if (!get(&fs.mtime_ns, 8) || !get(&fs.size, 8) || !get(&fs.ctime_ns, 8) || !get(&fs.ino, 8) || !get(&fs.parsed_wall, 8) || !get(&fl, 1) || !get(&n, 4) || n > 100000) break;
    fs.computed = fl & 1; fs.has_include_computed = fl & 2;
    bool bad = false;
    for (uint32_t k = 0; k < n; ++k) {
      uint8_t kind; uint16_t nl; if (!get(&kind, 1) || !get(&nl, 2) || i + nl > b.size()) { bad = true; break; }
      Directive d; d.angled = kind & 1; d.next = kind & 2; d.name = b.substr(i, nl); i += nl; fs.incs.push_back(std::move(d));
    }
    if (bad) break;
    m[path] = std::move(fs);
  }
  std::lock_guard<std::shared_mutex> g(mu_);
  preloaded_.swap(m);
}

}  // namespace opticc::incscan
