// SPDX-License-Identifier: GPL-2.0-only
// opticc direct cache path; see opticc/direct.hpp for the design and the correctness argument.
#include "opticc/direct.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "opticc/cachekey.hpp"
#include "opticc/util.hpp"

namespace opticc::direct {

double racy_margin_s() {
  static const double m = [] {
    const char* e = std::getenv("OPTICC_RACY_S");
    if (e != nullptr && *e != '\0') {
      char* end = nullptr; const double v = std::strtod(e, &end);
      if (end != e && v >= 0 && v <= 10) return v;
    }
#if defined(__linux__) && defined(CLOCK_REALTIME_COARSE)
    struct timespec res{};
    if (::clock_getres(CLOCK_REALTIME_COARSE, &res) == 0) {
      const double r = static_cast<double>(res.tv_sec) + static_cast<double>(res.tv_nsec) / 1e9;
      if (r > 0 && r <= 0.05) return std::min(kRacyS, 2 * r + 0.002);
    }
#endif
    return kRacyS;
  }();
  return m;
}

bool changed_since(const FStat& f, double t0) {
  const double m = (f.ctime_ns % 1000000000LL == 0) ? std::max(kRacyCoarseS, racy_margin_s()) : racy_margin_s();
  return f.ctime_ns >= static_cast<int64_t>((t0 - m) * 1e9);
}

double wall_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

std::string with_sysinc(const std::string& dkey0, const std::string& sig) { return util::sha256_hex("opticc-direct-sysinc-v1|" + dkey0 + "|" + sig); }

double stamp_now() {
#if defined(__linux__) && defined(CLOCK_REALTIME_COARSE)
  struct timespec ts{};
  if (::clock_gettime(CLOCK_REALTIME_COARSE, &ts) == 0) return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
#endif
  return wall_now();
}

static FStat from_stat(const struct stat& st);

FStat stat_path(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return FStat{};
  return from_stat(st);
}

// stat() of many files that share directories: one open() per directory, then fstatat(dirfd, name) — the kernel
// walks one path component instead of all of them (absolute-path stats otherwise dominate a hit's syscall time)
namespace {
struct DirStatter {
  std::string dir; int fd = -1;
  ~DirStatter() { if (fd >= 0) ::close(fd); }
  FStat operator()(const std::string& path) {
    auto sl = path.rfind('/');
    if (sl == std::string::npos || sl == 0) return stat_path(path);
    if (fd < 0 || dir.size() != sl || path.compare(0, sl, dir) != 0) {
      if (fd >= 0) ::close(fd);
      dir.assign(path, 0, sl);
#ifdef O_PATH
      fd = ::open(dir.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
#else
      fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#endif
    }
    if (fd < 0) return stat_path(path);
    struct stat st{};
    if (::fstatat(fd, path.c_str() + sl + 1, &st, 0) != 0) return FStat{};
    return from_stat(st);
  }
};
}  // namespace

std::vector<FStat> stat_many(const std::vector<std::string>& paths) {
  std::vector<FStat> out; out.reserve(paths.size());
  DirStatter dst;
  for (auto& p : paths) out.push_back(dst(p));
  return out;
}

static FStat from_stat(const struct stat& st) {
  FStat f;
  f.exists = true; f.is_dir = S_ISDIR(st.st_mode);
  f.dev = static_cast<uint64_t>(st.st_dev); f.ino = static_cast<uint64_t>(st.st_ino); f.size = static_cast<uint64_t>(st.st_size);
#ifdef __APPLE__
  f.mtime_ns = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
  f.ctime_ns = static_cast<int64_t>(st.st_ctimespec.tv_sec) * 1000000000LL + st.st_ctimespec.tv_nsec;
#else
  f.mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
  f.ctime_ns = static_cast<int64_t>(st.st_ctim.tv_sec) * 1000000000LL + st.st_ctim.tv_nsec;
#endif
  return f;
}

bool same(const FStat& a, const FStat& b) {
  if (a.exists != b.exists) return false;
  if (!a.exists) return true;
  return a.is_dir == b.is_dir && a.dev == b.dev && a.ino == b.ino && a.size == b.size && a.mtime_ns == b.mtime_ns && a.ctime_ns == b.ctime_ns;
}

std::string compiler_identity(const std::string& resolved) {
  FStat f = stat_path(resolved);
  char buf[256];
  std::snprintf(buf, sizeof buf, "|%llu|%llu|%llu|%lld|%lld", (unsigned long long)f.dev, (unsigned long long)f.ino, (unsigned long long)f.size,
                (long long)f.mtime_ns, (long long)f.ctime_ns);
  return resolved + buf;
}

std::vector<std::string> relevant_env() {
  // environment that changes what the compiler reads or emits (include search, determinism knobs, SDK selection)
  static const char* names[] = {"CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "OBJCPLUS_INCLUDE_PATH",
                                "SOURCE_DATE_EPOCH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "IPHONEOS_DEPLOYMENT_TARGET",
                                "COMPILER_PATH", "GCC_EXEC_PREFIX", "LIBRARY_PATH", "CCC_OVERRIDE_OPTIONS", "CLANG_CONFIG_FILE_SYSTEM_DIR",
                                "CLANG_CONFIG_FILE_USER_DIR", "DEPENDENCIES_OUTPUT", "SUNPRO_DEPENDENCIES", "TZ"};
  std::vector<std::string> out;
  for (const char* n : names) if (const char* v = std::getenv(n)) { out.emplace_back(std::string(n) + "=" + v); }
  return out;
}

bool safe_args(const std::vector<std::string>& argv, std::string* why) {
  static const char* prefixes[] = {"@", "-fplugin", "-fprofile-use", "-fprofile-instr-use", "-fprofile-sample-use", "-fauto-profile",
                                   "-fsanitize-blacklist", "-fsanitize-ignorelist", "-fsanitize-coverage-allowlist",
                                   "-fsanitize-coverage-ignorelist", "-fmodules", "-fcxx-modules", "-fmodule-", "-fprebuilt-module",
                                   "-fimplicit-modules", "-include-pch", "-specs", "-B", "-fpch", "-emit-pch", "-fxray-always-instrument=",
                                   "-fxray-never-instrument=", "-fxray-attr-list=", "-fprofile-list=", "--config", "-fopenmp-host-ir-file-path",
                                   // flags that WRITE files besides the object and the
                                   // dependency file — a cache hit would leave them missing or stale
                                   "-ftime-trace", "-fstack-usage", "-fsave-optimization-record", "-foptimization-record-file",
                                   "-MJ", "-save-temps", "--save-temps", "-fdump-", "-Wa,-a", "-gsplit-dwarf", "-fcallgraph-info",
                                   "-ftest-coverage", "--coverage", "-fdebug-dump", "-fproc-stat-report",
                                   // more extra-output flags
                                   "-fopt-info", "-aux-info", "-fdiagnostics-format=sarif-file", "-fdiagnostics-add-output",
                                   "-Wp,-P", "-Xpreprocessor"};
  static const char* exact[] = {"-load", "-plugin", "-add-plugin", "-include-pch", "-", "-E", "-M", "-MM", "-fsyntax-only"};
  static const char* suffixes[] = {".pch", ".gch", ".pcm", ".profdata", ".profraw", ".afdo", ".so", ".dylib"};
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    auto bad = [&]() { if (why) *why = a; return false; };
    for (const char* p : prefixes) if (a.rfind(p, 0) == 0) return bad();
    for (const char* e : exact) if (a == e) return bad();
    for (const char* s : suffixes) { size_t n = std::strlen(s); if (a.size() >= n && a.compare(a.size() - n, n, s) == 0) return bad(); }
    if ((a == "-o" || a == "-MF") && i + 1 < argv.size() && (argv[i + 1] == "-" || argv[i + 1].rfind("/dev/", 0) == 0)) return bad();
    // an assembler listing requested inside a -Wa list, or through -Xassembler
    if (a.rfind("-Wa,", 0) == 0 && a.find(",-a") != std::string::npos) return bad();
    if (a == "-Xassembler" && i + 1 < argv.size() && argv[i + 1].rfind("-a", 0) == 0) return bad();
  }
  return true;
}

bool changes_system_search(const std::vector<std::string>& argv) {
  static const char* prefixes[] = {"--sysroot", "-isysroot", "-target", "--target", "--gcc-toolchain", "--gcc-install-dir", "-stdlib",
                                   "-resource-dir", "-nostdlibinc", "-nobuiltininc", "-nostdinc++", "-iwithsysroot", "-imultilib",
                                   "-cxx-isystem", "--rtlib", "-arch", "-m32", "-m64", "-mx32", "-miphoneos", "-mmacos"};
  for (size_t i = 1; i < argv.size(); ++i)
    for (const char* p : prefixes) if (argv[i].rfind(p, 0) == 0) return true;
  return false;
}

std::string key(const std::string& cid, const std::vector<std::string>& argv, const std::string& cwd, const std::vector<std::string>& env, const std::string& src_sha) {
  std::string s = "opticc-direct-v7|C|"; s += cid; s += "|A|";
  for (auto& a : argv) { s += a; s.push_back('\0'); }
  s += "|W|"; s += cwd; s += "|E|";
  for (auto& e : env) { s += e; s.push_back('\0'); }
  s += "|S|"; s += src_sha;
  return util::sha256_hex(s);
}

std::string manifest_path(const std::string& dir, const std::string& dk) {
  return dir + "/d/" + dk.substr(0, 2) + "/" + dk;
}

// ---- binary format ------------------------------------------------------------------------------------------------
//   "OPD4" u32 nentries; per entry: str obj; f64 recorded; u32 npos; per pos: str path, stat, str sha;
//   u32 ngroups; per group: str dir, stat, u8 volatile, u32 nnames, str names...
//   stat = u8 exists, u8 is_dir, u64 dev, u64 ino, u64 size, i64 mtime_ns, i64 ctime_ns;  str = u32 len + bytes
namespace {
struct W {
  std::string b;
  template <class T> void num(T v) { b.append(reinterpret_cast<const char*>(&v), sizeof v); }
  void str(const std::string& s) { num<uint32_t>(static_cast<uint32_t>(s.size())); b += s; }
  void st(const FStat& f) { num<uint8_t>(f.exists); num<uint8_t>(f.is_dir); num(f.dev); num(f.ino); num(f.size); num(f.mtime_ns); num(f.ctime_ns); }
};
struct R {
  const std::string& b; size_t i = 0; bool ok = true;
  explicit R(const std::string& s) : b(s) {}
  template <class T> T num() { T v{}; if (!ok || i + sizeof v > b.size()) { ok = false; return v; } std::memcpy(&v, b.data() + i, sizeof v); i += sizeof v; return v; }
  std::string str() { uint32_t n = num<uint32_t>(); if (!ok || i + n > b.size() || n > (1u << 20)) { ok = false; return {}; } std::string s = b.substr(i, n); i += n; return s; }
  FStat st() { FStat f; f.exists = num<uint8_t>(); f.is_dir = num<uint8_t>(); f.dev = num<uint64_t>(); f.ino = num<uint64_t>(); f.size = num<uint64_t>(); f.mtime_ns = num<int64_t>(); f.ctime_ns = num<int64_t>(); return f; }
};
}  // namespace

std::string encode(const std::vector<Entry>& es) {
  W w; w.b = "OPD4"; w.num<uint32_t>(static_cast<uint32_t>(es.size()));
  for (auto& e : es) {
    w.str(e.obj); w.num(e.recorded_wall); w.num<uint8_t>(e.keysha ? 1 : 0);
    w.num<uint32_t>(static_cast<uint32_t>(e.pos.size()));
    for (auto& p : e.pos) { w.str(p.path); w.st(p.st); w.str(p.sha); }
    w.num<uint32_t>(static_cast<uint32_t>(e.neg.size()));
    for (auto& g : e.neg) { w.str(g.dir); w.st(g.st); w.num<uint8_t>(g.volatile_dir); w.num<uint32_t>(static_cast<uint32_t>(g.names.size())); for (auto& n : g.names) w.str(n); }
  }
  return w.b;
}

bool decode(const std::string& blob, std::vector<Entry>* out) {
  out->clear();
  if (blob.size() < 8 || blob.compare(0, 4, "OPD4") != 0) return false;
  R r(blob); r.i = 4;
  uint32_t n = r.num<uint32_t>(); if (!r.ok || n > 1024) return false;
  for (uint32_t k = 0; k < n; ++k) {
    Entry e; e.obj = r.str(); e.recorded_wall = r.num<double>(); e.keysha = r.num<uint8_t>() != 0;
    uint32_t np = r.num<uint32_t>(); if (!r.ok || np > 1000000) return false;
    e.pos.resize(np);
    for (auto& p : e.pos) { p.path = r.str(); p.st = r.st(); p.sha = r.str(); }
    uint32_t ng = r.num<uint32_t>(); if (!r.ok || ng > 1000000) return false;
    e.neg.resize(ng);
    for (auto& g : e.neg) { g.dir = r.str(); g.st = r.st(); g.volatile_dir = r.num<uint8_t>(); uint32_t nn = r.num<uint32_t>(); if (!r.ok || nn > 1000000) return false; g.names.resize(nn); for (auto& x : g.names) x = r.str(); }
    if (!r.ok) return false;
    out->push_back(std::move(e));
  }
  return r.ok && r.i == blob.size();
}

std::string content_sha(const std::string& content, bool keysha) {
  return keysha ? cachekey::content_key_sha(content) : util::sha256_hex(content);
}

static std::string sha_of_file(const std::string& path, bool keysha) {
  try { return content_sha(util::read_file(path), keysha); } catch (...) { return {}; }
}

static std::string memo_path(const std::string& dir, const std::string& path, bool keysha) {
  const std::string h = util::sha256_hex(std::string(keysha ? "k|" : "r|") + path);
  return dir + "/h/" + h.substr(0, 2) + "/" + h.substr(2);
}

bool hash_memo_get(const std::string& dir, const std::string& path, const FStat& now, bool keysha, std::string* sha) {
  std::string b;
  try { b = util::read_file(memo_path(dir, path, keysha)); } catch (...) { return false; }
  if (b.size() < 4 || b.compare(0, 4, "OPH1") != 0) return false;
  R r(b); r.i = 4;
  const FStat st = r.st(); std::string s = r.str();
  if (!r.ok || r.i != b.size() || s.size() < 64 || s.size() > 80 || !st.exists || !same(st, now)) return false;   // raw: 64 hex; key hash: "n:"/"r:" + 64
  *sha = std::move(s);
  return true;
}

void hash_memo_put(const std::string& dir, const std::string& path, const FStat& st, bool keysha, const std::string& sha) {
  W w; w.b = "OPH1"; w.st(st); w.str(sha);
  const std::string mp = memo_path(dir, path, keysha);
  char suf[48]; std::snprintf(suf, sizeof suf, ".tmp.%d", static_cast<int>(::getpid()));
  const std::string tmp = mp + suf;
  try { util::write_file(tmp, w.b); }
  catch (...) { try { util::mkdir_p(util::dirname(mp)); util::write_file(tmp, w.b); } catch (...) { return; } }
  if (std::rename(tmp.c_str(), mp.c_str()) != 0) std::remove(tmp.c_str());
}

bool valid(const Entry& e, std::string* why) { return valid(e, why, std::string()); }

bool valid(const Entry& e, std::string* why, const std::string& memo_dir) {
  auto no = [&](const std::string& w) { if (why) *why = w; return false; };
  DirStatter dst;   // positives are recorded sorted by path: consecutive files share their directory
  for (auto& p : e.pos) {
    FStat now = dst(p.path);
    if (same(now, p.st)) continue;
    if (!now.exists || now.is_dir) return no("positive gone: " + p.path);
    if (!e.keysha && now.size != p.st.size) return no("positive size: " + p.path);
    // touched but identical (key) bytes: still valid. The hash of the current bytes comes from the shared memo
    // when its stat matches exactly; otherwise it is computed and memoised if the file is settled (hash_memo_get)
    std::string cur;
    if (memo_dir.empty() || !hash_memo_get(memo_dir, p.path, now, e.keysha, &cur)) {
      const double t_hash = wall_now();
      cur = sha_of_file(p.path, e.keysha);
      if (!memo_dir.empty() && !cur.empty() && !changed_since(now, t_hash) && same(stat_path(p.path), now))
        hash_memo_put(memo_dir, p.path, now, e.keysha, cur);
    }
    if (cur != p.sha) return no("positive content: " + p.path);
  }
  for (auto& g : e.neg) {
    if (!g.volatile_dir && same(stat_path(g.dir), g.st)) continue;   // nothing created/removed in this directory
    for (auto& n : g.names) {
      std::string p = g.dir + "/" + n;
      if (::access(p.c_str(), F_OK) == 0) return no("shadowing candidate appeared: " + p);
    }
  }
  return true;
}

std::string lookup(const std::string& dir, const std::string& dk, std::string* why) {
  std::string blob;
  try { blob = util::read_file(manifest_path(dir, dk)); } catch (...) { if (why) *why = "no manifest"; return {}; }
  std::vector<Entry> es;
  if (!decode(blob, &es)) { if (why) *why = "corrupt manifest"; return {}; }
  std::string first;   // the NEWEST entry's reason is reported (the one most likely meant for this build)
  static const bool memo = util::getenv_or("OPTICC_HASH_MEMO", "on") != "off";   // kill switch
  for (auto it = es.rbegin(); it != es.rend(); ++it) { std::string w; if (valid(*it, &w, memo ? dir : std::string())) return it->obj; if (first.empty()) first = w; }
  if (why) *why = first;
  return {};
}

void record(const std::string& dir, const std::string& dk, const Entry& e) {
  std::string mp = manifest_path(dir, dk);
  std::vector<Entry> es;
  try { decode(util::read_file(mp), &es); } catch (...) {}
  es.erase(std::remove_if(es.begin(), es.end(), [&](const Entry& x) { return x.obj == e.obj; }), es.end());
  es.push_back(e);
  if (es.size() > kMaxEntries) es.erase(es.begin(), es.begin() + static_cast<long>(es.size() - kMaxEntries));
  util::mkdir_p(util::dirname(mp));
  char suf[64]; std::snprintf(suf, sizeof suf, ".tmp.%d.%p", static_cast<int>(::getpid()), static_cast<const void*>(&e));
  std::string tmp = mp + suf;
  util::write_file(tmp, encode(es));
  if (std::rename(tmp.c_str(), mp.c_str()) != 0) std::remove(tmp.c_str());
}

bool make_negative_groups(const std::vector<std::string>& absent, double t0, std::vector<NegGroup>* out) {
  std::map<std::string, std::vector<std::string>> by_dir;
  for (auto& p : absent) {
    auto slash = p.rfind('/');
    if (slash == std::string::npos || slash == 0) continue;
    by_dir[p.substr(0, slash)].push_back(p.substr(slash + 1));
  }
  const int64_t racy_ns = static_cast<int64_t>((t0 - kRacyS) * 1e9);
  for (auto& [d, names] : by_dir) {
    NegGroup g; g.dir = d; g.st = stat_path(d);
    std::sort(names.begin(), names.end()); names.erase(std::unique(names.begin(), names.end()), names.end());
    for (auto& n : names) if (::access((d + "/" + n).c_str(), F_OK) == 0) return false;   // appeared meanwhile
    // an absent directory is volatile when its parent changed recently? keep it simple: absent dirs are re-checked by
    // stat (a creation makes them exist, which differs from the recorded "absent" state -> names are probed)
    g.volatile_dir = g.st.exists && g.st.ctime_ns >= racy_ns;
    g.names = std::move(names);
    out->push_back(std::move(g));
  }
  return true;
}

}  // namespace opticc::direct
