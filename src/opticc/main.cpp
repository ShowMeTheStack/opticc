// opticc — client: drop-in compiler wrapper (`opticc cc -c foo.c -o foo.o`,
// or masqueraded as `cc` via a symlink) and entry point for the coordinator
// (`opticc --coordinator`).
// SPDX-License-Identifier: GPL-2.0-only
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#ifdef __APPLE__
#include <copyfile.h>
#include <mach-o/dyld.h>
#include <sys/clonefile.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "coordinator.hpp"
#include "link_pipeline.hpp"
#include "opticc/argscan.hpp"
#include "opticc/cachekey.hpp"
#include "opticc/sysinc.hpp"
#include "opticc/closure.hpp"
#include "opticc/direct.hpp"
#include "opticc/incscan.hpp"
#include "opticc/noise.hpp"
#include "opticc/probe.hpp"
#include "opticc/transport.hpp"
#include "opticc/util.hpp"

extern char** environ;

using namespace opticc;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string self_exe(const char* argv0) {
#ifdef __APPLE__
  char buf[4096]; uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) == 0) { std::error_code ec; auto p = fs::canonical(buf, ec); if (!ec) return p.string(); return buf; }
#else
  std::error_code ec; auto p = fs::read_symlink("/proc/self/exe", ec); if (!ec) return p.string();
#endif
  return util::abspath(argv0);
}

// Masquerade support: find `name` on PATH, skipping any entry that is this
// executable (or a symlink to it), as distcc's masquerade dir does.
std::string find_real_compiler(const std::string& name, const std::string& self) {
  if (name.find('/') != std::string::npos) return name;
  std::string path = util::getenv_or("PATH", "/usr/bin:/bin");
  size_t start = 0;
  while (start <= path.size()) {
    auto colon = path.find(':', start);
    std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    if (dir.empty()) dir = ".";
    std::string cand = dir + "/" + name;
    std::error_code ec;
    if (fs::exists(cand, ec) && access(cand.c_str(), X_OK) == 0) {
      auto real = fs::canonical(cand, ec);
      if (!ec && real.string() != self) return cand;
    }
    if (colon == std::string::npos) break;
    start = colon + 1;
  }
  return name;  // let execvp fail with a sensible message
}

// Unix socket paths are limited to ~100 bytes (104 on macOS), and $TMPDIR on
// macOS is long, so the default lives in a per-user 0700 directory under /tmp.
std::string sock_path() {
  std::string s = util::getenv_or("OPTICC_SOCK", "");
  if (s.empty()) {
    std::string dir = "/tmp/opticc-" + std::to_string(getuid());
    mkdir(dir.c_str(), 0700);
    s = dir + "/coord.sock";
  }
  constexpr size_t kMax = sizeof(sockaddr_un{}.sun_path) - 1;   // 103 on macOS, 107 on Linux
  if (s.size() > kMax) {
    std::fprintf(stderr, "opticc: unix socket path too long (%zu > %zu bytes): %s\n"
                         "opticc: remedy: export OPTICC_SOCK=/tmp/opticc-%u/c.sock (any short path)\n",
                 s.size(), kMax, s.c_str(), static_cast<unsigned>(getuid()));
    std::exit(2);
  }
  return s;
}

void send(const net::Socket& s, const json& j) { std::string l = j.dump(); l.push_back('\n'); s.send_str(l); }
bool recv(const net::Socket& s, json* j) { std::string l; if (!s.recv_line(&l)) return false; *j = json::parse(l); return true; }

int exec_local(const std::vector<std::string>& argv) {
  std::vector<char*> av; for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
  execvp(av[0], av.data());
  std::fprintf(stderr, "opticc: cannot exec %s: %s\n", av[0], std::strerror(errno));
  return 127;
}

// Run the compiler locally with inherited stdio; returns rc and child CPU.
util::RunResult run_local(const std::vector<std::string>& argv) {
  return util::run_process(argv, "", "", "", nullptr, true, true);
}

// The direct cache path (opticc/direct.hpp). The wrapper computes the direct key from what it already has (argv,
// cwd, include-affecting environment, the compiler's stat identity, the raw source hash) and answers a hit by itself:
// no coordinator round trip, no include scan, no hashing of unchanged headers — about one stat() per input. Anything
// unexpected (no manifest, a changed input, an evicted or unreadable object, a non-zero cached rc) is a miss, and the
// request goes to the coordinator as before, carrying the key so the coordinator can record the entry.
struct Direct { std::string dir, dkey, why, dkey0; std::string src_sha; std::vector<std::string> env; };   // dkey0: key without the system-include signature; why: reason the direct path was not used (trace)
// src_sha and env (the direct key's source hash and include-affecting environment) travel to the coordinator,
// which derives the PORTABLE key of the shared manifests from them (opticc/rdirect.hpp)

Direct direct_key(const std::vector<std::string>& cmd, const args::Scan& sc, const std::string& cwd) {
  Direct d;
  auto no = [&](const std::string& w) { d.dkey.clear(); d.why = w; return d; };
  if (util::getenv_or("OPTICC_LOCAL_CACHE", "on") == "off" || util::getenv_or("OPTICC_DIRECT", "on") == "off") return no("disabled");
  if (!util::file_exists(sock_path() + ".cache")) return no("no-coordinator-cache");   // no coordinator with a client cache has run here yet
  { std::string bad; if (!direct::safe_args(cmd, &bad)) return no("uncacheable-flag " + bad); }   // inputs the direct path cannot track
  if (sc.source_index < 0 || sc.output.empty() || sc.lang == "ir" || cmd[0].find('/') == std::string::npos) return no("not-a-plain-compile");
  try {
    std::error_code ec; fs::path real = fs::canonical(cmd[0], ec);
    if (ec) return no("compiler-unresolved");
    d.dir = util::read_file(sock_path() + ".cache");   // the coordinator's cache (written when it starts with one)
    if (d.dir.empty() || d.dir[0] != '/') return no("bad-cache-dir");
    // the comment-insensitive key hash unless comments/debug info reach the object (as the coordinator records it).
    // A large source's hash comes from the shared memo when its stat is unchanged since it was settled
    // (direct.hpp hash_memo_get), so a repeat build does not re-read and re-tokenize every source.
    const bool ks = !cachekey::keeps_comments(cmd);
    const std::string sp = closure::absolutize(sc.source, cwd);
    static const bool memo = util::getenv_or("OPTICC_HASH_MEMO", "on") != "off";
    const direct::FStat st = direct::stat_path(sp);
    std::string src_sha;
    if (!(memo && st.size >= 16384 && direct::hash_memo_get(d.dir, sp, st, ks, &src_sha))) {
      const double t_hash = direct::stamp_now();   // the file-stamping clock
      src_sha = direct::content_sha(util::read_file(sp), ks);
      if (memo && st.exists && st.size >= 16384 && !direct::changed_since(st, t_hash) && direct::same(direct::stat_path(sp), st))
        direct::hash_memo_put(d.dir, sp, st, ks, src_sha);
    }
    // the system headers are not in the entry's inputs when the dependency file omits them
    // (-MMD); their signature, published by the LIVE coordinator next to its socket, enters the key instead. No
    // published signature, or its coordinator gone: no direct path (the coordinator path still serves the compile).
    const std::string ident = direct::compiler_identity(real.string());
    d.env = direct::relevant_env(); d.src_sha = src_sha;   // for the portable key
    d.dkey0 = direct::key(ident, cmd, cwd, d.env, src_sha);   // sent to the coordinator (it records under
                                                                            // H(dkey0 | ITS signature at request time))
    std::string blob;
    try { blob = util::read_file(sock_path() + ".sysinc." + util::sha256_hex(ident).substr(0, 24)); } catch (...) { d.why = "no-sysinc-signature"; return d; }
    const auto nl = blob.find('\n');
    if (nl == std::string::npos) { d.why = "bad-sysinc-signature"; return d; }
    const long pid = std::atol(blob.substr(0, nl).c_str());
    std::string sig = blob.substr(nl + 1); while (!sig.empty() && (sig.back() == '\n' || sig.back() == '\r')) sig.pop_back();
    if (pid <= 0 || sig.size() != 64 || ::kill(static_cast<pid_t>(pid), 0) != 0) { d.why = "sysinc-signature-stale"; return d; }
    d.dkey = direct::with_sysinc(d.dkey0, sig);
  } catch (const std::exception& e) { return no(std::string("key-error ") + e.what()); }
  return d;
}

// Write `data` to `path` atomically (temp file in the same directory + rename): a new inode, never an in-place
// rewrite and never a hard link (kbuild's objtool edits objects in place). The direct path's object goes this way
// too, so its bytes are verified before they are written.
bool put_atomic(const std::string& path, const std::string& data) {
  std::string tmp = path + ".opticc-" + std::to_string(getpid());
  try { util::write_file(tmp, data); } catch (...) { ::unlink(tmp.c_str()); return false; }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
  return true;
}

// OPTICC_DIRECT_TRACE=<file>: one line per wrapper decision (appended with one write):
//   <unix time> \t hit|miss \t <reason> \t <source> \t <direct key prefix>
// reasons: no-manifest, positive-stat-and-content (an input changed), shadowing-candidate, corrupt-manifest,
// object-missing, uncacheable-flag <arg>, no-coordinator-cache, disabled, ... The coordinator writes record-skipped
// lines (and why) to the same file.
void trace_direct(const Direct& d, const args::Scan& sc, bool hit, const std::string& why) {
  static const std::string path = util::getenv_or("OPTICC_DIRECT_TRACE", "");
  if (path.empty()) return;
  char t[32]; std::snprintf(t, sizeof t, "%.3f", direct::wall_now());
  std::string line = std::string(t) + "\t" + (hit ? "hit" : "miss") + "\t" + why + "\t" + sc.source + "\t" + d.dkey.substr(0, 12) + "\n";
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd >= 0) { (void)!::write(fd, line.data(), line.size()); ::close(fd); }
}

// Wrapper start time (static init) and a trace line for pass-through calls (non-compiles such as kbuild's
// cc-option probes and --version), so per-call wrapper overhead and call counts can be measured:
//   <unix time> \t pass \t <reason> \t <first arg> \t <ms from process start to exec>
static const double g_wrapper_t0 = util::now_s();
void trace_pass(const args::Scan& sc, const std::vector<std::string>& cmd) {
  static const std::string path = util::getenv_or("OPTICC_DIRECT_TRACE", "");
  if (path.empty()) return;
  char t[96]; std::snprintf(t, sizeof t, "%.3f\tpass\t", direct::wall_now());
  char ms[32]; std::snprintf(ms, sizeof ms, "\t%.3f\n", (util::now_s() - g_wrapper_t0) * 1e3);
  std::string line = std::string(t) + sc.reason + "\t" + (cmd.size() > 1 ? cmd[1] : std::string()) + ms;
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd >= 0) { (void)!::write(fd, line.data(), line.size()); ::close(fd); }
}

// Compiler-probe cache (opticc/probe.hpp). kbuild runs hundreds of probe compiles per make invocation inside
// $(shell ...) (cc-option, as-instr, --version, -dM -E ...), serially on the wall-clock path, each taking several
// milliseconds when exec'ed. The wrapper answers a repeated probe from <cache>/p in about a millisecond, without a
// coordinator, and stores a new one after running the compiler itself. Returns the exit status when the invocation was
// handled here, or -1 when it must take the normal path (nothing consumed: stdin is read only once caching applies).
void trace_probe(const char* what, const std::string& why, const std::vector<std::string>& cmd, const std::string& pk) {
  static const std::string path = util::getenv_or("OPTICC_DIRECT_TRACE", "");
  if (path.empty()) return;
  char t[96]; std::snprintf(t, sizeof t, "%.3f\t%s\t", direct::wall_now(), what);
  char ms[32]; std::snprintf(ms, sizeof ms, "\t%.3f\n", (util::now_s() - g_wrapper_t0) * 1e3);
  std::string line = std::string(t) + why + "\t" + (cmd.size() > 1 ? cmd[1] : std::string()) + "\t" + pk.substr(0, 12) + ms;
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd >= 0) { (void)!::write(fd, line.data(), line.size()); ::close(fd); }
}

// Run argv with stdin from `in_path` (inherited when empty) and stdout/stderr into files; *signalled on abnormal exit.
int spawn_capture(const std::vector<std::string>& argv, const std::string& in_path, const std::string& out_path,
                  const std::string& err_path, bool* signalled) {
  std::vector<char*> av; for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
  posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
  if (!in_path.empty()) posix_spawn_file_actions_addopen(&fa, 0, in_path.c_str(), O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_addopen(&fa, 2, err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  pid_t pid = 0;
  int err = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  *signalled = true;
  if (err != 0) { std::fprintf(stderr, "opticc: cannot exec %s: %s\n", av[0], std::strerror(err)); return 127; }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return 127; }
  if (WIFEXITED(status)) { *signalled = false; return WEXITSTATUS(status); }
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

void write_all(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) { ssize_t n = ::write(fd, s.data() + off, s.size() - off); if (n < 0) { if (errno == EINTR) continue; return; } off += static_cast<size_t>(n); }
}

int probe_run(const std::vector<std::string>& cmd) {
  if (util::getenv_or("OPTICC_CC_PROBES", "on") == "off" || util::getenv_or("OPTICC_LOCAL_CACHE", "on") == "off" ||
      util::getenv_or("OPTICC_DIRECT", "on") == "off") return -1;
  const probe::Shape ps = probe::classify(cmd);
  if (!ps.ok) { if (ps.looks_like_probe) trace_probe("probe-refused", ps.why, cmd, ""); return -1; }
  if (isatty(STDOUT_FILENO) || isatty(STDERR_FILENO) || (ps.stdin_input && isatty(STDIN_FILENO))) { trace_probe("probe-refused", "terminal", cmd, ""); return -1; }
  if (cmd[0].find('/') == std::string::npos) return -1;
  std::string dir;
  try { dir = util::read_file(sock_path() + ".cache"); } catch (...) { return -1; }   // no coordinator with a client cache ran here
  if (dir.empty() || dir[0] != '/') return -1;
  std::error_code ec; const fs::path real = fs::canonical(cmd[0], ec);
  if (ec) return -1;
  char cwdbuf[4096]; if (::getcwd(cwdbuf, sizeof cwdbuf) == nullptr) return -1;
  const std::string cwd = cwdbuf;
  // ---- committed from here on when the input is stdin: the bytes are read once and replayed to the compiler on a miss
  std::string in; bool in_ok = true;
  if (ps.stdin_input) {
    char buf[1 << 16];
    for (;;) { ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf); if (n > 0) { in.append(buf, static_cast<size_t>(n)); continue; } if (n < 0 && errno == EINTR) continue; if (n < 0) in_ok = false; break; }
  }
  const std::string cid = direct::compiler_identity(real.string());
  // a -c probe whose driver would run an external assembler is not cached (the assembler,
  // found through the driver's prefix and PATH, is not pinned by any key component)
  if (ps.mode == probe::Mode::Compile && probe::external_assembler(real.string(), probe::lang_of(cmd), dir + "/p/implicit")) {
    if (!ps.stdin_input) { trace_probe("probe-refused", "external assembler", cmd, ""); return -1; }
  }
  const bool ext_as = ps.mode == probe::Mode::Compile && probe::external_assembler(real.string(), probe::lang_of(cmd), dir + "/p/implicit");
  // the configuration identity also pins the files the driver reads implicitly
  const std::string imp = ps.mode == probe::Mode::Info ? std::string("info") : probe::implicit_identity(real.string(), probe::lang_of(cmd), dir + "/p/implicit");
  const std::string cfg = probe::config_identity(real.string()) + "|IMPLICIT|" + imp;
  const bool cacheable = in_ok && !ext_as && !imp.empty() && in.size() <= (1u << 20) && !(ps.stdin_input && probe::input_reads_files(in));
  const bool out_file = !ps.output.empty() && ps.output != "-" && ps.output != "/dev/null";
  std::string pk;
  if (cacheable) {
    pk = probe::key(cid, cfg, cmd, ps, cwd, probe::env(), util::sha256_hex(in));
    std::string blob; probe::Result r;
    try { blob = util::read_file(probe::entry_path(dir, pk)); } catch (...) {}
    if (!blob.empty() && probe::decode(blob, &r) && (r.rc != 0 || r.has_obj == out_file)) {
      // a hit: the output file (success only — a failing compiler creates none), then stdout and stderr, then rc
      if (r.rc != 0 || !out_file || put_atomic(ps.output, r.obj)) {
        write_all(STDOUT_FILENO, r.out); write_all(STDERR_FILENO, r.err);
        int fd = ::open((dir + "/state/probehits").c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd >= 0) { (void)!::write(fd, "h", 1); ::close(fd); }
        trace_probe("probe-hit", std::to_string(r.rc), cmd, pk);
        return r.rc;
      }
    }
  } else if (!ps.stdin_input) {   // nothing consumed: let the normal path exec the compiler
    trace_probe("probe-refused", in_ok ? "input" : "stdin read error", cmd, "");
    return -1;
  }
  // ---- miss: run the compiler (stdin replayed from a file), replay its streams, store a deterministic result
  std::string tdir;
  try { tdir = util::make_temp_dir("opticc-p-"); }
  catch (...) { if (!ps.stdin_input) return -1; std::fprintf(stderr, "opticc: cannot create a temporary directory\n"); return 1; }
  if (ps.stdin_input) { try { util::write_file(tdir + "/in", in); } catch (...) { std::fprintf(stderr, "opticc: cannot buffer stdin\n"); util::remove_tree(tdir); return 1; } }
  bool sig = true;
  const int rc = spawn_capture(cmd, ps.stdin_input ? tdir + "/in" : std::string(), tdir + "/o", tdir + "/e", &sig);
  probe::Result r; r.rc = rc;
  try { r.out = util::read_file(tdir + "/o"); } catch (...) {}
  try { r.err = util::read_file(tdir + "/e"); } catch (...) {}
  write_all(STDOUT_FILENO, r.out); write_all(STDERR_FILENO, r.err);
  if (cacheable) {
    std::string skip;
    if (sig) skip = "signalled";
    else if (rc != 0 && !(rc == 1 && probe::cacheable_failure(r.err))) skip = "failure not known deterministic (rc " + std::to_string(rc) + ")";
    if (skip.empty() && rc == 0 && out_file) {
      try { r.obj = util::read_file(ps.output); r.has_obj = true; } catch (...) { skip = "output missing"; }
      if (r.obj.size() > (8u << 20)) skip = "output too large";
    }
    if (skip.empty() && (r.out.size() + r.err.size() > (8u << 20))) skip = "streams too large";
    if (skip.empty() && (direct::compiler_identity(real.string()) != cid ||
                         probe::config_identity(real.string()) + "|IMPLICIT|" + (ps.mode == probe::Mode::Info ? std::string("info") : probe::implicit_identity(real.string(), probe::lang_of(cmd), dir + "/p/implicit")) != cfg))
      skip = "compiler or its implicit inputs changed during the run";
    if (skip.empty()) {
      const std::string ep = probe::entry_path(dir, pk);
      const std::string tmp = ep + ".tmp." + std::to_string(::getpid());
      try { util::mkdir_p(util::dirname(ep)); util::write_file(tmp, probe::encode(r)); if (std::rename(tmp.c_str(), ep.c_str()) != 0) { ::unlink(tmp.c_str()); skip = "rename failed"; } }
      catch (...) { ::unlink(tmp.c_str()); skip = "write failed"; }
    }
    trace_probe(skip.empty() ? "probe-store" : "probe-store-skipped", skip.empty() ? std::to_string(rc) : skip, cmd, pk);
  } else {
    trace_probe("probe-refused", in_ok ? "stdin names files" : "stdin read error", cmd, "");
  }
  util::remove_tree(tdir);
  return rc;
}

// Returns the compile's exit status on a hit, -1 on a miss (nothing written). *why: the miss reason.
int direct_hit(const Direct& d, const args::Scan& sc, const std::string& cwd, std::string* why) {
  static const bool dbg = util::getenv_or("OPTICC_DEBUG_CACHE", "") == "1";
  if (d.dkey.empty()) { *why = d.why; return -1; }
  const std::string okey = direct::lookup(d.dir, d.dkey, why);
  if (okey.empty()) { if (dbg) std::fprintf(stderr, "direct miss %s: %s\n", sc.source.c_str(), why->c_str()); return -1; }
  const std::string opath = d.dir + "/o/" + okey;
  std::string meta_s;
  try { meta_s = util::read_file(opath + ".meta"); } catch (...) { *why = "object-missing"; return -1; }
  json meta = json::parse(meta_s, nullptr, false);
  if (meta.is_discarded() || !meta.is_object() || meta.value("rc", 1) != 0) { *why = "bad-meta"; return -1; }
  {   // the object must have the size and XXH64 the meta recorded (a torn or incomplete store is a miss); verified
      // on a read-only mapping and copied in the kernel (copy_file_range / clone), never through user space
    if (!meta.contains("size") || !meta["size"].is_number_unsigned() || !meta.contains("xxh") || !meta["xxh"].is_string()) { *why = "bad-meta"; return -1; }
    const std::string xxh = meta["xxh"].get<std::string>();
    if (xxh.empty()) { *why = "bad-meta"; return -1; }
    if (!util::copy_verified_atomic(opath, closure::absolutize(sc.output, cwd), static_cast<int64_t>(meta["size"].get<uint64_t>()), xxh, why)) return -1;
  }
  if (sc.has_md) {   // exactly what the coordinator's cache hit writes
    std::string tgt = sc.dep_target.empty() ? sc.output : sc.dep_target;
    std::string dp = sc.dep_file.empty() ? args::default_dep_name(sc.output) : sc.dep_file;
    if (!put_atomic(closure::absolutize(dp, cwd), tgt + ":" + meta.value("depfile_body", std::string()))) { *why = "dep-write-failed"; return -1; }
  }
  std::string so = meta.value("stdout", std::string()), se = meta.value("stderr", std::string());
  if (!so.empty()) fwrite(so.data(), 1, so.size(), stdout);
  if (!se.empty()) fwrite(se.data(), 1, se.size(), stderr);
  ::utimes(opath.c_str(), nullptr);   // LRU: the coordinator orders its cache by object mtime at start
  int fd = ::open((d.dir + "/state/fasthits").c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);   // telemetry: 1 byte per hit
  if (fd >= 0) { (void)!::write(fd, "h", 1); ::close(fd); }
  return 0;
}

// A local compile whose output can be cached by the coordinator. When stderr is not a terminal (make with
// redirected output, CI, test scripts) stdout/stderr are captured, re-emitted unchanged, and returned so that a later
// client-cache hit replays them exactly (as ccache does). On a terminal the compiler keeps its own (coloured) output and
// the result is reported as not captured, so the coordinator does not cache it.
struct LocalRun { util::RunResult rr; bool captured = false; std::string out, err; };
LocalRun run_local_capturing(const std::vector<std::string>& argv) {
  LocalRun L;
  if (isatty(STDERR_FILENO) || isatty(STDOUT_FILENO)) { L.rr = run_local(argv); return L; }
  std::string dir;
  try { dir = util::make_temp_dir("opticc-l-"); } catch (...) { L.rr = run_local(argv); return L; }
  L.rr = util::run_process(argv, dir + "/o", dir + "/e", "", nullptr, false, false);
  try { L.out = util::read_file(dir + "/o"); } catch (...) {}
  try { L.err = util::read_file(dir + "/e"); } catch (...) {}
  util::remove_tree(dir);
  if (!L.out.empty()) fwrite(L.out.data(), 1, L.out.size(), stdout);
  if (!L.err.empty()) fwrite(L.err.data(), 1, L.err.size(), stderr);
  fflush(stdout); fflush(stderr);
  L.captured = L.out.size() + L.err.size() < (1u << 20);
  if (!L.captured) { L.out.clear(); L.err.clear(); }
  return L;
}
// After a captured local compile the coordinator reads the object and the dependency file before this process
// exits: make's next command may delete the .d (kbuild's fixdep) or edit the object in place (objtool), so reading
// them afterwards could lose the result or cache a post-objtool object. The coordinator answers {"op":"stored"} once it holds the bytes (≤ 5 s wait, then proceed).
void wait_stored(const net::Socket& co) {
  struct timeval tv{5, 0}; ::setsockopt(co.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  json a; try { recv(co, &a); } catch (...) {}
}

json done_msg(const LocalRun& L, double extra_cpu, uint64_t obj_bytes) {
  json d{{"op", "done"}, {"rc", L.rr.rc}, {"cpu_s", L.rr.cpu_s + extra_cpu}, {"wall_s", L.rr.wall_s}, {"obj_bytes", obj_bytes}, {"captured", L.captured}};
  if (L.captured) { d["stdout"] = L.out; d["stderr"] = L.err; }
  return d;
}

void spawn_coordinator(const std::string& self) {
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid == 0) {
    setsid();
    pid_t p2 = fork();
    if (p2 != 0) _exit(0);
    std::string logp = util::getenv_or("OPTICC_LOG", "/dev/null");
    int fd = open(logp.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
    int dn = open("/dev/null", O_RDONLY); if (dn >= 0) { dup2(dn, 0); if (dn > 2) close(dn); }
    // a daemon keeps nothing of its spawner. Inherited descriptors include make's jobserver pipe; holding it open can
    // hang the build in read() for a job token (seen with GNU make 3.81 on macOS).
    { struct rlimit rl{}; int top = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) ? static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 65536)) : 4096;
      for (int k = 3; k < top; ++k) close(k); }
    execl(self.c_str(), self.c_str(), "--coordinator", (char*)nullptr);
    _exit(127);
  }
  int st; waitpid(pid, &st, 0);
}

net::Socket connect_coordinator(const std::string& self) {
  std::string path = sock_path();
  // ~4 s total: the spawned coordinator is ready in ~0.1-0.3 s with a probe cache, ~1 s on first contact.
  for (int attempt = 0; attempt < 400; ++attempt) {   // 10 ms polls (a cached coordinator is ready in ~0.09 s)
    try { return net::Socket::connect_unix(path); } catch (const std::exception&) {}
    if (attempt % 100 == 0) spawn_coordinator(self);   // again every ~1 s (a previous coordinator may have been exiting)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("cannot reach coordinator at " + path);
}

int coordinator_main(int argc, char** argv) {
  coord::Options o;
  o.sock_path = sock_path();
  o.hosts = util::getenv_or("OPTICC_HOSTS", "");
  o.policy = util::getenv_or("OPTICC_POLICY", "ect-window");
  o.token = util::getenv_or("OPTICC_TOKEN", "");
  o.summary_path = util::getenv_or("OPTICC_SUMMARY", "");
  o.predictor = util::getenv_or("OPTICC_PREDICTOR", "affine");
  o.idle_timeout_s = std::atof(util::getenv_or("OPTICC_IDLE_TIMEOUT", "1800").c_str());   // stay warm between builds
  o.zstd_level = std::atoi(util::getenv_or("OPTICC_ZSTD", "3").c_str());
  o.zstd_explicit = std::getenv("OPTICC_ZSTD") != nullptr;
  o.local_slots = std::atoi(util::getenv_or("OPTICC_LOCAL_SLOTS", "-1").c_str());
  o.verbose = util::getenv_or("OPTICC_VERBOSE", "") == "1";
  o.key_dir = util::getenv_or("OPTICC_KEY_DIR", "");
  o.security = util::getenv_or("OPTICC_SECURITY", "auto");
  o.tofu = util::getenv_or("OPTICC_TOFU", "") == "1";
  o.psk_file = util::getenv_or("OPTICC_PSK_FILE", "");
  o.verify_frac = std::atof(util::getenv_or("OPTICC_VERIFY", "0").c_str());
  o.verify_seed = std::strtoull(util::getenv_or("OPTICC_VERIFY_SEED", "1").c_str(), nullptr, 10);
  o.binding = util::getenv_or("OPTICC_BINDING", "");   // empty = follow the mode (classic late, header-push early)
  o.prefetch = util::getenv_or("OPTICC_PREFETCH", "1") != "0";
  o.prefetch_frac = std::atof(util::getenv_or("OPTICC_PREFETCH_FRAC", "0.3").c_str());
  o.prefetch_budget_mb = std::atof(util::getenv_or("OPTICC_PREFETCH_BUDGET_MB", "64").c_str());
  o.prefetch_warm = util::getenv_or("OPTICC_PREFETCH_WARM", util::getenv_or("HOME", "/tmp") + "/.cache/opticc/popular.txt");
  o.prefetch_min_remote = std::max(0, std::atoi(util::getenv_or("OPTICC_PREFETCH_MIN_REMOTE", "8").c_str()));
  o.prefetch_warm_mb = std::atof(util::getenv_or("OPTICC_PREFETCH_WARM_MB", "8").c_str());
  o.pred_gamma = std::atof(util::getenv_or("OPTICC_PRED_GAMMA", "0").c_str());
  o.seed = std::strtoull(util::getenv_or("OPTICC_SEED", "1").c_str(), nullptr, 10);
  o.window = util::getenv_or("OPTICC_WINDOW", "");
  o.link_domains = util::getenv_or("OPTICC_LINK_DOMAINS", "0") == "1";
  o.link_pipeline = util::getenv_or("OPTICC_LINK_PIPELINE", "pipelined");
  o.link_partition = util::getenv_or("OPTICC_DOMAIN_PARTITION", "placement");
  o.link_gc = util::getenv_or("OPTICC_DOMAIN_GC", "1") == "1";
  o.link_pcrel = util::getenv_or("OPTICC_DOMAIN_PCREL", "1") == "1";
  o.link_keep = util::getenv_or("OPTICC_DOMAIN_KEEP", "");
  o.link_report = util::getenv_or("OPTICC_LINK_REPORT", "");
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--hosts" && i + 1 < argc) o.hosts = argv[++i];
    else if (a == "--policy" && i + 1 < argc) o.policy = argv[++i];
    else if (a == "--summary" && i + 1 < argc) o.summary_path = argv[++i];
    else if (a == "--idle-timeout" && i + 1 < argc) o.idle_timeout_s = std::atof(argv[++i]);
    else if (a == "-v") o.verbose = true;
    else { std::fprintf(stderr, "opticc --coordinator: unknown option %s\n", a.c_str()); return 2; }
  }
  return coord::run(o);
}

}  // namespace

// opticc keygen [--dir D]: create ~/.config/opticc/client.{key,pub} (or show the existing one).
int keygen_main(int argc, char** argv) {
  std::string dir = util::getenv_or("OPTICC_KEY_DIR", "");
  for (int i = 2; i < argc; ++i) { std::string a = argv[i]; if (a == "--dir" && i + 1 < argc) dir = argv[++i]; else { std::fprintf(stderr, "usage: opticc keygen [--dir DIR]\n"); return 2; } }
  if (dir.empty()) dir = util::getenv_or("HOME", "/tmp") + "/.config/opticc";
  try {
    noise::KeyPair kp; bool created = noise::load_or_create_keypair(dir, "client", &kp);
    std::printf("%s client key in %s\n  public key:  %s\n  fingerprint: %s\nAdd the public key line to each worker's authorized_clients.\n",
                created ? "created" : "existing", dir.c_str(), kp.pub_b64().c_str(), noise::fingerprint(kp.pub).c_str());
  } catch (const std::exception& e) { std::fprintf(stderr, "opticc keygen: %s\n", e.what()); return 1; }
  return 0;
}


// `opticc --link -- <linker argv...>` (and, with OPTICC_LINK_DOMAINS=1, any link command given to the compiler
// wrapper): ask the coordinator for the domain-link plan, then exec the (possibly rewritten) command locally.
// Any failure falls back to the original command.
int link_main(const std::string& self, std::vector<std::string> cmd) {
  bool verbose = util::getenv_or("OPTICC_VERBOSE", "") == "1";
  if (util::getenv_or("OPTICC_LINK_DOMAINS", "0") != "1") return exec_local(cmd);
  net::Socket co;
  try { co = connect_coordinator(self); }
  catch (const std::exception& e) { std::fprintf(stderr, "opticc: %s; linking without domains\n", e.what()); return exec_local(cmd); }
  std::string cwd = fs::current_path().string();
  // the domain plan must arrive within OPTICC_LINK_PLAN_TIMEOUT_S (default 60 s); otherwise the original link runs,
  // so a pathological plan costs at most the deadline, never an unbounded stall.
  {
    double to = std::atof(util::getenv_or("OPTICC_LINK_PLAN_TIMEOUT_S", "60").c_str()); if (to <= 0) to = 60;
    struct timeval tv; tv.tv_sec = static_cast<time_t>(to); tv.tv_usec = static_cast<suseconds_t>((to - static_cast<double>(tv.tv_sec)) * 1e6);
    ::setsockopt(co.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  try {
    send(co, {{"op", "link"}, {"argv", cmd}, {"cwd", cwd}, {"pid", getpid()}, {"report", util::getenv_or("OPTICC_LINK_REPORT", "")},
              {"pipeline", util::getenv_or("OPTICC_LINK_PIPELINE", "")}, {"partition", util::getenv_or("OPTICC_DOMAIN_PARTITION", "")},
              {"gc", util::getenv_or("OPTICC_DOMAIN_GC", "")}, {"pcrel", util::getenv_or("OPTICC_DOMAIN_PCREL", "")},
              {"force", util::getenv_or("OPTICC_LINK_FORCE", "")}, {"min_mb", util::getenv_or("OPTICC_LINK_MIN_MB", "")}});
    json d; if (!recv(co, &d)) throw std::runtime_error("coordinator closed");
    if (d.value("used_domains", false)) {
      cmd = d.value("argv", cmd);
      if (verbose) std::fprintf(stderr, "opticc: link via %llu domain object(s)\n", static_cast<unsigned long long>(d["report"].value("domains_formed", 0ULL)));
    } else if (verbose) std::fprintf(stderr, "opticc: link fallback: %s\n", d.value("error", "").c_str());
  } catch (const std::exception& e) { std::fprintf(stderr, "opticc: link plan failed (%s); linking without domains\n", e.what()); }
  return exec_local(cmd);
}
// `opticc cache-report <compiler> [tree...]`: read-only diagnostics of the system-include fingerprint. For the
// compiler: its fingerprinted directories, the stat-walk size and time (out-of-tree links followed), the content
// fingerprint time and every header marked as using a time macro. For each tree: the source files uses_time_macros flags.
static int cache_report_main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: opticc cache-report <compiler> [tree...]\n"); return 2; }
  const std::string cc = cachekey::resolve_compiler(argv[2]);
  const auto dirs = sysinc::dirs_for(cc);
  std::printf("compiler %s\n", cc.c_str());
  for (auto& d : dirs) std::printf("  dir %s\n", d.c_str());
  auto t0 = std::chrono::steady_clock::now();
  std::map<std::string, sysinc::FileSig> files; sysinc::stat_signature(dirs, &files);
  auto t1 = std::chrono::steady_clock::now();
  std::map<std::string, sysinc::FileSig> memo; sysinc::content_fingerprint(dirs, &memo);
  auto t2 = std::chrono::steady_clock::now();
  size_t outside = 0; for (auto& [p, f] : files) { bool in = false; for (auto& d : dirs) if (util::path_within(p, d)) { in = true; break; } if (!in) ++outside; }
  std::printf("stat walk: %zu entries (%zu outside the directories, via links) in %.3f s; content fingerprint (cold) %.3f s\n", files.size(), outside,
              std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count());
  size_t nt = 0; for (auto& [p, f] : memo) if (f.sha.rfind("T:", 0) == 0) { ++nt; std::printf("  time-macro header: %s\n", p.c_str()); }
  std::printf("time-macro headers: %zu\n", nt);
  for (int a = 3; a < argc; ++a) {
    size_t scanned = 0, flagged = 0; std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(argv[a], std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      const std::string p = it->path().string(); const auto dot = p.rfind('.'); if (dot == std::string::npos) continue;
      const std::string ext = p.substr(dot);
      if (ext != ".h" && ext != ".c" && ext != ".S" && ext != ".cpp" && ext != ".cc" && ext != ".inc" && ext != ".def" && ext != ".hpp") continue;
      std::error_code e2; if (!it->is_regular_file(e2)) continue;
      ++scanned;
      try { if (cachekey::uses_time_macros(util::read_file(p))) { ++flagged; if (flagged <= 50) std::printf("  flagged: %s\n", p.c_str()); } } catch (...) {}
    }
    std::printf("tree %s: %zu files scanned, %zu flagged\n", argv[a], scanned, flagged);
  }
  return 0;
}

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  if (argc >= 3 && std::string(argv[1]) == "cache-report") return cache_report_main(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "--coordinator") return coordinator_main(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "keygen") return keygen_main(argc, argv);
  if (argc >= 3 && std::string(argv[1]) == "--link") {   // opticc --link [--] <linker argv>
    int start = (std::string(argv[2]) == "--") ? 3 : 2;
    std::vector<std::string> lcmd(argv + start, argv + argc);
    if (lcmd.empty()) { std::fprintf(stderr, "usage: opticc --link -- <linker> [args...]\n"); return 2; }
    return link_main(self_exe(argv[0]), lcmd);
  }
  if (argc >= 3 && std::string(argv[1]) == "--scan-closure") {
    // Debugging aid: print the include scanner's closure for a compile command (cwd = current directory), one
    // absolute path per line, then "# computed=0|1 files_scanned=N". Useful for comparison with `clang -H`.
    std::vector<std::string> cmd(argv + 2, argv + argc);
    args::Scan sc = args::scan(cmd);
    if (sc.source_index < 0) { std::fprintf(stderr, "opticc --scan-closure: not a compile command\n"); return 2; }
    char cwdbuf[4096]; std::string cwd = getcwd(cwdbuf, sizeof cwdbuf) ? cwdbuf : ".";
    std::string lang = sc.lang.empty() ? "c" : sc.lang;
    auto sysd = closure::system_include_dirs(cmd[0], lang, cwd);
    auto dirs = args::include_dirs(cmd);
    incscan::Scanner scanner; bool computed = false; int n = 0;
    auto files = scanner.closure(closure::absolutize(sc.source, cwd), dirs, sysd, cwd, &computed, &n);
    for (auto& f : files) std::printf("%s\n", f.c_str());
    std::printf("# computed=%d files_scanned=%d system_dirs=%zu\n", computed ? 1 : 0, n, sysd.size());
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "--bench-crypto") {   // ChaCha20-Poly1305 encrypt+decrypt throughput
    size_t mb = argc >= 3 ? static_cast<size_t>(std::atoi(argv[2])) : 16;
    double bps = noise::benchmark_bytes_per_s(mb << 20);
    std::printf("{\"bench\":\"chacha20poly1305-encrypt-decrypt\",\"bytes\":%zu,\"bytes_per_s\":%.0f,\"MB_per_s\":%.1f}\n", mb << 20, bps, bps / 1e6);
    return 0;
  }
  std::string self = self_exe(argv[0]);
  std::string me = util::basename(argv[0]);
  std::vector<std::string> cmd;
  if (me == "opticc") {
    if (argc < 2) { std::fprintf(stderr, "usage: opticc <compiler> [args...] | opticc --coordinator [--hosts H] [--policy P] | opticc keygen [--dir D]\n"); return 2; }
    for (int i = 1; i < argc; ++i) cmd.emplace_back(argv[i]);
  } else {  // masqueraded: argv[0] is the compiler name
    cmd.emplace_back(me);
    for (int i = 1; i < argc; ++i) cmd.emplace_back(argv[i]);
  }
  std::string remote_name = util::basename(cmd[0]);   // what the worker will exec
  cmd[0] = find_real_compiler(cmd[0], self);
  bool verbose = util::getenv_or("OPTICC_VERBOSE", "") == "1";

  args::Scan sc = args::scan(cmd);
  if (!sc.distributable && coord::looks_like_link(cmd) && util::getenv_or("OPTICC_LINK_DOMAINS", "0") == "1") return link_main(self, cmd);
  // An assembler compile (never distributed) still uses the client caches when a coordinator with a client cache
  // has run here (as ccache caches it): the direct path below, else the coordinator answers from its cache or lets
  // this process compile at once and stores the result. Without a client cache it is exec'ed as before.
  const bool cache_local = !sc.distributable && sc.local_cacheable && util::getenv_or("OPTICC_HOSTS", "") != "__local__" &&
                           util::getenv_or("OPTICC_LOCAL_CACHE", "on") != "off" && util::file_exists(sock_path() + ".cache");
  if ((!sc.distributable && !cache_local) || util::getenv_or("OPTICC_HOSTS", "") == "__local__") {
    if (util::getenv_or("OPTICC_HOSTS", "") != "__local__") { int prc = probe_run(cmd); if (prc >= 0) return prc; }   // compiler-probe cache
    trace_pass(sc, cmd);
    if (verbose) std::fprintf(stderr, "opticc: local (%s): %s\n", sc.reason.c_str(), cmd.size() > 1 ? cmd[1].c_str() : "");
    return exec_local(cmd);
  }

  std::string cwd = fs::current_path().string();
  const double td0 = util::now_s();
  const Direct dk = direct_key(cmd, sc, cwd);   // answered here when the direct cache has it
  const double td1 = util::now_s();
  { std::string why; int rc = -1;
    try { rc = direct_hit(dk, sc, cwd, &why); } catch (...) { rc = -1; why = "bad-meta"; }   // a malformed meta is a miss
    trace_direct(dk, sc, rc >= 0, rc >= 0 ? std::string() : why);
    if (util::getenv_or("OPTICC_DEBUG_DIRECT_TIMING", "") == "1")
      std::fprintf(stderr, "direct-timing %s: key %.3f ms, lookup+materialise %.3f ms, %s\n", sc.source.c_str(), (td1 - td0) * 1e3, (util::now_s() - td1) * 1e3, rc >= 0 ? "hit" : "miss");
    if (rc >= 0) return rc; }

  net::Socket co;
  try { co = connect_coordinator(self); }
  catch (const std::exception& e) { std::fprintf(stderr, "opticc: %s; compiling locally\n", e.what()); return exec_local(cmd); }
  // every wait for the coordinator is bounded by OPTICC_WRAPPER_TIMEOUT_S (default 7200 s, above the
  // coordinator's per-attempt deadline of 3600 s plus a local fallback compile); past it this process compiles locally
  // (the catch below). The coordinator sees the closed connection and then never writes this job's output.
  { double to = std::atof(util::getenv_or("OPTICC_WRAPPER_TIMEOUT_S", "7200").c_str()); co.set_recv_timeout(to > 0 ? to : 7200); }

  std::vector<std::string> remote_cmd = cmd; remote_cmd[0] = remote_name;
  try {
    json req{{"op", "request"}, {"src", util::abspath(sc.source)}, {"out", sc.output}, {"lang", sc.lang}, {"cwd", cwd},
             {"src_bytes", util::file_size(sc.source)}, {"argv", remote_cmd}, {"cc_local", cmd[0]}, {"pid", getpid()}};
    if (!dk.dkey0.empty()) { req["dkey"] = dk.dkey0; req["dsrc"] = dk.src_sha; req["denv"] = dk.env; }   // dsrc/denv for the portable key
    { auto env = direct::relevant_env(); if (!env.empty()) req["env"] = env; }   // keys every cache and keeps the compile local
    send(co, req);
    json d; if (!recv(co, &d)) throw std::runtime_error("coordinator closed");
    int node = d.value("node", 0);
    // a local-only compile is answered from the client cache (header-push result below) or compiled here
    if (!sc.distributable && node != 0 && d.value("mode", "") != "header_push") throw std::runtime_error("unexpected decision for a local-only compile");
    if (node == 0) {
      auto L = run_local_capturing(cmd);
      try { send(co, done_msg(L, 0.0, util::file_size(sc.output))); if (d.value("ack", false)) wait_stored(co); } catch (...) {}
      return L.rr.rc;
    }
    if (d.value("mode", "classic") == "header_push") {
      // header-push: the coordinator ships source + headers and writes the object and dep file; we just wait
      json res; if (!recv(co, &res)) throw std::runtime_error("coordinator closed mid-job");
      if (res.value("op", "") == "fallback") {
        if (verbose) std::fprintf(stderr, "opticc: fallback to local for %s: %s\n", sc.source.c_str(), res.value("reason", "").c_str());
        auto L = run_local_capturing(cmd);
        try { send(co, done_msg(L, 0.0, util::file_size(sc.output))); if (res.value("ack", false)) wait_stored(co); } catch (...) {}
        return L.rr.rc;
      }
      std::string so = res.value("stdout", ""), se = res.value("stderr", "");
      if (!so.empty()) fwrite(so.data(), 1, so.size(), stdout);
      if (!se.empty()) fwrite(se.data(), 1, se.size(), stderr);
      return res.value("rc", 1);
    }
    // remote: preprocess locally (classic mode), ship via the coordinator
    std::string dir = util::make_temp_dir("opticc-");
    std::string ifile = dir + "/" + util::basename(sc.source);
    auto dot = ifile.rfind('.'); if (dot != std::string::npos) ifile = ifile.substr(0, dot);
    ifile += args::preprocessed_suffix(sc.lang);
    auto pp = util::run_process(args::preprocess_argv(cmd, sc, ifile), "", "", "", nullptr, true, true);
    if (pp.rc != 0 || !util::file_exists(ifile)) {
      util::remove_tree(dir);
      send(co, {{"op", "pp_failed"}, {"rc", pp.rc}});
      json fb; recv(co, &fb);
      auto rr = run_local(cmd);
      try { send(co, {{"op", "done"}, {"rc", rr.rc}, {"cpu_s", rr.cpu_s + pp.cpu_s}}); } catch (...) {}
      return rr.rc;
    }
    send(co, {{"op", "preprocessed"}, {"ifile", ifile}, {"pre_bytes", util::file_size(ifile)}, {"pp_cpu_s", pp.cpu_s}, {"pp_wall_s", pp.wall_s}});
    json res; if (!recv(co, &res)) throw std::runtime_error("coordinator closed mid-job");
    util::remove_tree(dir);
    if (res.value("op", "") == "fallback") {
      if (verbose) std::fprintf(stderr, "opticc: fallback to local for %s: %s\n", sc.source.c_str(), res.value("reason", "").c_str());
      auto rr = run_local(cmd);
      try { send(co, {{"op", "done"}, {"rc", rr.rc}, {"cpu_s", rr.cpu_s + pp.cpu_s}, {"obj_bytes", util::file_size(sc.output)}}); } catch (...) {}
      return rr.rc;
    }
    std::string so = res.value("stdout", ""), se = res.value("stderr", "");
    if (!so.empty()) fwrite(so.data(), 1, so.size(), stdout);
    if (!se.empty()) fwrite(se.data(), 1, se.size(), stderr);
    return res.value("rc", 1);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "opticc: %s; compiling locally\n", e.what());
    co.close();   // the socket is not close-on-exec; closing it tells the coordinator this job is ours now
    return exec_local(cmd);
  }
}
