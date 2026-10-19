// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/sysinc.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <climits>
#include <set>
#include <sstream>

#include "opticc/closure.hpp"
#include "opticc/direct.hpp"
#include "opticc/cachekey.hpp"
#include "opticc/util.hpp"

namespace opticc::sysinc {

std::vector<std::string> dirs_for(const std::string& compiler) {
  std::vector<std::string> out;
  for (const char* lang : {"c", "c++"})
    for (auto& d : closure::system_include_dirs(compiler, lang, "/")) {
      struct stat st{};
      if (::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(d);
    }
  // no fallback list: a fingerprint of GUESSED directories would differ between two calls for the same compiler (one
  // parse failing, one succeeding) and between machines; an unparsable search list means "unknown" (the caller caches
  // nothing and retries later)
  std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
  // a directory nested inside another listed one is walked once (as part of the outer one)
  std::vector<std::string> keep;
  for (auto& d : out) {
    bool nested = false;
    for (auto& k : keep) if (d.size() > k.size() && d.compare(0, k.size(), k) == 0 && d[k.size()] == '/') { nested = true; break; }
    if (!nested) keep.push_back(d);
  }
  return keep;
}

namespace {
// A symlink inside a fingerprinted directory whose target lies OUTSIDE every fingerprinted directory
// (/usr/include/foo -> /opt/foo-1.2/include) would leave the bytes a compile reads through it uncovered: the target is
// followed — a file is recorded (stat in the signature, content in the fingerprint), a directory is walked (once).
// Targets inside a fingerprinted directory are covered there; only the link string is recorded.
struct Roots { std::vector<std::string> canon; std::set<std::string> visited; };
void walk(const std::string& dir, std::string& sig, std::map<std::string, FileSig>* files, int depth, Roots& roots) {
  if (depth > 64) { sig += "deep:" + dir + "\n"; return; }
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) { sig += "unreadable:" + dir + "\n"; return; }
  std::vector<std::string> names;
  while (dirent* e = ::readdir(d)) { if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) names.emplace_back(e->d_name); }
  const int fd = ::dirfd(d);
  std::sort(names.begin(), names.end());
  std::vector<std::string> subdirs;
  for (auto& n : names) {
    struct stat st{};
    if (::fstatat(fd, n.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) { sig += "gone:" + dir + "/" + n + "\n"; continue; }
#ifdef __APPLE__
    const int64_t mt = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
    const int64_t ct = static_cast<int64_t>(st.st_ctimespec.tv_sec) * 1000000000LL + st.st_ctimespec.tv_nsec;
#else
    const int64_t mt = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    const int64_t ct = static_cast<int64_t>(st.st_ctim.tv_sec) * 1000000000LL + st.st_ctim.tv_nsec;
#endif
    const std::string p = dir + "/" + n;
    char buf[200];
    std::snprintf(buf, sizeof buf, "|%o|%lld|%lld|%lld|%llu\n", static_cast<unsigned>(st.st_mode), static_cast<long long>(st.st_size),
                  static_cast<long long>(mt), static_cast<long long>(ct), static_cast<unsigned long long>(st.st_ino));
    sig += p; sig += buf;
    if (S_ISLNK(st.st_mode)) {   // a symlink's target string matters (headers are often symlinked); followed only out of the roots
      char t[4096]; ssize_t k = ::readlinkat(fd, n.c_str(), t, sizeof t - 1);
      if (k >= 0) { t[k] = 0; sig += "->"; sig += t; sig += "\n"; }
      if (files) { FileSig f; f.size = st.st_size; f.mtime_ns = mt; f.ctime_ns = ct; f.ino = st.st_ino; (*files)[p] = f; }
      char rp[PATH_MAX];
      if (::realpath(p.c_str(), rp) == nullptr) { sig += "dangling\n"; }
      else {
        const std::string r(rp);
        sig += "=>"; sig += r; sig += "\n";   // the END of the chain (an alternatives link may change it)
        bool inside = false;
        for (auto& c : roots.canon) if (util::path_within(r, c)) { inside = true; break; }
        struct stat ts{};
        if (!inside && ::stat(rp, &ts) == 0) {
          if (S_ISDIR(ts.st_mode)) { if (roots.visited.insert(r).second) { sig += "follow:" + r + "\n"; walk(r, sig, files, depth + 1, roots); } }
          else if (S_ISREG(ts.st_mode)) {
#ifdef __APPLE__
            const int64_t tm = static_cast<int64_t>(ts.st_mtimespec.tv_sec) * 1000000000LL + ts.st_mtimespec.tv_nsec;
            const int64_t tc = static_cast<int64_t>(ts.st_ctimespec.tv_sec) * 1000000000LL + ts.st_ctimespec.tv_nsec;
#else
            const int64_t tm = static_cast<int64_t>(ts.st_mtim.tv_sec) * 1000000000LL + ts.st_mtim.tv_nsec;
            const int64_t tc = static_cast<int64_t>(ts.st_ctim.tv_sec) * 1000000000LL + ts.st_ctim.tv_nsec;
#endif
            char b2[200];
            std::snprintf(b2, sizeof b2, "|%lld|%lld|%lld|%llu\n", static_cast<long long>(ts.st_size), static_cast<long long>(tm), static_cast<long long>(tc), static_cast<unsigned long long>(ts.st_ino));
            sig += "follow:" + r + b2;
            if (files) { FileSig f; f.size = ts.st_size; f.mtime_ns = tm; f.ctime_ns = tc; f.ino = ts.st_ino; (*files)[r] = f; }
          }
        }
      }
    } else if (S_ISDIR(st.st_mode)) {
      subdirs.push_back(p);
    } else if (S_ISREG(st.st_mode) && files) {
      FileSig f; f.size = st.st_size; f.mtime_ns = mt; f.ctime_ns = ct; f.ino = st.st_ino; (*files)[p] = f;
    }
  }
  ::closedir(d);
  for (auto& s : subdirs) walk(s, sig, files, depth + 1, roots);
}
}  // namespace

std::string stat_signature(const std::vector<std::string>& dirs, std::map<std::string, FileSig>* files) {
  std::string sig = "opticc-sysinc-stat-v2\n";
  Roots roots;
  for (auto& d : dirs) { char rp[PATH_MAX]; roots.canon.push_back(::realpath(d.c_str(), rp) ? std::string(rp) : d); }
  for (auto& d : dirs) { sig += "D:" + d + "\n"; walk(d, sig, files, 0, roots); }
  return util::sha256_hex(sig);
}

std::vector<std::string> explicit_system_dirs(const std::vector<std::string>& argv, const std::string& cwd) {
  std::vector<std::string> out;
  static const char* flags[] = {"-isystem-after", "-isystem", "-idirafter"};
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    for (const char* f : flags) {
      const std::string fl(f);
      if (a == fl && i + 1 < argv.size()) { out.push_back(closure::absolutize(argv[++i], cwd)); break; }
      if (a.size() > fl.size() && a.compare(0, fl.size(), fl) == 0 && !(fl == "-isystem" && a.compare(0, 14, "-isystem-after") == 0)) {
        out.push_back(closure::absolutize(a.substr(fl.size()), cwd)); break;
      }
    }
  }
  return out;
}

std::string time_macro_input(const std::vector<std::string>& tfiles, const std::vector<std::string>& compiler_dirs,
                             const std::vector<std::string>& argv, const std::string& cwd,
                             const std::vector<std::string>& dep_abs, bool dep_lists_system) {
  if (tfiles.empty()) return "";
  if (direct::changes_system_search(argv)) return tfiles.front();
  auto real = [](const std::string& p) { char rp[PATH_MAX]; return ::realpath(p.c_str(), rp) ? std::string(rp) : p; };
  std::set<std::string> treal; for (auto& t : tfiles) { treal.insert(t); treal.insert(real(t)); }
  if (dep_lists_system) {
    for (auto& d : dep_abs) if (treal.count(d) || treal.count(real(d))) return d;
    return "";
  }
  std::vector<std::string> dirs; bool nostdinc = false;
  static const char* sep[] = {"-I", "-isystem", "-idirafter", "-iquote", "-include", "-imacros"};
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-nostdinc") { nostdinc = true; continue; }
    for (const char* f : sep) {
      const std::string fl(f);
      if (a == fl && i + 1 < argv.size()) { dirs.push_back(closure::absolutize(argv[i + 1], cwd)); break; }
      if (a.size() > fl.size() && a.compare(0, fl.size(), fl) == 0 && (fl == "-I" || fl.size() > 2) &&
          (fl != "-I" || a.compare(0, 8, "-include") != 0)) { dirs.push_back(closure::absolutize(a.substr(fl.size()), cwd)); break; }
    }
  }
  if (!nostdinc) dirs.insert(dirs.end(), compiler_dirs.begin(), compiler_dirs.end());
  std::vector<std::string> rdirs = dirs; for (auto& d : dirs) rdirs.push_back(real(d));
  for (auto& t : treal) for (auto& d : rdirs) if (util::path_within(t, d)) return t;
  return "";
}

std::string content_fingerprint(const std::vector<std::string>& dirs, std::map<std::string, FileSig>* memo) {
  std::map<std::string, FileSig> now;
  stat_signature(dirs, &now);
  std::string acc = "opticc-sysinc-content-v2\n";
  for (auto& d : dirs) acc += "D:" + d + "\n";
  for (auto& [p, f] : now) {
    std::string sha;
    const FileSig* old = nullptr;
    if (memo) { auto it = memo->find(p); if (it != memo->end()) old = &it->second; }
    struct stat lst{};
    const bool is_link = ::lstat(p.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode);
    if (is_link) {   // never memoised: the link text AND the real path its chain ends in
      char t[4096]; ssize_t k = ::readlink(p.c_str(), t, sizeof t - 1); sha = "link:" + std::string(t, k > 0 ? static_cast<size_t>(k) : 0);
      char rp[PATH_MAX]; sha += "=>"; sha += ::realpath(p.c_str(), rp) ? std::string(rp) : std::string("dangling");
    } else if (old && old->size == f.size && old->mtime_ns == f.mtime_ns && old->ctime_ns == f.ctime_ns && old->ino == f.ino && !old->sha.empty()) {
      sha = old->sha;
    } else {
      {
        // "T:" marks a file that uses a time macro (memoised with the hash: the memo file is .memo2)
        try { const std::string txt = util::read_file(p); sha = (cachekey::uses_time_macros(txt) ? "T:" : "") + util::sha256_hex(txt); } catch (...) { sha = "unreadable"; }
      }
    }
    f.sha = sha;
    // the path relative to its search directory is what a compile names; the absolute path is the same on equal installs
    acc += p; acc += '\t'; acc += sha; acc += '\n';
  }
  if (memo) *memo = std::move(now);
  return util::sha256_hex(acc);
}

Tracker::Tracker(std::string memo_dir, double refresh_s) : memo_dir_(std::move(memo_dir)), refresh_s_(refresh_s > 0 ? refresh_s : 10.0) {
  thr_ = std::thread([this] { loop(); });
}

Tracker::~Tracker() { stop_ = true; if (thr_.joinable()) thr_.join(); }

void Tracker::on_change(std::function<void(const std::string&, const State&)> f) { std::lock_guard<std::mutex> g(mu_); cb_ = std::move(f); }

void Tracker::load_memo(const std::string& compiler, Ent& e) {
  if (memo_dir_.empty()) return;
  try {
    std::istringstream is(util::read_file(memo_dir_ + "/" + util::sha256_hex(compiler).substr(0, 24) + ".memo2"));
    std::string line;
    while (std::getline(is, line)) {
      std::istringstream ls(line); std::string p, sha; FileSig f;
      if (!std::getline(ls, p, '\t')) continue;
      ls >> f.size >> f.mtime_ns >> f.ctime_ns >> f.ino >> sha;
      if (!sha.empty()) { f.sha = sha; e.memo[p] = f; }
    }
  } catch (...) {}
}

void Tracker::save_memo(const std::string& compiler, const Ent& e) {
  if (memo_dir_.empty()) return;
  try {
    util::mkdir_p(memo_dir_);
    std::string s;
    for (auto& [p, f] : e.memo) {
      if (f.sha.empty() || p.find('\t') != std::string::npos || p.find('\n') != std::string::npos) continue;
      s += p; s += '\t'; s += std::to_string(f.size); s += ' '; s += std::to_string(f.mtime_ns); s += ' ';
      s += std::to_string(f.ctime_ns); s += ' '; s += std::to_string(f.ino); s += ' '; s += f.sha; s += '\n';
    }
    const std::string path = memo_dir_ + "/" + util::sha256_hex(compiler).substr(0, 24) + ".memo2";
    const std::string tmp = path + ".tmp" + std::to_string(::getpid());
    util::write_file(tmp, s); std::rename(tmp.c_str(), path.c_str());
  } catch (...) {}
}

State Tracker::get(const std::string& compiler, bool need_content) {
  std::shared_ptr<Ent> e;
  {
    std::lock_guard<std::mutex> g(mu_);
    auto it = ents_.find(compiler);
    if (it != ents_.end()) e = it->second;
  }
  if (!e) {
    auto ne = std::make_shared<Ent>();
    ne->dirs = dirs_for(compiler);
    if (ne->dirs.empty()) return State{};   // search list not parsable now: unknown, not cached (retried next time)
    ne->st.stat_sig = stat_signature(ne->dirs);
    ne->st.generation = 1; ne->st.ready = true;
    std::lock_guard<std::mutex> g(mu_);
    auto it = ents_.find(compiler);
    if (it == ents_.end()) { ents_[compiler] = ne; e = ne; } else e = it->second;
  }
  if (need_content) {
    // computed OUTSIDE the lock (it can take a second on a large SDK): peek() callers on the compile path never wait.
    // Concurrent computations are harmless (same result); the first to finish publishes, unless a refresh changed the
    // stat signature meanwhile (then the next call recomputes).
    std::vector<std::string> dirs; std::map<std::string, FileSig> memo; std::string sig0; bool done;
    { std::lock_guard<std::mutex> g(mu_); done = e->content_done; dirs = e->dirs; memo = e->memo; sig0 = e->st.stat_sig; }
    if (!done) {
      Ent tmp; if (memo.empty()) { load_memo(compiler, tmp); memo = std::move(tmp.memo); }
      const std::string fp = content_fingerprint(dirs, &memo);
      std::lock_guard<std::mutex> g(mu_);
      if (!e->content_done && e->st.stat_sig == sig0) {
        e->st.content_fp = fp; e->memo = std::move(memo); e->content_done = true;
        e->st.time_macro_files.clear(); for (auto& [p, f] : e->memo) if (f.sha.rfind("T:", 0) == 0) e->st.time_macro_files.push_back(p);
        save_memo(compiler, *e);
      }
    }
  }
  std::lock_guard<std::mutex> g(mu_);
  return e->st;
}

State Tracker::peek(const std::string& compiler, bool need_content) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = ents_.find(compiler);
  if (it != ents_.end() && (!need_content || it->second->content_done)) return it->second->st;
  for (auto& t : todo_) if (t.first == compiler && (t.second || !need_content)) return State{};
  todo_.emplace_back(compiler, need_content);
  return State{};
}

void Tracker::loop() {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#else
  ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);   // this thread only (Linux)
#endif
  using clk = std::chrono::steady_clock;
  auto next = clk::now() + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(refresh_s_));
  while (!stop_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {   // computations requested by peek()
      std::vector<std::pair<std::string, bool>> todo;
      { std::lock_guard<std::mutex> g(mu_); todo.swap(todo_); }
      for (auto& [c, content] : todo) { if (stop_.load()) return; try { (void)get(c, content); } catch (...) {} }
    }
    if (clk::now() < next) continue;
    next = clk::now() + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(refresh_s_));
    std::vector<std::pair<std::string, std::shared_ptr<Ent>>> all;
    { std::lock_guard<std::mutex> g(mu_); for (auto& kv : ents_) all.emplace_back(kv.first, kv.second); }
    for (auto& [c, e] : all) {
      if (stop_.load()) return;
      std::vector<std::string> dirs; { std::lock_guard<std::mutex> g(mu_); dirs = e->dirs; }
      const std::string s = stat_signature(dirs);
      std::function<void(const std::string&, const State&)> cb; State copy; bool changed = false;
      {
        std::lock_guard<std::mutex> g(mu_);
        if (s != e->st.stat_sig) {
          e->st.stat_sig = s; e->st.generation++; changed = true;
          // the content fingerprint is stale: unavailable (nothing cached against it) until recomputed off the lock
          if (e->content_done) { e->content_done = false; e->st.content_fp.clear(); todo_.emplace_back(c, true); }
          copy = e->st; cb = cb_;
        }
      }
      if (changed && cb) cb(c, copy);
    }
  }
}

}  // namespace opticc::sysinc
