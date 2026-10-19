// SPDX-License-Identifier: GPL-2.0-only
// opticc shared direct manifests; design and soundness argument in opticc/rdirect.hpp.
#include "opticc/rdirect.hpp"

#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <sodium.h>

#include "opticc/cachekey.hpp"
#include "opticc/util.hpp"

namespace fs = std::filesystem;

namespace opticc::rdirect {

using json = proto::json;

// ---- binary format ------------------------------------------------------------------------------------------------
//   "OPR2" u32 n; per entry: str obj, str obj_sha, f64 recorded, u8 keysha, u32 npos {str path, str sha, u64 size, str fsha},
//   u32 nneg {str dir, u32 nn, str names...};   str = u32 len + bytes
namespace {
struct W {
  std::string b;
  template <class T> void num(T v) { b.append(reinterpret_cast<const char*>(&v), sizeof v); }
  void str(const std::string& s) { num<uint32_t>(static_cast<uint32_t>(s.size())); b += s; }
};
struct R {
  const std::string& b; size_t i = 0; bool ok = true;
  explicit R(const std::string& s) : b(s) {}
  template <class T> T num() { T v{}; if (!ok || i + sizeof v > b.size()) { ok = false; return v; } std::memcpy(&v, b.data() + i, sizeof v); i += sizeof v; return v; }
  std::string str() { uint32_t n = num<uint32_t>(); if (!ok || i + n > b.size() || n > (1u << 20)) { ok = false; return {}; } std::string s = b.substr(i, n); i += n; return s; }
};
}  // namespace

PEntry from_direct(const direct::Entry& e, const std::string& obj_sha) {
  PEntry p; p.obj = e.obj; p.obj_sha = obj_sha; p.keysha = e.keysha; p.recorded_wall = e.recorded_wall;
  p.pos.reserve(e.pos.size());
  for (auto& x : e.pos) p.pos.push_back(PPos{x.path, x.sha, x.st.size});
  p.neg.reserve(e.neg.size());
  for (auto& g : e.neg) p.neg.push_back(PNeg{g.dir, g.names});
  return p;
}

std::string encode(const std::vector<PEntry>& es) {
  W w; w.b = "OPR2"; w.num<uint32_t>(static_cast<uint32_t>(es.size()));
  for (auto& e : es) {
    w.str(e.obj); w.str(e.obj_sha); w.num(e.recorded_wall); w.num<uint8_t>(e.keysha ? 1 : 0);
    w.num<uint32_t>(static_cast<uint32_t>(e.pos.size()));
    for (auto& p : e.pos) { w.str(p.path); w.str(p.sha); w.num<uint64_t>(p.size); w.str(p.fsha); }
    w.num<uint32_t>(static_cast<uint32_t>(e.neg.size()));
    for (auto& g : e.neg) { w.str(g.dir); w.num<uint32_t>(static_cast<uint32_t>(g.names.size())); for (auto& n : g.names) w.str(n); }
  }
  return w.b;
}

bool decode(const std::string& blob, std::vector<PEntry>* out) {
  out->clear();
  if (blob.size() < 8 || blob.compare(0, 4, "OPR2") != 0) return false;
  R r(blob); r.i = 4;
  uint32_t n = r.num<uint32_t>(); if (!r.ok || n > 1024) return false;
  for (uint32_t k = 0; k < n; ++k) {
    PEntry e; e.obj = r.str(); e.obj_sha = r.str(); e.recorded_wall = r.num<double>(); e.keysha = r.num<uint8_t>() != 0;
    uint32_t np = r.num<uint32_t>(); if (!r.ok || np > 1000000) return false;
    e.pos.resize(np);
    for (auto& p : e.pos) { p.path = r.str(); p.sha = r.str(); p.size = r.num<uint64_t>(); p.fsha = r.str(); }
    uint32_t ng = r.num<uint32_t>(); if (!r.ok || ng > 1000000) return false;
    e.neg.resize(ng);
    for (auto& g : e.neg) { g.dir = r.str(); uint32_t nn = r.num<uint32_t>(); if (!r.ok || nn > 1000000) return false; g.names.resize(nn); for (auto& x : g.names) x = r.str(); }
    if (!r.ok) return false;
    out->push_back(std::move(e));
  }
  return r.ok && r.i == blob.size();
}

std::string pkey(const std::string& fp, const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env, const std::string& src_sha) {
  std::string s = "opticc-rdirect-v3|F|"; s += fp; s += "|A|";
  for (auto& a : argv) { s += a; s.push_back('\0'); }
  s += "|W|"; s += cwd; s += "|E|";
  for (auto& e : env) { s += e; s.push_back('\0'); }
  s += "|S|"; s += src_sha;
  return util::sha256_hex(s);
}

bool env_ok(const std::vector<std::string>& env) {
  for (auto& e : env) if (e.rfind("SOURCE_DATE_EPOCH=", 0) != 0 && e.rfind("TZ=", 0) != 0) return false;
  return true;
}

std::vector<std::string> system_search_args(const std::vector<std::string>& argv) {
  static const char* sep[] = {"-isystem", "-idirafter", "-isysroot", "--sysroot", "-target", "--target", "-arch", "-iprefix", "-iwithprefix",
                              "-iwithprefixbefore", "-iwithsysroot", "-imultilib", "-cxx-isystem", "-resource-dir", "--gcc-toolchain",
                              "--gcc-install-dir", "-stdlib", "--rtlib"};
  static const char* flags[] = {"-nostdinc", "-nostdinc++", "-nostdlibinc", "-nobuiltininc", "-m16", "-m32", "-m64", "-mx32"};
  static const char* joined[] = {"-isystem", "-idirafter", "-isysroot", "--sysroot=", "-target=", "--target=", "-iprefix", "-iwithprefix",
                                 "-iwithsysroot", "-imultilib", "-cxx-isystem", "-resource-dir=", "--gcc-toolchain=", "--gcc-install-dir=",
                                 "-stdlib=", "--rtlib=", "-mmacos", "-miphoneos"};
  std::vector<std::string> out;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    bool done = false;
    for (const char* s : sep) if (a == s) { out.push_back(a); if (i + 1 < argv.size()) out.push_back(argv[++i]); done = true; break; }
    if (done) continue;
    for (const char* f : flags) if (a == f) { out.push_back(a); done = true; break; }
    if (done) continue;
    for (const char* j : joined) if (a.size() > std::strlen(j) && a.rfind(j, 0) == 0) { out.push_back(a); break; }
  }
  return out;
}

// ---- Validator --------------------------------------------------------------------------------------------------
static std::string blake2b_hex(const std::string& data) {
  unsigned char h[32];
  crypto_generichash(h, sizeof h, reinterpret_cast<const unsigned char*>(data.data()), data.size(), nullptr, 0);
  static const char* hex = "0123456789abcdef";
  std::string s(64, '0');
  for (int i = 0; i < 32; ++i) { s[2 * i] = hex[h[i] >> 4]; s[2 * i + 1] = hex[h[i] & 15]; }
  return s;
}

uint64_t Validator::thread_cpu_ns() {
  struct timespec ts{};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

std::string Validator::hash_of(const std::string& path, const direct::FStat& st, Kind k) {
  {
    std::shared_lock<std::shared_mutex> g(hmu_);
    auto it = hmemo_.find(path);
    if (it != hmemo_.end() && direct::same(it->second.st, st) && !it->second.h[k].empty()) { memo_hits++; return it->second.h[k]; }
  }
  const double t = direct::wall_now();
  // a SETTLED file (the memo's own condition) whose digest another thread is computing for this exact stat: wait for
  // it instead of reading and digesting the same bytes again (fresh builds start many TUs on the same headers at once)
  const bool settled_now = !direct::changed_since(st, t);
  std::shared_ptr<InFlight> mine;
  if (settled_now) {
    std::shared_ptr<InFlight> other;
    {
      std::lock_guard<std::mutex> g(fmu_);
      {   // the first reader may have finished between our memo miss and here: it memoised before leaving the slot
        std::shared_lock<std::shared_mutex> gm(hmu_);
        auto it = hmemo_.find(path);
        if (it != hmemo_.end() && direct::same(it->second.st, st) && !it->second.h[k].empty()) { memo_hits++; return it->second.h[k]; }
      }
      auto& slot = inflight_[k][path];
      if (slot && direct::same(slot->st, st)) other = slot;
      else { slot = std::make_shared<InFlight>(); slot->st = st; mine = slot; }
    }
    if (other) {
      std::unique_lock<std::mutex> l(other->m);
      other->cv.wait(l, [&] { return other->done; });
      if (!other->h.empty()) { shared_waits++; return other->h; }
      // the other reader failed (unreadable or changed while read): try ourselves, without sharing
    }
  }
  struct Publish {   // completes our in-flight slot on every return path
    Validator* v; std::shared_ptr<InFlight> f; Kind k; std::string path; std::string h;
    ~Publish() {
      if (!f) return;
      { std::lock_guard<std::mutex> g(f->m); f->h = h; f->done = true; }
      f->cv.notify_all();
      std::lock_guard<std::mutex> g(v->fmu_);
      auto it = v->inflight_[k].find(path);
      if (it != v->inflight_[k].end() && it->second == f) v->inflight_[k].erase(it);
    }
  } pub{this, mine, k, path, {}};
  const uint64_t c0 = thread_cpu_ns(); const auto w0 = std::chrono::steady_clock::now();
  std::string content;
  try { content = util::read_file(path); } catch (...) { return {}; }
  if (!direct::same(direct::stat_path(path), st)) return {};   // changed while read: the bytes may not be the stat's
  std::string s = k == Fast ? blake2b_hex(content) : direct::content_sha(content, k == Key);
  hashed_++;
  n_digest[k]++; bytes_digest[k] += content.size(); cpu_digest[k] += thread_cpu_ns() - c0;
  ns_digest[k] += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count());
  { std::lock_guard<std::mutex> g(dmu_); digested_paths_.insert(path); }
  if (!direct::changed_since(st, t)) {   // settled: any later change changes ctime, so the stat pins these bytes
    std::unique_lock<std::shared_mutex> g(hmu_);
    auto& m = hmemo_[path];
    if (!direct::same(m.st, st)) { m = HMemo{}; m.st = st; }
    m.h[k] = s;
    pub.h = s;   // shared only when memoisable: the same condition as the memo
  }
  return s;
}

static bool list_dir(const std::string& dir, std::vector<std::string>* names) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return false;
  names->clear();
  while (struct dirent* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
    names->emplace_back(e->d_name);
  }
  ::closedir(d);
  std::sort(names->begin(), names->end());
  return true;
}

#ifndef __APPLE__
int Validator::listing_has(const std::string& dir, const std::string& name, GroupSeen* seen) {
  std::string akey = dir; akey += '\0'; akey += name;
  if (auto a = seen->ans.find(akey); a != seen->ans.end()) return a->second;
  auto si = seen->st.find(dir);
  if (si == seen->st.end()) si = seen->st.emplace(dir, direct::stat_path(dir)).first;
  const direct::FStat st = si->second;
  auto done = [&](int r) { seen->ans.emplace(std::move(akey), r); return r; };
  if (!st.exists || !st.is_dir) return done(-1);
  {
    std::shared_lock<std::shared_mutex> g(lmu_);
    auto it = lmemo_.find(dir);
    if (it != lmemo_.end() && direct::same(it->second.st, st)) return done(std::binary_search(it->second.names.begin(), it->second.names.end(), name) ? 1 : 0);
  }
  const double t = direct::wall_now();
  std::vector<std::string> ls;
  if (!list_dir(dir, &ls)) return done(-1);
  listed_++;
  if (!direct::same(direct::stat_path(dir), st) || direct::changed_since(st, t)) return done(-1);   // not pinned: probe
  const int r = std::binary_search(ls.begin(), ls.end(), name) ? 1 : 0;
  {
    std::unique_lock<std::shared_mutex> g(lmu_);
    lmemo_[dir] = LMemo{st, std::move(ls)};
  }
  return done(r);
}

bool Validator::rel_absent(const std::string& dir, const std::string& rel, GroupSeen* seen) {
  auto probe = [&]() { neg_access++; return ::access((dir + "/" + rel).c_str(), F_OK) != 0; };
  std::string cur = dir;
  size_t p = 0;
  for (;;) {
    const size_t q = rel.find('/', p);
    const std::string comp = rel.substr(p, q == std::string::npos ? std::string::npos : q - p);
    if (comp.empty() || comp == "." || comp == "..") { neg_access_why[2]++; return probe(); }   // not a plain descent: let the kernel resolve it
    const int r = listing_has(cur, comp, seen);
    if (r < 0) { neg_access_why[1]++; return probe(); }
    if (r == 0) { neg_listed++; return true; }   // a missing component: nothing below it exists
    if (q == std::string::npos) { neg_access_why[3]++; return probe(); }  // the last component is listed: access() decides (rare in a valid entry; a dangling symlink stays absent)
    cur += "/"; cur += comp; p = q + 1;
  }
}

bool Validator::dir_absent_pinned(const std::string& dir, GroupSeen* seen) {
  std::string a = dir;
  for (int depth = 0; depth < 64; ++depth) {
    const size_t slash = a.rfind('/');
    if (slash == std::string::npos || slash == 0) return false;
    const std::string parent = a.substr(0, slash), comp = a.substr(slash + 1);
    if (comp.empty() || comp == "." || comp == "..") return false;
    const int r = listing_has(parent, comp, seen);
    if (r == 0) return true;      // the parent's pinned listing has no such entry: nothing below it exists
    if (r > 0) return false;      // listed (a file, a dangling symlink, ...): the per-name check decides
    if (seen->st.count(parent) && seen->st[parent].exists) return false;   // the parent exists but is not pinned
    a = parent;                   // the parent is missing too: ask its parent
  }
  return false;
}
#endif

bool Validator::names_absent(const std::string& dir, const std::vector<std::string>& names, direct::FStat* dst) {
  const direct::FStat st = direct::stat_path(dir); *dst = st;
  if (!st.exists || !st.is_dir) {   // a missing directory (or a file in its place) holds no candidate...
#ifndef __APPLE__
    {   // ...provably, when an existing ancestor's pinned listing lacks the path (one check per group)
      GroupSeen seen;
      if (!st.exists && dir_absent_pinned(dir, &seen)) { neg_listed += names.size(); return true; }
    }
#endif
    for (auto& n : names) { neg_access++; neg_access_why[0]++; if (::access((dir + "/" + n).c_str(), F_OK) == 0) return false; }   // ...but re-check (cheap, ENOENT/ENOTDIR)
    return true;
  }
  auto probe_all = [&]() { for (auto& n : names) { neg_access++; neg_access_why[1]++; if (::access((dir + "/" + n).c_str(), F_OK) == 0) return false; } return true; };
#ifdef __APPLE__
  for (auto& n : names) if (n.find('/') != std::string::npos) return probe_all();
#else
  // names with '/' are decided component by component from memoised listings (rel_absent); the rest of
  // the group below, against this directory's listing
  bool slashed = false;
  for (auto& n : names) if (n.find('/') != std::string::npos) { slashed = true; break; }
  if (slashed) {
    GroupSeen seen; seen.st.emplace(dir, st);
    std::vector<std::string> plain; plain.reserve(names.size());
    for (auto& n : names) {
      if (n.find('/') == std::string::npos) { plain.push_back(n); continue; }
      if (!rel_absent(dir, n, &seen)) return false;
    }
    if (plain.empty()) return true;
    return names_absent(dir, plain, dst);   // the single-component names: this directory's listing
  }
#endif
#ifdef __APPLE__
  // case- (and normalisation-) insensitive file systems: the listing is case-folded; a name with any match, or with a
  // non-ASCII byte, is decided by access() itself — the listing only proves absence (no case-folded match at all)
  auto fold = [](std::string x) { for (auto& ch : x) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a'); return x; };
  auto check = [&](const std::vector<std::string>& ls) {
    for (auto& n : names) {
      bool ascii = true; for (unsigned char ch : n) if (ch >= 0x80) ascii = false;
      if (!ascii || std::binary_search(ls.begin(), ls.end(), fold(n))) { if (::access((dir + "/" + n).c_str(), F_OK) == 0) return false; }
    }
    return true;
  };
#else
  auto check = [&](const std::vector<std::string>& ls) {
    for (auto& n : names) if (std::binary_search(ls.begin(), ls.end(), n)) return false;
    neg_listed += names.size();
    return true;
  };
#endif
  {
    std::shared_lock<std::shared_mutex> g(lmu_);
    auto it = lmemo_.find(dir);
    if (it != lmemo_.end() && direct::same(it->second.st, st)) return check(it->second.names);
  }
  const double t = direct::wall_now();
  std::vector<std::string> ls;
  if (!list_dir(dir, &ls)) return probe_all();
#ifdef __APPLE__
  for (auto& x : ls) for (auto& ch : x) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  std::sort(ls.begin(), ls.end());
#endif
  listed_++;
  // settled and unchanged while listed: a later creation/removal/rename in it changes its mtime and ctime, so an
  // exactly equal stat pins this listing (git's racy-clean rule, applied to the directory)
  if (!direct::same(direct::stat_path(dir), st) || direct::changed_since(st, t)) return probe_all();
  const bool ok = check(ls);
  std::unique_lock<std::shared_mutex> g(lmu_);
  lmemo_[dir] = LMemo{st, std::move(ls)};
  return ok;
}

bool Validator::valid(const PEntry& e, std::string* why, direct::Entry* local) {
  auto no = [&](const std::string& w) { if (why) *why = w; if (local) local->obj.clear(); return false; };
  const double t0 = direct::wall_now();
  std::vector<std::string> paths; paths.reserve(e.pos.size());
  for (auto& p : e.pos) paths.push_back(p.path);
  uint64_t cp = thread_cpu_ns();
  auto tick = [&cp](std::atomic<uint64_t>& acc, std::atomic<uint64_t>& cpu, std::chrono::steady_clock::time_point& t) {
    auto n = std::chrono::steady_clock::now(); acc += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(n - t).count()); t = n;
    const uint64_t c = thread_cpu_ns(); cpu += c - cp; cp = c; };
  auto tp = std::chrono::steady_clock::now();
  entries++; positives += e.pos.size();
  const std::vector<direct::FStat> sts = direct::stat_many(paths);
  tick(ns_stat, cpu_stat, tp);
  bool settled = true;
  if (local) { *local = direct::Entry{}; local->obj = e.obj; local->keysha = e.keysha; local->recorded_wall = t0; local->pos.resize(e.pos.size()); }
  for (size_t i = 0; i < e.pos.size(); ++i) {
    const auto& p = e.pos[i]; const auto& st = sts[i];
    if (!st.exists || st.is_dir) return no("positive gone: " + p.path);
    if (!e.keysha && st.size != p.size) return no("positive size: " + p.path);
    // the published exact-bytes hash first (no tokenizing); equal bytes have equal hashes of every kind
    bool ok = false;
    if (!p.fsha.empty()) {
      const std::string f = hash_of(p.path, st, Fast);
      if (f.empty()) return no("positive unreadable or changing: " + p.path);
      ok = f == p.fsha;
      if (!ok) fsha_mismatch++;
    } else {
      no_fsha++;
    }
    if (!ok) {
      const std::string cur = content_sha_of(p.path, st, e.keysha);
      if (cur.empty()) return no("positive unreadable or changing: " + p.path);
      if (cur != p.sha) return no("positive content: " + p.path);
    }
    if (local) { local->pos[i] = direct::Pos{p.path, st, p.sha}; if (direct::changed_since(st, t0)) settled = false; }
  }
  tick(ns_hash, cpu_hash, tp);
  const int64_t racy_ns = static_cast<int64_t>((t0 - direct::kRacyS) * 1e9);
  for (auto& g : e.neg) {
    direct::FStat dst;
    if (!names_absent(g.dir, g.names, &dst)) return no("shadowing candidate appeared in " + g.dir);
    if (local) {
      direct::NegGroup ng; ng.dir = g.dir; ng.st = dst; ng.names = g.names;
      ng.volatile_dir = dst.exists && dst.ctime_ns >= racy_ns;   // as direct::make_negative_groups
      local->neg.push_back(std::move(ng));
    }
  }
  tick(ns_neg, cpu_neg, tp);
  if (local && !settled) local->obj.clear();   // valid now, but not recordable as a stat-trusting local entry
  return true;
}

// ---- Toolchain fingerprint ----------------------------------------------------------------------------------------
static std::string lang_flag(const std::string& lang) {
  if (lang == "c++") return "c++";
  if (lang == "asm") return "assembler-with-cpp";
  if (lang == "objc") return "objective-c";
  if (lang == "objc++") return "objective-c++";
  return "c";
}

std::string Toolchain::fingerprint(const std::string& cc, const std::vector<std::string>& argv, const std::string& lang, bool full_deps,
                                   const std::string& cid, const std::string& sysfp, const std::vector<std::string>& sysdirs, std::string* why) {
  if (cid.empty() || sysfp.empty()) { if (why) *why = "compiler identity or system-include fingerprint not available"; return {}; }
  const std::vector<std::string> sys = system_search_args(argv);
  std::string mk = cc + "\x01" + lang + "\x01" + (full_deps ? "MD" : "MMD") + "\x01" + cid + "\x01" + sysfp;
  for (auto& a : sys) { mk += "\x01"; mk += a; }
  mk += "\x02"; for (auto& d : sysdirs) { mk += d; mk += "\x01"; }   // the coverage decision depends on them
  {
    std::lock_guard<std::mutex> g(mu_);
    auto it = fp_.find(mk);
    if (it != fp_.end()) { if (it->second.empty() && why) *why = why_[mk]; return it->second; }
  }
  auto refuse = [&](const std::string& w) { std::lock_guard<std::mutex> g(mu_); fp_[mk] = ""; why_[mk] = w; if (why) *why = w; return std::string(); };
  // the system include search list the compiler reports for exactly these arguments
  std::string tmpd;
  try { tmpd = util::make_temp_dir("opticc-rdv-"); } catch (...) { if (why) *why = "no temp dir"; return {}; }
  std::vector<std::string> cmd{cc}; cmd.insert(cmd.end(), sys.begin(), sys.end());
  const std::string lf = lang_flag(lang);
  for (const std::string& x : {std::string("-x"), lf, std::string("-E"), std::string("-v"), std::string("/dev/null")}) cmd.push_back(x);
  util::RunResult rr = util::run_process(cmd, tmpd + "/o", tmpd + "/e", "/");
  std::string err; try { err = util::read_file(tmpd + "/e"); } catch (...) {}
  util::remove_tree(tmpd);
  if (rr.rc != 0) {   // not memoised: a transient failure (macOS xcrun shims under load) must not disable the path
    std::string tail = err.size() > 160 ? err.substr(err.size() - 160) : err;
    for (auto& ch : tail) if (ch == '\n') ch = ' ';
    if (why) *why = "`-E -v` failed (rc " + std::to_string(rr.rc) + "): " + tail;
    return {};
  }
  std::vector<std::string> dirs; int sect = 0;
  size_t p = 0;
  while (p < err.size()) {
    size_t e = err.find('\n', p); if (e == std::string::npos) e = err.size();
    std::string l = err.substr(p, e - p); p = e + 1;
    if (l.find("search starts here:") != std::string::npos) { sect = 1; continue; }
    if (l.rfind("End of search list.", 0) == 0) { sect = 0; continue; }
    if (sect && !l.empty() && l[0] == ' ') {
      std::string d = l.substr(1);
      auto fw = d.find(" (framework directory)"); if (fw != std::string::npos) d = d.substr(0, fw);
      std::error_code ec; const fs::path c = fs::canonical(d, ec);
      dirs.push_back(ec ? fs::path(d).lexically_normal().string() : c.string());
    }
  }
  if (!full_deps) {
    // -MMD: the system headers are not in an entry's positives; their bytes must be covered by the sysinc content
    // fingerprint, i.e. every searched system directory must lie inside one of its directories
    for (auto& d : dirs) {
      std::error_code ec;
      if (!fs::exists(d, ec)) continue;   // searched but absent: holds nothing (its later creation changes sysinc's listing only
                                          // if inside a sysinc directory — otherwise refuse below would apply on the next run)
      bool inside = false;
      for (auto& s2 : sysdirs) if (d == s2 || util::path_within(d, s2)) { inside = true; break; }
      if (!inside) return refuse("system directory outside the system-include fingerprint: " + d);
    }
  }
  std::string s = "opticc-rdirect-F2|" + cc + "|" + cid + "|" + sysfp + "|" + lang + "|" + (full_deps ? "MD" : "MMD") + "|A|";
  for (auto& a : sys) { s += a; s.push_back('\0'); }
  s += "|D|";
  for (auto& d : dirs) { s += d; s.push_back('\0'); }
  // compiler configuration files (clang: <bindir>/*.cfg, /etc/clang/*.cfg) change what the driver does
  for (const std::string cfgdir : {util::dirname(cc), std::string("/etc/clang")}) {
    std::error_code ec;
    if (!fs::is_directory(cfgdir, ec)) continue;
    std::vector<std::string> cfgs;
    for (auto& de : fs::directory_iterator(cfgdir, ec)) if (de.path().extension() == ".cfg") cfgs.push_back(de.path().string());
    std::sort(cfgs.begin(), cfgs.end());
    for (auto& c : cfgs) { const direct::FStat st = direct::stat_path(c); s += "|cfg|" + c + "|" + files_.content_sha_of(c, st, false); }
  }
  const std::string fp = util::sha256_hex(s);
  std::lock_guard<std::mutex> g(mu_); fp_[mk] = fp; return fp;
}

// ---- worker Store -------------------------------------------------------------------------------------------------
std::string worker_object_key(const std::string& okey, const std::string& obj_sha) { return util::sha256_hex("opticc-rdirect-obj2|" + okey + "|" + obj_sha); }

const PEntry* select_for_get(const std::vector<PEntry>& es, const std::string& req) {
  if (es.empty()) return nullptr;
  if (req.empty()) return &es.back();
  const auto at = req.find('@');
  const std::string ok = req.substr(0, at), sha = at == std::string::npos ? std::string() : req.substr(at + 1);
  const PEntry* want = nullptr;
  for (auto& e : es) if (e.obj == ok && (sha.empty() || e.obj_sha == sha)) want = &e;
  return want;
}

void Store::init(const std::string& dir) {
  dir_ = dir; util::mkdir_p(dir_);
  std::error_code ec;
  for (auto& sub : fs::directory_iterator(dir_, ec)) {
    if (!sub.is_directory()) continue;
    for (auto& f : fs::directory_iterator(sub.path(), ec)) {
      std::string n = f.path().filename().string();
      if (n.size() == 64) keys_.insert(n);
    }
  }
}

static std::string zpack(const std::string& raw) {
  std::string out = "OPRZ"; proto::put_u32(out, static_cast<uint32_t>(raw.size()));
  return out + proto::zstd_compress(raw, 3);
}
static bool zunpack(const std::string& z, std::string* raw) {
  if (z.size() < 8 || z.compare(0, 4, "OPRZ") != 0) return false;
  try { *raw = proto::zstd_decompress(z.substr(8), proto::get_u32(z.data() + 4)); } catch (...) { return false; }
  return true;
}

bool Store::put(const std::string& pk, const PEntry& e) {
  if (dir_.empty() || pk.size() != 64 || pk.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
  std::lock_guard<std::mutex> g(lock_[std::hash<std::string>{}(pk) % 64]);
  const std::string p = path(pk);
  std::vector<PEntry> es;
  try { std::string raw; if (zunpack(util::read_file(p), &raw)) decode(raw, &es); } catch (...) {}
  es.erase(std::remove_if(es.begin(), es.end(), [&](const PEntry& x) { return x.obj == e.obj; }), es.end());
  es.push_back(e);
  if (es.size() > kMaxEntries) es.erase(es.begin(), es.begin() + static_cast<long>(es.size() - kMaxEntries));
  try {
    util::mkdir_p(util::dirname(p));
    char suf[48]; std::snprintf(suf, sizeof suf, ".tmp.%d", static_cast<int>(::getpid()));
    const std::string tmp = p + suf + std::to_string(std::hash<std::string>{}(e.obj) % 100000);
    util::write_file(tmp, zpack(encode(es)));
    if (std::rename(tmp.c_str(), p.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
  } catch (...) { return false; }
  std::lock_guard<std::mutex> k(kmu_); keys_.insert(pk);
  return true;
}

bool Store::get_stored(const std::string& pk, std::string* z) {
  if (dir_.empty() || pk.size() != 64 || pk.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
  try { *z = util::read_file(path(pk)); } catch (...) { return false; }
  return z->size() >= 8 && z->compare(0, 4, "OPRZ") == 0;
}

bool Store::get(const std::string& pk, std::string* encoded) {
  if (dir_.empty() || pk.size() != 64 || pk.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
  try { return zunpack(util::read_file(path(pk)), encoded); } catch (...) { return false; }
}

std::vector<std::string> Store::keys() const {
  std::lock_guard<std::mutex> g(kmu_);
  return std::vector<std::string>(keys_.begin(), keys_.end());
}

// ---- wire -----------------------------------------------------------------------------------------------------------
static json obj_header(const Object& o) {
  return json{{"okey", o.okey}, {"obj_sha", o.obj_sha}, {"rc", o.rc}, {"stdout", o.out}, {"stderr", o.err}, {"depfile_body", o.depfile_body}};
}
static Object obj_from(const json& h) {
  Object o; o.okey = h.value("okey", ""); o.obj_sha = h.value("obj_sha", ""); o.rc = h.value("rc", -1);
  o.out = h.value("stdout", ""); o.err = h.value("stderr", ""); o.depfile_body = h.value("depfile_body", "");
  return o;
}

bool entry_fits_kind(const PEntry& e, const std::string& kind) {
  if (kind.empty()) return !e.pos.empty();
  if (kind == "pp" || kind == "ppidx") return e.pos.empty() && e.neg.empty();
  return false;   // unknown kinds fit nothing
}
std::vector<PEntry> filter_kind(const std::vector<PEntry>& es, const std::string& kind) {
  std::vector<PEntry> out;
  for (auto& e : es) if (entry_fits_kind(e, kind)) out.push_back(e);
  return out;
}

proto::Frame encode_put(const PutMsg& m, uint64_t* wire) {
  const std::string entry = encode({m.entry});
  const std::string raw = entry + m.obj.object;
  const std::string z = proto::zstd_compress(raw, 1);
  json h = obj_header(m.obj);
  h["pkey"] = m.pkey; h["entry_raw_len"] = entry.size(); h["obj_raw_len"] = m.obj.object.size(); h["zstd_len"] = z.size();
  if (!m.kind.empty()) h["kind"] = m.kind;
  proto::Frame f{proto::Type::DirectPut, proto::pack(h, z)};
  if (wire) *wire = f.payload.size() + 5;
  return f;
}
PutMsg decode_put(const proto::Frame& f) {
  json h; std::string z; proto::unpack(f.payload, &h, &z);
  if (h.at("zstd_len").get<size_t>() != z.size()) throw std::runtime_error("DIRECT_PUT: truncated");
  const size_t el = h.at("entry_raw_len").get<size_t>(), ol = h.at("obj_raw_len").get<size_t>();
  const std::string raw = proto::zstd_decompress(z, el + ol);
  PutMsg m; m.pkey = h.at("pkey").get<std::string>(); m.obj = obj_from(h); m.kind = h.value("kind", std::string());
  std::vector<PEntry> es;
  if (!decode(raw.substr(0, el), &es) || es.size() != 1) throw std::runtime_error("DIRECT_PUT: bad entry");
  m.entry = std::move(es[0]); m.obj.object = raw.substr(el, ol);
  return m;
}
proto::Frame encode_get(const GetMsg& m) {
  json j{{"id", m.id}, {"pkey", m.pkey}, {"okey", m.okey}};
  if (!m.kind.empty()) j["kind"] = m.kind;
  return proto::Frame{proto::Type::DirectGet, j.dump()};
}
GetMsg decode_get(const proto::Frame& f) {
  json j = json::parse(f.payload);
  GetMsg m; m.id = j.at("id").get<int64_t>(); m.pkey = j.value("pkey", ""); m.okey = j.value("okey", ""); m.kind = j.value("kind", "");
  return m;
}
proto::Frame encode_reply(const ReplyMsg& m) {
  const std::string ez = !m.entries_z.empty() ? m.entries_z : (m.entries.empty() ? std::string() : zpack(m.entries));
  const std::string oz = m.obj.object.empty() ? std::string() : proto::zstd_compress(m.obj.object, 1);
  json h = obj_header(m.obj);
  h["id"] = m.id; h["have"] = m.have; h["entries_zlen"] = ez.size(); h["obj_raw_len"] = m.obj.object.size(); h["zstd_len"] = oz.size();
  return proto::Frame{proto::Type::DirectReply, proto::pack(h, ez + oz)};
}
ReplyMsg decode_reply(const proto::Frame& f) {
  json h; std::string b; proto::unpack(f.payload, &h, &b);
  const size_t ez = h.at("entries_zlen").get<size_t>(), oz = h.at("zstd_len").get<size_t>(), ol = h.at("obj_raw_len").get<size_t>();
  if (ez + oz != b.size()) throw std::runtime_error("DIRECT_REPLY: truncated");
  ReplyMsg m; m.id = h.at("id").get<int64_t>(); m.have = h.value("have", false); m.obj = obj_from(h);
  if (ez && !zunpack(b.substr(0, ez), &m.entries)) throw std::runtime_error("DIRECT_REPLY: bad entries");
  if (ol) m.obj.object = proto::zstd_decompress(b.substr(ez, oz), ol);
  return m;
}

}  // namespace opticc::rdirect
