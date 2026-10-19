// opticcd: the opticc worker daemon.
// Accepts persistent pipelined connections from coordinators, answers probes,
// and compiles preprocessed translation units with N concurrent slots.
// Security: the first bytes of a connection select the channel: a Noise IK handshake (default; the client's static key must be in
// --authorized-clients) or the magic "OPTICC-PLAIN\n" (only with --accept plain,
// optionally guarded by a --psk challenge). "OPTICC-PUBKEY?\n" returns our public
// key (for the client's trust-on-first-use). --allow CIDR is checked on accept;
// --token is still required in HELLO. Bind to a private (VPN/VPC) address in
// production.
// SPDX-License-Identifier: GPL-2.0-only
#include <signal.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <deque>
#include <map>
#include <filesystem>
#include <sstream>

#include <sodium.h>

#include "opticc/bloom.hpp"
#include "opticc/argscan.hpp"
#include "opticc/cachekey.hpp"
#include "objcache.hpp"
#include "opticc/rdirect.hpp"
#include "opticc/closure.hpp"
#include "opticc/direct.hpp"
#include "opticc/noise.hpp"
#include "opticc/protocol.hpp"
#include "opticc/sysinc.hpp"
#include "opticc/transport.hpp"
#include "opticc/util.hpp"
#include "opticc/elf/liveness.hpp"
#include "opticc/elf/prelink.hpp"
#include "opticc/elf/wire.hpp"

using namespace opticc;
using proto::json;

namespace {

struct Config {
  bool keep_objects = false;       // keep compiled objects in the content store for domain prelink
  bool accept_cache_uploads = true;   // CACHE_PUT from authenticated clients (--no-cache-uploads refuses them)
  std::string object_cache_dir;    // --object-cache DIR: content-addressed compile cache (empty = off)
  uint64_t object_cache_cap = 8ULL << 30;
  std::string bind_ip = "127.0.0.1"; int port = proto::kDefaultPort;  // safe default: never expose a port unless --listen names an address
  std::vector<std::pair<uint32_t, uint32_t>> allow;
  int jobs = util::online_cores();
  std::string tmp;
  std::string token;
  int zstd_level = 3;
  bool verbose = false;
  uint64_t cas_cap_bytes = 2ULL << 30;   // header-push content store cap (LRU)
  int hp_max_rounds = 8;                 // MISSING round trips before giving up
  int test_slow_ms = 0;                  // OPTICC_TEST_SLOW_MS: sleep before every compile (hedge e2e)
  double compile_timeout_s = 1800;       // OPTICC_COMPILE_TIMEOUT_S: a compiler running longer is killed (worker-error)
  // connection security
  std::string key_dir;                   // host.key / host.pub / authorized_clients live here
  std::string authorized_clients;        // file; default <key_dir>/authorized_clients
  bool accept_noise = true, accept_plain = false;
  std::string psk_file; bool have_psk = false; noise::Key psk{};
  noise::KeyPair host_key;
  bool test_corrupt = false;             // OPTICC_TEST_CORRUPT=1: flip a byte of every object (verify-mode tests only)
  std::string test_missing_name;         // OPTICC_TEST_MISSING_NAME (tests only): ask the client for this name in a MISSING round before every header-push compile, and log what came back (a hostile-worker probe)
  std::string test_missing_includer;     // OPTICC_TEST_MISSING_INCLUDER (tests only): the includer the probe names (a worker may claim any)
};

// Content-addressed store for header-push:
// <tmp>/opticcd-cas/<sha256>. Immutable files, LRU-evicted beyond the cap.
class Cas {
 public:
  void init(const std::string& dir, uint64_t cap) {
    dir_ = dir; cap_ = cap; util::mkdir_p(dir_);
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir_, ec)) {
      if (!e.is_regular_file()) continue;
      uint64_t sz = e.file_size(); std::string sha = e.path().filename().string();
      index_[sha] = {sz, ++tick_}; total_ += sz;
    }
  }
  // Defence in depth: a store key is always a sha256 in lowercase hex; anything else (e.g. a client
  // "sha" containing '/' or "..") never names a file. put() is only reached after the content was hashed to the key.
  static bool valid_key(const std::string& sha) { return sha.size() == 64 && sha.find_first_not_of("0123456789abcdef") == std::string::npos; }
  bool has(const std::string& sha) { if (!valid_key(sha)) return false; std::lock_guard<std::mutex> g(m_); auto it = index_.find(sha); if (it == index_.end()) return false; it->second.last = ++tick_; return true; }
  void put(const std::string& sha, const std::string& data) {
    if (!valid_key(sha)) throw std::runtime_error("cas: invalid key");
    std::string tmp = dir_ + "/." + sha + ".tmp." + std::to_string(reinterpret_cast<uintptr_t>(&data));
    util::write_file(tmp, data);
    std::error_code ec; std::filesystem::rename(tmp, dir_ + "/" + sha, ec);
    if (ec) { std::filesystem::remove(tmp, ec); }
    std::lock_guard<std::mutex> g(m_);
    if (!index_.count(sha)) { index_[sha] = {data.size(), ++tick_}; total_ += data.size(); }
    while (total_ > cap_ && index_.size() > 1) {   // evict least recently used
      auto victim = index_.begin();
      for (auto it = index_.begin(); it != index_.end(); ++it) if (it->second.last < victim->second.last) victim = it;
      if (victim->first == sha) break;
      std::filesystem::remove(dir_ + "/" + victim->first, ec);
      total_ -= victim->second.size; index_.erase(victim);
    }
  }
  std::string path(const std::string& sha) const { return dir_ + "/" + sha; }
  uint64_t total() const { return total_; }
  size_t count() const { return index_.size(); }
 private:
  struct Ent { uint64_t size; uint64_t last; };
  std::string dir_; uint64_t cap_ = 0, total_ = 0, tick_ = 0;
  std::unordered_map<std::string, Ent> index_;
  std::mutex m_;
};

// Counting semaphore for compile slots (C++20 std::counting_semaphore needs a
// compile-time max; a mutex/cv version keeps --jobs dynamic).
class Slots {
 public:
  explicit Slots(int n) : free_(n) {}
  void acquire() { std::unique_lock<std::mutex> l(m_); cv_.wait(l, [&] { return free_ > 0; }); --free_; }
  void release() { std::lock_guard<std::mutex> l(m_); ++free_; cv_.notify_one(); }
 private:
  std::mutex m_; std::condition_variable cv_; int free_;
};

// Per-job inbox for BLOBS frames (header-push), fed by the connection reader.
struct Inbox {
  std::mutex m; std::condition_variable cv;
  std::deque<proto::BlobsMsg> msgs; bool closed = false;
  bool wait(proto::BlobsMsg* out, double timeout_s) {
    std::unique_lock<std::mutex> l(m);
    if (!cv.wait_for(l, std::chrono::duration<double>(timeout_s), [&] { return closed || !msgs.empty(); })) return false;
    if (msgs.empty()) return false;
    *out = std::move(msgs.front()); msgs.pop_front(); return true;
  }
};

struct Conn {
  net::Socket sock;
  std::mutex wmu;
  std::mutex jobs_mu;
  std::unordered_map<int64_t, pid_t> running;      // id -> pid (0 = queued)
  std::unordered_map<int64_t, bool> cancelled;
  std::unordered_map<int64_t, std::shared_ptr<Inbox>> inboxes;
  std::atomic<int> active{0};
  std::string peer;
  void send(const proto::Frame& f) { std::lock_guard<std::mutex> g(wmu); sock.write_frame(f); }
};

Config g_cfg;
// Speed calibration (util::compile_bench_s), measured in a background thread after the listener is up so that
// probes arriving during start-up are not refused. HELLO_ACK reports 0 until it is done (the coordinator then uses a
// prior).
std::atomic<double> g_compile_bench_s{0.0};
std::unique_ptr<Slots> g_slots;
std::atomic<int64_t> g_jobs_done{0};
std::atomic<uint64_t> g_tm_refusals{0};   // stores refused for a time-macro system header
Cas g_cas;
std::atomic<uint64_t> g_prefetch_bytes{0}, g_prefetch_rejected{0};
std::atomic<uint64_t> g_uploads_stored{0}, g_uploads_rejected{0}, g_upload_bytes{0};   // CACHE_PUT
std::atomic<uint64_t> g_blobs_rejected{0};   // BLOBS whose bytes do not hash to their sha (never stored)
opticc::objcache::ObjectCache g_objcache;   // compile cache; enabled by --object-cache
opticc::rdirect::Store g_rdstore;           // published direct manifests (<object-cache>/rd); with --object-cache
std::atomic<uint64_t> g_rd_puts{0}, g_rd_rejected{0}, g_rd_gets{0}, g_rd_found{0};
// Fingerprint of the system include directories of each compiler this worker runs; part of
// every header-push cache key (the closure a client ships omits system headers). Only with --object-cache.
std::unique_ptr<opticc::sysinc::Tracker> g_sysinc;
static std::string sysinc_fp_for(const std::string& compiler_name) {
  if (!g_sysinc) return "";
  // computed once per compiler per daemon run (normally already done by the start-up warm-up: a stat walk plus the
  // on-disk content memo, ≈0.1 s on Linux; the very first run hashes the system headers once); afterwards a lookup
  try { return g_sysinc->get(opticc::cachekey::resolve_compiler(compiler_name), true).content_fp; } catch (...) { return ""; }
}

// The system directories this worker's fingerprint covers for a compiler (cached per daemon run). Only a
// dependency inside them may be left out of a cache entry's closure; any other unshipped input refuses the store.
static std::vector<std::string> fp_dirs_for(const std::string& compiler_name) {
  static std::mutex mu; static std::unordered_map<std::string, std::vector<std::string>> cache;
  const std::string c = opticc::cachekey::resolve_compiler(compiler_name);
  { std::lock_guard<std::mutex> g(mu); auto it = cache.find(c); if (it != cache.end()) return it->second; }
  std::vector<std::string> d;
  try { d = opticc::sysinc::dirs_for(c); } catch (...) { return {}; }
  if (d.empty()) return d;
  std::lock_guard<std::mutex> g(mu); cache[c] = d; return d;
}

void log_json(const json& j) { std::string s = j.dump(); s.push_back('\n'); fwrite(s.data(), 1, s.size(), stderr); }

// A request frame that does not decode still gets a RESULT (status worker-error) when its id can be
// read, so the client's attempt fails at once and takes the local fallback instead of waiting forever; and the
// connection's active-job count drops, so the reaper can join the handler thread.
void reject_request(const std::shared_ptr<Conn>& c, const proto::Frame& f, const char* event, const std::exception& e) {
  c->active--;
  const int64_t id = proto::frame_id(f);
  log_json({{"event", event}, {"peer", c->peer}, {"id", id}, {"error", e.what()}});
  if (id < 0) return;
  proto::CompileResult res; res.id = id; res.status = "worker-error"; res.rc = -1; res.err = std::string("opticcd: malformed request: ") + e.what();
  try { c->send(proto::encode_result(res, g_cfg.zstd_level)); } catch (const std::exception& se) { log_json({{"event", "send-failed"}, {"id", id}, {"error", se.what()}}); }
}

// Kills a compiler that runs longer than OPTICC_COMPILE_TIMEOUT_S. Armed from run_process's
// on_spawn callback, disarmed as soon as run_process returns (the watchdog thread is joined then, so it never signals
// a reaped pid afterwards; the only window is between the kernel reaping the child inside run_process and the disarm,
// microseconds, against a deadline of minutes).
class CompileWatchdog {
 public:
  void arm(pid_t pid) {
    if (!(g_cfg.compile_timeout_s > 0)) return;
    try {   // never throw out of run_process's on_spawn (the child would not be reaped)
      t_ = std::thread([this, pid] {
        std::unique_lock<std::mutex> l(m_);
        if (!cv_.wait_for(l, std::chrono::duration<double>(g_cfg.compile_timeout_s), [&] { return done_; })) { fired_ = true; kill(pid, SIGKILL); }
      });
    } catch (const std::exception& e) { log_json({{"event", "watchdog-unarmed"}, {"error", e.what()}}); }
  }
  void disarm() { { std::lock_guard<std::mutex> g(m_); done_ = true; } cv_.notify_all(); if (t_.joinable()) t_.join(); }
  bool fired() { std::lock_guard<std::mutex> g(m_); return fired_; }
  ~CompileWatchdog() { disarm(); }
 private:
  std::mutex m_; std::condition_variable cv_; std::thread t_; bool done_ = false, fired_ = false;
};
std::string timeout_note() { return "\nopticcd: compile timeout (OPTICC_COMPILE_TIMEOUT_S=" + std::to_string(static_cast<long long>(g_cfg.compile_timeout_s)) + " s): compiler killed"; }

void handle_compile(std::shared_ptr<Conn> c, proto::Frame f) {
  double t_recv = util::now_s();
  proto::CompileResult res;
  proto::CompileRequest req;
  uint64_t in_zstd = 0;
  try { req = proto::decode_compile(f, &in_zstd); }
  catch (const std::exception& e) { reject_request(c, f, "bad-compile-frame", e); return; }
  res.id = req.id; res.in_raw = req.preprocessed.size(); res.in_zstd = in_zstd;
  { std::lock_guard<std::mutex> g(c->jobs_mu); c->running[req.id] = 0; }
  g_slots->acquire();
  double t_start = util::now_s();
  res.queue_s = t_start - t_recv;
  std::string dir;
  try {
    bool cancelled = false;
    { std::lock_guard<std::mutex> g(c->jobs_mu); cancelled = c->cancelled.count(req.id) > 0; }
    args::Scan sc = args::scan(req.argv);
    if (cancelled) { res.status = "cancelled"; res.rc = -1; }
    else if (!sc.distributable) { res.status = "worker-error"; res.err = "opticcd: not distributable: " + sc.reason; res.rc = -1; }
    else {
      std::string ckey; bool cache_hit = false;
      // Profile data, plugins, ignore-lists etc. are not in the key, and neither is the compile directory, which
      // debug info records (DW_AT_comp_dir: here this worker's per-job scratch directory). So a classic compile with
      // debug info is not cached (the header-push key has the client's cwd under -g, cachekey::normalise_argv).
      if (g_objcache.enabled() && direct::safe_args(req.argv) && !cachekey::debug_info(req.argv)) {
        std::string cid = cachekey::compiler_id(req.argv.empty() ? "" : req.argv[0]);
        // no compiler identity, no cache; and never a preprocessed text with an assembler file
        // directive (.incbin/.include reads a file the text only names)
        if (!cid.empty() && req.preprocessed.find(".incbin") == std::string::npos && !cachekey::file_directive(req.preprocessed, sc.lang == "asm"))
        ckey = cachekey::classic_key(cid, cachekey::normalise_argv(req.argv, sc.source_index, ""), util::sha256_hex(req.preprocessed));
        res.cache_key = ckey; res.cache = "miss";
        objcache::Hit hit;
        if (!ckey.empty() && g_objcache.lookup_object(ckey, &hit)) { res.cache = "hit"; res.status = "ok"; res.rc = hit.rc; res.object = std::move(hit.object); res.out = hit.out; res.err = hit.err; cache_hit = true; }
      }
      if (!cache_hit) {
      dir = util::make_temp_dir("opticcd-");
      // input_name is client data; only a plain base name may become a file name in the job dir
      std::string stem = util::safe_file_stem(req.input_name);
      std::string ifile = dir + "/" + stem + args::preprocessed_suffix(req.lang);
      std::string ofile = dir + "/" + stem + ".o";
      util::write_file(ifile, req.preprocessed);
      auto argv = args::remote_argv(req.argv, sc, ifile, ofile);
      if (g_cfg.test_slow_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.test_slow_ms));   // hedge e2e: a slow worker
      CompileWatchdog wd;
      util::RunResult rr = util::run_process(argv, dir + "/stdout", dir + "/stderr", dir, [&](pid_t pid) {
        wd.arm(pid);
        std::lock_guard<std::mutex> g(c->jobs_mu);
        c->running[req.id] = pid;
        if (c->cancelled.count(req.id)) kill(pid, SIGKILL);
      });
      wd.disarm();
      res.rc = rr.rc; res.wall_s = rr.wall_s; res.cpu_s = rr.cpu_s;
      res.out = util::file_exists(dir + "/stdout") ? util::read_file(dir + "/stdout") : "";
      res.err = util::file_exists(dir + "/stderr") ? util::read_file(dir + "/stderr") : "";
      bool was_cancelled = false;
      { std::lock_guard<std::mutex> g(c->jobs_mu); was_cancelled = c->cancelled.count(req.id) > 0; }
      if (was_cancelled) res.status = "cancelled";
      else if (wd.fired()) { res.status = "worker-error"; res.err += timeout_note(); }
      else if (rr.rc == 0 && util::file_exists(ofile)) {
        res.status = "ok"; res.object = util::read_file(ofile);
        if (g_cfg.keep_objects) { res.obj_sha = util::sha256_hex(res.object); g_cas.put(res.obj_sha, res.object); }
        if (g_cfg.test_corrupt && !res.object.empty()) res.object.back() = static_cast<char>(res.object.back() ^ 0x5a);
        if (!ckey.empty() && g_objcache.store(ckey, res.object, "", res.out, res.err, res.rc)) res.cache = "store";
      }
      else if (rr.rc == 127 || rr.signalled) { res.status = "worker-error"; res.err += "\nopticcd: compiler failed to run or was killed (rc " + std::to_string(rr.rc) + ")"; }
      else res.status = "compile-error";
      }  // !cache_hit
    }
  } catch (const std::exception& e) {
    res.status = "worker-error"; res.err = std::string("opticcd: ") + e.what(); res.rc = -1;
  }
  g_slots->release();
  c->active--;
  if (!dir.empty()) util::remove_tree(dir);
  { std::lock_guard<std::mutex> g(c->jobs_mu); c->running.erase(req.id); c->cancelled.erase(req.id); }
  res.compile_bench_s = g_compile_bench_s.load();   // 0 until the background benchmark is done
  try { c->send(proto::encode_result(res, g_cfg.zstd_level)); } catch (const std::exception& e) { log_json({{"event", "send-failed"}, {"id", req.id}, {"error", e.what()}}); }
  g_jobs_done++;
  log_json({{"event", "job"}, {"id", req.id}, {"peer", c->peer}, {"status", res.status}, {"cache", res.cache}, {"rc", res.rc},
            {"in_raw", res.in_raw}, {"in_zstd", res.in_zstd}, {"out_raw", res.object.size()},
            {"queue_s", res.queue_s}, {"wall_s", res.wall_s}, {"cpu_s", res.cpu_s}, {"input", req.input_name}});
}

// ---- header-push ------------------------------------------------------------------
// Materialise `path` (absolute client path) under root from the store entry `sha`.
static void materialise(const std::string& root, const std::string& path, const std::string& sha) {
  std::string dst = util::under_root(root, path);
  util::mkdir_p(util::dirname(dst));
  std::error_code ec;
  std::filesystem::remove(dst, ec);
  std::filesystem::create_hard_link(g_cas.path(sha), dst, ec);
  if (ec) std::filesystem::copy_file(g_cas.path(sha), dst, std::filesystem::copy_options::overwrite_existing);
}

// Store requested content only if it hashes to the sha it is filed under. The client hashes a file when it builds
// the closure and reads it again when it answers NEED: an edit in between would otherwise put the NEW bytes under the
// OLD sha, and the job (and this worker's compile cache) would use content its key does not describe. A rejected blob
// leaves the file missing: the compile fails on it and the client falls back to a local compile.
static bool cas_put_verified(const std::string& sha, const std::string& data) {
  if (util::sha256_hex(data) != sha) { g_blobs_rejected++; log_json({{"event", "blob-rejected"}, {"sha", sha}, {"bytes", data.size()}}); return false; }
  g_cas.put(sha, data); return true;
}

// The compile-cache keys of a header-push TU: manifest key M from compiler id + normalised
// argv + raw source sha, and the client's closure view (relative path -> sha of the shipped closure + source).
// Shared by COMPILE_HP and CACHE_PUT so that an uploaded result is filed exactly where the worker's own compile
// of the same request would have filed it.
static std::string hp_cache_keys(const std::vector<std::string>& argv, const args::Scan& sc, const std::string& cwd, const std::string& src_path,
                                 const std::string& src_sha, const std::vector<proto::FileRef>& deps, std::unordered_map<std::string, std::string>* view) {
  std::string cid = cachekey::compiler_id(argv.empty() ? "" : argv[0]);
  if (cid.empty()) return {};   // no compiler identity, no key (no lookup, no store)
  auto norm = cachekey::normalise_argv(argv, sc.source_index, cwd);
  std::string src_abs0 = src_path.empty() ? closure::absolutize(sc.source, cwd) : src_path;
  (*view)[cachekey::relativise(src_abs0, cwd)] = src_sha;
  for (auto& d : deps) (*view)[cachekey::relativise(d.path, cwd)] = d.sha;
  // the system headers this worker compiles against (never shipped, omitted from the closure) enter the key as a
  // pseudo-input; an upgrade changes the fingerprint and every entry stored before it stops matching
  (*view)["@sysinc"] = sysinc_fp_for(argv.empty() ? "" : argv[0]);
  return cachekey::manifest_key(cid, norm, src_sha);
}

// The compile expanded (or may have expanded) __DATE__/__TIME__/__TIMESTAMP__: the source or a shipped /
// MISSING-delivered input uses one. Content in the CAS is verified against its sha on arrival, so the answer is memoised
// per sha; an input missing from the CAS counts as yes. (Fingerprinted system headers are not scanned: residual.)
static bool closure_uses_time_macros(const std::vector<std::string>& argv, const std::string& cwd, const std::string& dep_text,
                                     const std::string& source, const std::vector<proto::FileRef>& deps) {
  static std::mutex mu; static std::unordered_map<std::string, bool> memo;
  if (cachekey::uses_time_macros(source) || cachekey::argv_uses_time_macros(argv)) return true;   // includes -D values
  // the fingerprinted system headers (never shipped) are scanned by the tracker; only
  // a TU that may read a marked one is refused (its dependency file under -MD, else its effective search path)
  if (!g_sysinc) return true;
  const std::string cc0 = argv.empty() ? std::string() : argv[0];
  try {
    const sysinc::State st = g_sysinc->get(opticc::cachekey::resolve_compiler(cc0), true);
    if (st.content_fp.empty()) return true;
    if (!st.time_macro_files.empty()) {
      std::vector<std::string> dep_abs;
      for (auto& p : closure::parse_depfile(dep_text)) dep_abs.push_back(closure::absolutize(p, cwd));
      const std::string tin = sysinc::time_macro_input(st.time_macro_files, fp_dirs_for(cc0), argv, cwd, dep_abs,
                                                       !dep_text.empty() && cachekey::dep_lists_system_headers(argv));
      if (!tin.empty()) { g_tm_refusals++; log_json({{"event", "store-refused"}, {"why", "time macro in a system header"}, {"file", tin}}); return true; }
    }
  } catch (...) { return true; }
  for (auto& d : deps) {
    { std::lock_guard<std::mutex> g(mu); auto it = memo.find(d.sha); if (it != memo.end()) { if (it->second) return true; continue; } }
    bool t = true;
    try { t = cachekey::uses_time_macros(util::read_file(g_cas.path(d.sha))); } catch (...) { return true; }
    { std::lock_guard<std::mutex> g(mu); if (memo.size() > (1u << 20)) memo.clear(); memo[d.sha] = t; }
    if (t) return true;
  }
  return false;
}

// An assembler file directive (.incbin/.include) in the source or any shipped or
// delivered input reads a file outside the key and the dependency file: never stored (memoised per content)
static bool closure_file_directive(const std::string& source, bool assembler, const std::vector<proto::FileRef>& deps) {
  if (source.find(".incbin") != std::string::npos || cachekey::file_directive(source, assembler)) return true;
  static std::mutex mu; static std::unordered_map<std::string, bool> memo;
  for (auto& d : deps) {
    { std::lock_guard<std::mutex> g(mu); auto it = memo.find(d.sha); if (it != memo.end()) { if (it->second) return true; continue; } }
    bool t = true;
    try { t = cachekey::file_directive(util::read_file(g_cas.path(d.sha)), assembler); } catch (...) { return true; }
    { std::lock_guard<std::mutex> g(mu); if (memo.size() > (1u << 20)) memo.clear(); memo[d.sha] = t; }
    if (t) return true;
  }
  return false;
}

// Store a successful header-push compile under M: the used closure is the dependency file's inputs (minus the source)
// mapped to the shas the client shipped; conservatively the whole shipped closure when there is no dependency file or
// it names a non-system input the client did not ship. `root` is the per-job root a worker compile ran under (its
// paths are the worker's own scratch files), or "" for an uploaded result. Returns false if nothing was stored.
static bool hp_cache_store(const std::vector<std::string>& argv_for_store, const args::Scan& sc, const std::string& cwd, const std::string& src_path, const std::string& src_sha,
                           const std::vector<proto::FileRef>& deps, const std::string& mkey, const std::unordered_map<std::string, std::string>& view,
                           const std::string& dep_text, const std::string& root, const std::string& object, const std::string& out,
                           const std::string& err, int rc) {
  const std::string argv0_for_store = argv_for_store.empty() ? std::string() : argv_for_store[0];
  cachekey::Closure used; std::unordered_map<std::string, std::string> abs2sha;
  for (auto& d : deps) abs2sha[d.path] = d.sha;
  std::string src_abs2 = src_path.empty() ? closure::absolutize(sc.source, cwd) : src_path;
  bool ok_dep = false;
  if (!dep_text.empty()) {
    const std::vector<std::string> sysd_fp = fp_dirs_for(argv0_for_store);
    auto in_fp = [&](const std::string& p) { for (auto& d : sysd_fp) if (util::path_within(p, d)) return true; return false; };
    try {
      for (auto& pth : closure::parse_depfile(dep_text)) {
        std::string ap = closure::absolutize(pth, cwd);
        if (ap == src_abs2) continue;
        auto it2 = abs2sha.find(ap);
        if (it2 != abs2sha.end()) used.emplace_back(cachekey::relativise(ap, cwd), it2->second);
        // an input the dependency file names but nobody vouched for (not shipped, not delivered in a MISSING round,
        // not a system header) means the key cannot describe what the compile read: nothing is stored. (Falling back
        // to the shipped closure would be wrong: by construction it lacks that input.)
        else if (!in_fp(ap) && (root.empty() || ap.rfind(root, 0) != 0)) return false;
        ok_dep = true;
      }
    } catch (...) { return false; }   // an unparsable dependency file: no store (no guess)
  }
  // no dependency file: the shipped closure plus every file a MISSING round delivered (`deps` carries both) is a
  // superset of the non-system inputs: the scanner over-approximates literal includes, and the compile could read a
  // file it was not shipped only through a MISSING round
  // a -MMD dependency file omits headers found through the project's own -isystem / -idirafter
  // directories; the shipped files under such a directory (outside the fingerprinted ones) enter the closure as well
  if (ok_dep && !cachekey::dep_lists_system_headers(argv_for_store)) {
    const auto xs = sysinc::explicit_system_dirs(argv_for_store, cwd);
    if (!xs.empty()) {
      const std::vector<std::string> fpd = fp_dirs_for(argv0_for_store);
      auto within = [](const std::string& p, const std::vector<std::string>& ds) { for (auto& d : ds) if (util::path_within(p, d)) return true; return false; };
      std::set<std::string> have; for (auto& u : used) have.insert(u.first);
      for (auto& d : deps) {
        if (d.path == src_abs2 || within(d.path, fpd) || !within(d.path, xs)) continue;
        const std::string rel = cachekey::relativise(d.path, cwd);
        if (have.insert(rel).second) used.emplace_back(rel, d.sha);
      }
    }
  }
  if (!ok_dep) { used.clear(); for (auto& d : deps) used.emplace_back(cachekey::relativise(d.path, cwd), d.sha); }
  used.emplace_back(cachekey::relativise(src_abs2, cwd), src_sha);
  {   // the system-include fingerprint the key was computed with must still be current (no store on a change)
    auto it = view.find("@sysinc");
    if (it == view.end() || it->second.empty()) return false;
    if (sysinc_fp_for(argv0_for_store) != it->second) return false;
    used.emplace_back("@sysinc", it->second);
  }
  std::string okey = cachekey::object_key(mkey, used, cwd);
  std::string body = dep_text; auto colon = body.find(':'); body = colon == std::string::npos ? "" : body.substr(colon + 1);
  const std::string fkey = cachekey::full_key(mkey, std::vector<std::pair<std::string, std::string>>(view.begin(), view.end()));
  try { if (!g_objcache.store(okey, object, body, out, err, rc)) return false; g_objcache.add_manifest_entry(mkey, used, okey, objcache::ObjectCache::shape_hash(view), fkey); return true; }
  catch (...) { return false; }
}

// CACHE_PUT: a client-local compile result written back into this worker's compile cache (protocol.hpp), so that
// the worker tier covers the TUs clients compiled themselves (a fresh client then finds them here). Integrity: the
// object's sha256 must match; the keys are computed HERE, from the request as a COMPILE_HP would carry it; nothing is
// stored on any doubt (non-zero rc, not distributable, entry already present). Trust: like the shared remote storage of
// ccache/Bazel, an authorised client is trusted to report its own compiler's output; --no-cache-uploads refuses.
void handle_cache_put(std::shared_ptr<Conn> c, proto::Frame f) {
  std::string outcome; uint64_t bytes = f.payload.size(); std::string input;
  try {
    if (!g_objcache.enabled()) outcome = "no-cache";
    else if (!g_cfg.accept_cache_uploads) outcome = "refused";
    else {
      proto::CachePutMsg m = proto::decode_cache_put(f);
      input = util::basename(m.src_path);
      args::Scan sc = args::scan(m.argv);
      if (!sc.distributable) outcome = "not-distributable";
      else if (!direct::safe_args(m.argv)) outcome = "uncacheable-flag";
      else if (m.rc != 0 || m.object.empty()) outcome = "no-object";
      else if (m.obj_sha.size() != 64 || util::sha256_hex(m.object) != m.obj_sha) outcome = "integrity";
      else if (m.src_sha.size() != 64 || m.cwd.empty() || m.src_path.empty()) outcome = "bad-request";
      else {
        // an upload is background work: it may wait for this compiler's first fingerprint (a compile never waits)
        if (g_sysinc) { try { (void)g_sysinc->get(opticc::cachekey::resolve_compiler(m.argv.empty() ? "" : m.argv[0]), true); } catch (...) {} }
        std::unordered_map<std::string, std::string> view;
        const std::string mkey = hp_cache_keys(m.argv, sc, m.cwd, m.src_path, m.src_sha, m.deps, &view);
        objcache::Hit hit;
        // the key names THIS worker's compiler and system headers; the object came from
        // the client's. Store only when both are identical (binary + version; content of the system include tree).
        if (m.compiler_id.empty() || m.compiler_id != cachekey::compiler_id(m.argv.empty() ? "" : m.argv[0])) outcome = "compiler-mismatch";
        else if (m.sysinc_fp.empty() || m.sysinc_fp != view["@sysinc"]) outcome = "sysinc-mismatch";
        else if (g_objcache.lookup_manifest(mkey, view, &hit)) outcome = "present";
        else outcome = hp_cache_store(m.argv, sc, m.cwd, m.src_path, m.src_sha, m.deps, mkey, view, m.depfile, "", m.object, m.out, m.err, m.rc) ? "stored" : "store-failed";
      }
    }
  } catch (const std::exception& e) { outcome = std::string("bad-frame: ") + e.what(); }
  if (outcome == "stored") { g_uploads_stored++; g_upload_bytes += bytes; } else if (outcome != "present") g_uploads_rejected++;
  log_json({{"event", "cache-put"}, {"peer", c->peer}, {"outcome", outcome}, {"input", input}, {"bytes", bytes},
            {"stored", g_uploads_stored.load()}, {"rejected", g_uploads_rejected.load()}, {"cache_objects", g_objcache.object_count()}});
  c->active--;
}

// DIRECT_PUT: a client publishes a direct manifest entry and its object (opticc/rdirect.hpp). Integrity: the
// object's sha256 must equal the published one and the entry must name it; the object is kept in the published
// namespace (rdirect::worker_object_key), never under a COMPILE_HP key. Trust as CACHE_PUT (--no-cache-uploads refuses).
void handle_direct_put(std::shared_ptr<Conn> c, proto::Frame f) {
  std::string outcome;
  try {
    if (!g_rdstore.enabled() || !g_objcache.enabled()) outcome = "no-cache";
    else if (!g_cfg.accept_cache_uploads) outcome = "refused";
    else {
      rdirect::PutMsg m = rdirect::decode_put(f);
      // the key kind travels with the PUT; only direct keys (kind "", entries with inputs) are stored
      if (!m.kind.empty() || !rdirect::entry_fits_kind(m.entry, "")) outcome = "bad-entry";
      else if (m.obj.rc != 0 || m.obj.object.empty()) outcome = "no-object";
      else if (m.obj.obj_sha.size() != 64 || util::sha256_hex(m.obj.object) != m.obj.obj_sha || m.entry.obj_sha != m.obj.obj_sha || m.entry.obj != m.obj.okey) outcome = "integrity";
      else {
        const std::string wk = rdirect::worker_object_key(m.obj.okey, m.obj.obj_sha);   // keyed by the object bytes
        if (!g_objcache.has_object(wk) && !g_objcache.store(wk, m.obj.object, m.obj.depfile_body, m.obj.out, m.obj.err, m.obj.rc)) outcome = "store-failed";
        else outcome = g_rdstore.put(m.pkey, m.entry) ? "stored" : "store-failed";
      }
    }
  } catch (const std::exception& e) { outcome = std::string("bad-frame: ") + e.what(); }
  if (outcome == "stored") g_rd_puts++; else g_rd_rejected++;
  if (outcome != "stored") log_json({{"event", "direct-put"}, {"peer", c->peer}, {"outcome", outcome}});
  c->active--;
}

// DIRECT_GET: the manifest of a portable key and, when held, the object of the requested (or the newest) entry
void handle_direct_get(std::shared_ptr<Conn> c, proto::Frame f) {
  rdirect::ReplyMsg rep;
  try {
    rdirect::GetMsg m = rdirect::decode_get(f); rep.id = m.id; g_rd_gets++;
    std::string z, enc;
    if (g_rdstore.enabled() && g_rdstore.get_stored(m.pkey, &z) && g_rdstore.get(m.pkey, &enc)) {
      std::vector<rdirect::PEntry> all;
      // only direct-key entries (kind "") are served; a mixed set (a malformed publisher) is re-encoded without the
      // others
      std::vector<rdirect::PEntry> es = (m.kind.empty() && rdirect::decode(enc, &all)) ? rdirect::filter_kind(all, "") : std::vector<rdirect::PEntry>{};
      if (!es.empty()) {
        rep.have = true; g_rd_found++;
        if (es.size() == all.size()) rep.entries_z = std::move(z);   // sent in the stored compressed form
        else rep.entries = rdirect::encode(es);
        const rdirect::PEntry* want = rdirect::select_for_get(es, m.okey);   // "okey@sha" selects by bytes
        objcache::Hit hit;
        if (want && g_objcache.lookup_object(rdirect::worker_object_key(want->obj, want->obj_sha), &hit) && hit.rc == 0) {
          rep.obj.okey = want->obj; rep.obj.obj_sha = want->obj_sha; rep.obj.object = std::move(hit.object);
          rep.obj.depfile_body = std::move(hit.depfile_body); rep.obj.out = std::move(hit.out); rep.obj.err = std::move(hit.err); rep.obj.rc = 0;
        }
      }
    }
  } catch (const std::exception& e) { log_json({{"event", "bad-direct-get"}, {"error", e.what()}}); }
  try { c->send(rdirect::encode_reply(rep)); } catch (...) {}
  c->active--;
}

void handle_compile_hp(std::shared_ptr<Conn> c, proto::Frame f) {
  double t_recv = util::now_s();
  proto::CompileResult res; res.mode = "header_push";
  proto::HpRequest req;
  uint64_t in_zstd = 0;
  try { req = proto::decode_compile_hp(f, &in_zstd); }
  catch (const std::exception& e) { reject_request(c, f, "bad-compile-hp-frame", e); return; }
  res.id = req.id; res.in_raw = req.source.size(); res.in_zstd = in_zstd;
  auto inbox = std::make_shared<Inbox>();
  { std::lock_guard<std::mutex> g(c->jobs_mu); c->running[req.id] = 0; c->inboxes[req.id] = inbox; }
  std::string root;
  std::vector<std::string> last_argv; std::string last_cwd;
  auto cancelled = [&] { std::lock_guard<std::mutex> g(c->jobs_mu); return c->cancelled.count(req.id) > 0; };
  try {
    args::Scan sc = args::scan(req.argv);
    if (!sc.distributable) throw std::runtime_error("not distributable: " + sc.reason);
    // the toolchain identity this worker compiles (and its cache serves) with; the client caches the
    // result only when both equal its own compiler's
    { const std::string cc0 = req.argv.empty() ? std::string() : req.argv[0];
      res.compiler_id = cachekey::compiler_id(cc0); res.sysinc_fp = sysinc_fp_for(cc0); }
    // 0. compile cache: manifest key from compiler id + normalised argv + source sha,
    //    matched against the client's closure view; a hit returns the stored object without a slot or a compile.
    std::string mkey, src_sha_pre; std::unordered_map<std::string, std::string> view;
    if (g_objcache.enabled()) {
      src_sha_pre = util::sha256_hex(req.source);
      mkey = hp_cache_keys(req.argv, sc, req.cwd, req.src_path, src_sha_pre, req.deps, &view);
      // report the full content key, so the client's affinity index names exactly this closure content
      res.cache_key = cachekey::full_key(mkey, std::vector<std::pair<std::string, std::string>>(view.begin(), view.end())); res.cache = "miss";
      objcache::Hit hit;
      if (!mkey.empty() && g_objcache.lookup_manifest(mkey, view, &hit)) {
        res.cache = "hit"; res.status = "ok"; res.rc = hit.rc; res.object = std::move(hit.object); res.out = hit.out; res.err = hit.err;
        if (sc.has_md) { std::string tgt = sc.dep_target.empty() ? sc.output : sc.dep_target; res.depfile = tgt + ":" + hit.depfile_body; }
        if (g_cfg.keep_objects) { res.obj_sha = util::sha256_hex(res.object); g_cas.put(res.obj_sha, res.object); }
        res.queue_s = util::now_s() - t_recv;
        c->active--;
        { std::lock_guard<std::mutex> g(c->jobs_mu); c->running.erase(req.id); c->cancelled.erase(req.id); c->inboxes.erase(req.id); }
        res.compile_bench_s = g_compile_bench_s.load();   // 0 until the background benchmark is done
        try { c->send(proto::encode_result(res, g_cfg.zstd_level)); } catch (const std::exception& e) { log_json({{"event", "send-failed"}, {"id", req.id}, {"error", e.what()}}); }
        g_jobs_done++;
        log_json({{"event", "job"}, {"mode", "header_push"}, {"id", req.id}, {"peer", c->peer}, {"status", "ok"}, {"cache", "hit"}, {"rc", res.rc},
                  {"out_raw", res.object.size()}, {"queue_s", res.queue_s}, {"input", util::basename(req.src_path)}, {"cache_objects", g_objcache.object_count()}});
        return;
      }
    }
    // 1. ask for store misses among the declared closure (before taking a slot: network time is not compile time)
    std::vector<std::string> need;
    for (auto& d : req.deps) if (!g_cas.has(d.sha)) need.push_back(d.sha);
    if (!need.empty()) {
      c->send(proto::encode_need(proto::NeedMsg{req.id, need}));
      proto::BlobsMsg b;
      if (!inbox->wait(&b, 120.0)) throw std::runtime_error("timeout waiting for BLOBS");
      for (auto& [sha, data] : b.blobs) { if (cas_put_verified(sha, data)) res.miss_bytes += data.size(); }
      for (auto& fr : b.files) req.deps.push_back(fr);
    }
    g_slots->acquire();
    double t_start = util::now_s();
    res.queue_s = t_start - t_recv;
    if (cancelled()) { res.status = "cancelled"; res.rc = -1; }
    else {
      root = util::make_temp_dir("opticcd-hp-");
      std::string src_sha = util::sha256_hex(req.source);
      g_cas.put(src_sha, req.source);
      std::string src_abs = req.src_path.empty() ? closure::absolutize(sc.source, req.cwd) : req.src_path;
      materialise(root, src_abs, src_sha);
      for (auto& d : req.deps) if (g_cas.has(d.sha)) materialise(root, d.path, d.sha);
      // the client cwd may be "/" itself (then the job root); every other client path goes through under_root
      std::string cwd = util::lexically_normal_abs(req.cwd.empty() ? "/" : req.cwd) == "/" ? root : util::under_root(root, req.cwd);
      util::mkdir_p(cwd);
      std::string ofile = root + "/__opticc_out.o", depfile = root + "/__opticc_dep.d";
      std::string dep_target = sc.dep_target.empty() ? sc.output : sc.dep_target;
      auto argv = args::hp_remote_argv(req.argv, sc, root, ofile, depfile, dep_target);
      bool forced_dep = false;
      if (g_objcache.enabled() && !sc.has_md) { argv.push_back("-MMD"); argv.push_back("-MF"); argv.push_back(depfile); argv.push_back("-MT"); argv.push_back("x.o"); forced_dep = true; }
      last_argv = argv; last_cwd = cwd;
      // kbuild compiles with -Werror=missing-include-dirs: every -I/-iquote/-isystem/-idirafter directory must exist in
      // the per-job root even when the closure brought no file from it
      {
        static const char* dflags[] = {"-I", "-iquote", "-isystem", "-idirafter"};
        for (size_t i = 1; i < argv.size(); ++i) {
          for (const char* df : dflags) {
            std::string dflag(df), dir;
            if (argv[i] == dflag && i + 1 < argv.size()) dir = argv[i + 1];
            else if (argv[i].rfind(dflag, 0) == 0 && argv[i].size() > dflag.size()) dir = argv[i].substr(dflag.size());
            if (dir.empty()) continue;
            if (dir[0] != '/') dir = cwd + "/" + dir;          // relative to the job's cwd inside the root
            // never touch paths outside the per-job root: decided on the lexically normalised path (a prefix test on
            // the raw string would accept "<root>/../../etc")
            dir = util::lexically_normal_abs(dir);
            const std::string nroot = util::lexically_normal_abs(root);
            if (util::path_within(dir, nroot) && dir != nroot) util::mkdir_p(dir);
            break;
          }
        }
      }
      int round = 0;
      std::vector<proto::FileRef> missing_files;   // inputs delivered by MISSING rounds
      if (!g_cfg.test_missing_name.empty()) {      // tests only: behave like a compromised worker once per job
        c->send(proto::encode_missing(proto::MissingMsg{req.id, g_cfg.test_missing_name, g_cfg.test_missing_includer, 0}));
        proto::BlobsMsg pb;
        g_slots->release(); bool got = inbox->wait(&pb, 60.0); g_slots->acquire();
        uint64_t pbytes = 0; for (auto& bl : pb.blobs) pbytes += bl.second.size();
        json paths = json::array(); for (auto& fr : pb.files) paths.push_back(fr.path);
        log_json({{"event", "test-missing-probe"}, {"id", req.id}, {"name", g_cfg.test_missing_name}, {"answered", got},
                  {"files", pb.files.size()}, {"paths", paths}, {"bytes", pbytes}});   // the content is never used
      }
      for (;;) {
        if (g_cfg.test_slow_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.test_slow_ms));
        CompileWatchdog wd;
        util::RunResult rr = util::run_process(argv, root + "/stdout", root + "/stderr", cwd, [&](pid_t pid) {
          wd.arm(pid);
          std::lock_guard<std::mutex> g(c->jobs_mu);
          c->running[req.id] = pid;
          if (c->cancelled.count(req.id)) kill(pid, SIGKILL);
        });
        wd.disarm();
        res.rc = rr.rc; res.wall_s += rr.wall_s; res.cpu_s += rr.cpu_s;
        res.out = util::file_exists(root + "/stdout") ? util::read_file(root + "/stdout") : "";
        res.err = util::file_exists(root + "/stderr") ? util::read_file(root + "/stderr") : "";
        if (cancelled()) { res.status = "cancelled"; break; }
        if (wd.fired()) { res.status = "worker-error"; res.err += timeout_note(); break; }
        if (rr.rc == 0 && util::file_exists(ofile)) {
          res.status = "ok"; res.object = util::read_file(ofile);
          if (g_cfg.keep_objects) { res.obj_sha = util::sha256_hex(res.object); g_cas.put(res.obj_sha, res.object); }
          if (g_cfg.test_corrupt && !res.object.empty()) res.object.back() = static_cast<char>(res.object.back() ^ 0x5a);
          std::string dep_text = util::file_exists(depfile) ? util::replace_all(util::read_file(depfile), root, "") : "";
          if (sc.has_md && !dep_text.empty()) res.depfile = dep_text;
          if (g_objcache.enabled() && !mkey.empty() && direct::safe_args(req.argv)) {   // no store for flags reading unkeyed files
            // file the result under the closure the compile ACTUALLY had (shipped + MISSING-delivered
            // inputs), in both the entry's closure and its view (shape, full key): a later client whose view holds
            // those files (e.g. from its .d) can hit, one whose view lacks them cannot
            std::vector<proto::FileRef> deps2 = req.deps; auto view2 = view;
            for (auto& fr : missing_files) { deps2.push_back(fr); view2[cachekey::relativise(fr.path, req.cwd)] = fr.sha; }
            if (!closure_uses_time_macros(req.argv, req.cwd, dep_text, req.source, deps2) &&
                !closure_file_directive(req.source, sc.lang == "asm", deps2) &&
                hp_cache_store(req.argv, sc, req.cwd, req.src_path, src_sha_pre, deps2, mkey, view2, dep_text, root, res.object, res.out, res.err, res.rc))
              res.cache = "store";
          }
          (void)forced_dep;
          break;
        }
        if (rr.rc == 127 || rr.signalled) { res.status = "worker-error"; res.err += "\nopticcd: compiler failed to run or was killed (rc " + std::to_string(rr.rc) + ")"; break; }
        // missing include? ask the client for it (bounded rounds)
        std::string name, includer;
        if (!closure::parse_missing_include(res.err, &name, &includer) || round >= g_cfg.hp_max_rounds) { res.status = "compile-error"; break; }
        ++round; res.rounds = round;
        if (includer.rfind(root, 0) == 0) includer = includer.substr(root.size());
        else if (!includer.empty() && includer[0] != '/') includer = closure::absolutize(includer, req.cwd);
        c->send(proto::encode_missing(proto::MissingMsg{req.id, name, includer, round}));
        proto::BlobsMsg b;
        g_slots->release();                       // the round trip is not compile time
        bool got = inbox->wait(&b, 120.0);
        g_slots->acquire();
        if (!got) { res.status = "worker-error"; res.err += "\nopticcd: timeout waiting for missing include"; break; }
        if (b.files.empty()) { res.status = "compile-error"; break; }   // client could not resolve it: genuine error
        for (auto& [sha, data] : b.blobs) { if (cas_put_verified(sha, data)) res.miss_bytes += data.size(); }
        // a MISSING-round file is an input of this compile like a shipped one: it joins the closure the
        // result is filed under (hp_cache_store below), with the sha it was verified against
        for (auto& fr : b.files) if (g_cas.has(fr.sha)) { materialise(root, fr.path, fr.sha); missing_files.push_back(fr); }
      }
      // diagnostics must read as if compiled in place
      res.out = util::replace_all(res.out, root, ""); res.err = util::replace_all(res.err, root, "");
    }
    g_slots->release();
  } catch (const std::exception& e) {
    res.status = "worker-error"; res.err = std::string("opticcd: ") + e.what(); res.rc = -1;
    if (res.queue_s > 0 || !root.empty()) g_slots->release();
  }
  c->active--;
  // OPTICC_KEEP_FAILED_ROOT=1 keeps the per-job root of a failed compile for post-mortem (debug aid)
  if (!root.empty()) {
    static const bool keep_failed = std::getenv("OPTICC_KEEP_FAILED_ROOT") && std::string(std::getenv("OPTICC_KEEP_FAILED_ROOT")) == "1";
    if (keep_failed && res.rc != 0) {
      std::fprintf(stderr, "{\"event\":\"kept-root\",\"root\":\"%s\",\"input\":\"%s\"}\n", root.c_str(), req.src_path.c_str());
      std::string a; for (auto& x : last_argv) { a += x; a += '\n'; }
      util::write_file(root + "/__opticc_argv.txt", a); util::write_file(root + "/__opticc_cwd.txt", last_cwd);
    }
    else util::remove_tree(root);
  }
  { std::lock_guard<std::mutex> g(c->jobs_mu); c->running.erase(req.id); c->cancelled.erase(req.id); c->inboxes.erase(req.id); }
  res.compile_bench_s = g_compile_bench_s.load();   // 0 until the background benchmark is done
  try { c->send(proto::encode_result(res, g_cfg.zstd_level)); } catch (const std::exception& e) { log_json({{"event", "send-failed"}, {"id", req.id}, {"error", e.what()}}); }
  g_jobs_done++;
  log_json({{"event", "job"}, {"mode", "header_push"}, {"id", req.id}, {"peer", c->peer}, {"status", res.status}, {"cache", res.cache}, {"rc", res.rc},
            {"in_raw", res.in_raw}, {"in_zstd", res.in_zstd}, {"miss_bytes", res.miss_bytes}, {"rounds", res.rounds},
            {"out_raw", res.object.size()}, {"queue_s", res.queue_s}, {"wall_s", res.wall_s}, {"cpu_s", res.cpu_s},
            {"input", util::basename(req.src_path)}, {"cas_entries", g_cas.count()}, {"cas_bytes", g_cas.total()}, {"prefetch_bytes", g_prefetch_bytes.load()}, {"prefetch_rejected", g_prefetch_rejected.load()}});
}

// Re-read on every connection: cheap, and edits take effect without a restart.
std::vector<noise::Key> load_authorized_clients() {
  std::vector<noise::Key> out;
  if (!util::file_exists(g_cfg.authorized_clients)) return out;
  std::istringstream is(util::read_file(g_cfg.authorized_clients)); std::string line;
  while (std::getline(is, line)) {
    auto hash = line.find('#'); if (hash != std::string::npos) line = line.substr(0, hash);
    std::istringstream ls(line); std::string tok; if (!(ls >> tok)) continue;
    noise::Key k; if (noise::parse_pub(tok, &k)) out.push_back(k);
    else log_json({{"event", "bad-authorized-key"}, {"line", line}});
  }
  return out;
}

// Channel selection by the first bytes (no negotiation, no downgrade). Returns false to drop the connection.

// --- linker domains -------------------------------------------------------------------------------------------
// The coordinator names the domain's objects by sha (kept here by --keep-objects after each compile, or fetched
// now via NEED/BLOBS). Phase "manifest" returns the condensed manifest; phase "prelink" expands the live set from
// the coordinator's live entries and returns the prelinked domain object. Object order = request order = link order.
void handle_prelink(std::shared_ptr<Conn> c, proto::Frame f) {
  proto::PrelinkReply rep;
  proto::PrelinkRequest req;
  double t0 = util::now_s();
  try {
    req = proto::decode_prelink(f);
    rep.id = req.id; rep.domain = req.domain; rep.phase = req.phase;
    auto inbox = std::make_shared<Inbox>();
    { std::lock_guard<std::mutex> g(c->jobs_mu); c->inboxes[req.id] = inbox; }
    std::vector<std::string> need;
    for (auto& o : req.objects) if (!g_cas.has(o.sha)) need.push_back(o.sha);
    if (!need.empty()) {
      c->send(proto::encode_need(proto::NeedMsg{req.id, need}));
      proto::BlobsMsg b;
      if (!inbox->wait(&b, 600.0)) throw std::runtime_error("timeout waiting for objects");
      for (auto& [sha, data] : b.blobs) cas_put_verified(sha, data);
      for (auto& o : req.objects) if (!g_cas.has(o.sha)) throw std::runtime_error("object still missing after BLOBS: " + o.path);
    }
    { std::lock_guard<std::mutex> g(c->jobs_mu); c->inboxes.erase(req.id); }
    std::vector<std::string> paths; for (auto& o : req.objects) paths.push_back(g_cas.path(o.sha));
    elf::ObjectSet set = elf::ObjectSet::load(paths);
    for (size_t i = 0; i < set.objs.size(); ++i) set.objs[i].path = req.objects[i].path;   // manifests label with client paths
    elf::Roots roots = elf::roots_from(req.roots);
    if (req.phase == "manifest") {
      elf::DomainManifest m = elf::condense_domain(set, roots, req.domain);
      rep.manifest = elf::domain_manifest_wire(m);
      elf::SectionGraph g = elf::build_section_graph(set);
      rep.stats = {{"arc_count", m.arc_count()}, {"entries", m.entries.size()}, {"exports", m.exports.size()},
                   {"undefined", g.globals.undefined.size()}, {"objects", set.objs.size()}};
      rep.status = "ok";
    } else {
      elf::PrelinkOptions po;
      po.gc = req.options.value("gc", true); po.apply_pcrel = req.options.value("apply_pcrel", true);
      po.keep_function_sections = req.options.value("keep_function_sections", false);
      po.fold_data = req.options.value("fold_data", false); po.static_link = req.options.value("static_link", true);
      po.strip_locals = req.options.value("strip_locals", false);
      for (auto& e : roots.entry_symbols) po.entry_symbols.insert(e);
      if (req.options.contains("exports")) { po.have_exports = true; for (auto& e : req.options["exports"]) po.exports.insert(e.get<std::string>()); }
      po.origin_tag = "domain-" + std::to_string(req.domain);
      std::set<elf::SecRef> live;
      if (po.gc) {
        std::set<elf::SecRef> entries = elf::secrefs_from(req.live);
        elf::Liveness L = elf::expand_domain(set, roots, entries);
        live = L.live;
      }
      elf::PrelinkResult r = elf::prelink(set, po.gc ? &live : nullptr, po);
      auto bytes = elf::write_object(r.out);
      rep.object.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      rep.manifest = r.manifest; rep.stats = r.stats.json(); rep.status = "ok";
    }
  } catch (const std::exception& e) {
    rep.status = "error"; rep.error = e.what();
    { std::lock_guard<std::mutex> g(c->jobs_mu); c->inboxes.erase(req.id); }
  }
  rep.wall_s = util::now_s() - t0; rep.cpu_s = rep.wall_s;   // single-threaded CPU work
  log_json({{"event", "prelink"}, {"id", req.id}, {"domain", req.domain}, {"phase", req.phase}, {"status", rep.status},
            {"error", rep.error}, {"objects", req.objects.size()}, {"out_bytes", rep.object.size()}, {"wall_s", rep.wall_s}, {"peer", c->peer}});
  try { c->send(proto::encode_prelink_reply(rep, g_cfg.zstd_level)); } catch (...) {}
  c->active--;
}

bool establish_channel(const std::shared_ptr<Conn>& c) {
  char b2[2]; c->sock.recv_all(b2, 2);
  if (b2[0] == 'O' && b2[1] == 'P') {
    std::string line(b2, 2);
    while (line.size() < 32) { char ch; c->sock.recv_all(&ch, 1); line.push_back(ch); if (ch == '\n') break; }
    if (line == noise::kPubkeyQuery) {   // TOFU key discovery: public information, answered to anyone in --allow
      c->sock.send_str(g_cfg.host_key.pub_b64() + "\n");
      log_json({{"event", "pubkey-query"}, {"peer", c->peer}});
      return false;
    }
    if (line != noise::kPlainMagic) { log_json({{"event", "reject"}, {"peer", c->peer}, {"why", "unknown preamble"}}); return false; }
    if (!g_cfg.accept_plain) {
      char no = 2; c->sock.send_all(&no, 1);
      log_json({{"event", "reject"}, {"peer", c->peer}, {"why", "plain connection but --accept plain not given"}});
      return false;
    }
    if (g_cfg.have_psk) {
      std::string ch(32, '\0'); randombytes_buf(ch.data(), 32);
      char yes = 1; c->sock.send_all(&yes, 1); c->sock.send_all(ch.data(), 32);
      std::string tag(32, '\0'); c->sock.recv_all(tag.data(), 32);
      if (!noise::psk_verify(g_cfg.psk, ch, tag)) { log_json({{"event", "reject"}, {"peer", c->peer}, {"why", "psk mismatch"}}); return false; }
    } else { char no = 0; c->sock.send_all(&no, 1); }
    c->sock.set_channel(std::make_unique<net::PlainChannel>());
    log_json({{"event", "channel"}, {"peer", c->peer}, {"kind", "plain"}, {"psk", g_cfg.have_psk}});
    return true;
  }
  if (!g_cfg.accept_noise) { log_json({{"event", "reject"}, {"peer", c->peer}, {"why", "noise handshake but --accept plain only"}}); return false; }
  auto allowed = load_authorized_clients();
  noise::Key client_key{};
  try {
    c->sock.set_channel(noise::respond(c->sock, g_cfg.host_key, std::string(b2, 2),
        [&](const noise::Key& k) { for (auto& a : allowed) if (a == k) return true; return false; }, &client_key));
  } catch (const std::exception& e) {
    log_json({{"event", "reject"}, {"peer", c->peer}, {"why", e.what()}, {"client_fingerprint", noise::fingerprint(client_key)},
              {"hint", allowed.empty() ? "authorized_clients is empty or missing: " + g_cfg.authorized_clients : ""}});
    return false;
  }
  log_json({{"event", "channel"}, {"peer", c->peer}, {"kind", "noise"}, {"client_fingerprint", noise::fingerprint(client_key)}});
  return true;
}

constexpr double kHandshakeTimeoutS = 30;

void serve(std::shared_ptr<Conn> c) {
  std::vector<std::thread> workers;
  try {
    // an unauthenticated peer that connects and goes silent may hold its thread for at most
    // kHandshakeTimeoutS; cleared once HELLO is accepted (an authenticated connection may idle between builds)
    c->sock.set_recv_timeout(kHandshakeTimeoutS);
    if (!establish_channel(c)) return;
    c->sock.set_max_frame(net::Socket::kPreAuthFrame);   // no large read before the HELLO token is checked
    proto::Frame h = c->sock.read_frame();
    if (h.type != proto::Type::Hello) { log_json({{"event", "reject"}, {"peer", c->peer}, {"why", "no HELLO"}}); return; }
    json hello = json::parse(h.payload);
    if (!g_cfg.token.empty() && hello.value("token", "") != g_cfg.token) {
      c->send({proto::Type::HelloAck, json{{"error", "bad token"}}.dump()});
      log_json({{"event", "reject"}, {"peer", c->peer}, {"why", "bad token"}});
      return;
    }
    json ack{{"cores", util::online_cores()}, {"jobs", g_cfg.jobs}, {"mem_bytes", util::total_memory_bytes()},
             {"bench_s", util::cpu_bench_s()}, {"compile_bench_s", g_compile_bench_s.load()}, {"version", proto::kVersion}, {"hostname", util::hostname()},
             {"channel", c->sock.encrypted() ? "noise" : "plain"}, {"fingerprint", noise::fingerprint(g_cfg.host_key.pub)},
             {"cas_entries", g_cas.count()}};   // 0 = empty store: the coordinator's held-set for this node is exact
    // which objects this worker's cache holds (Bloom filter of full content keys; ~1.25 bytes per key), so a
    // coordinator without history can route each job to the worker that holds its object. Full content keys rather
    // than manifest keys, so that a header edit does not route every job to the old holder.
    if (g_objcache.enabled()) ack["objcache_bloom"] = bloom::Filter::build(g_objcache.full_keys()).to_json();
    if (g_rdstore.enabled()) { ack["rdirect"] = 1; ack["rdirect_bloom"] = bloom::Filter::build(g_rdstore.keys()).to_json(); }
    c->send({proto::Type::HelloAck, ack.dump()});
    c->sock.set_max_frame(net::Socket::default_max_frame());   // authenticated: normal frame limit
    c->sock.set_recv_timeout(0);
    log_json({{"event", "hello"}, {"peer", c->peer}, {"client", hello.value("client", "")}});
    bool bye = false;
    while (!bye) {
      proto::Frame f = c->sock.read_frame();
      switch (f.type) {
        case proto::Type::Ping: c->send({proto::Type::Pong, {}}); break;
        case proto::Type::CacheQuery: {   // answered from the in-memory index: no disk, no slot
          proto::CacheQuery q = proto::decode_cache_query(f); proto::CacheHave have; have.have.reserve(q.keys.size());
          for (auto& k : q.keys) have.have.push_back(g_objcache.enabled() && (g_objcache.has_manifest(k) || g_objcache.has_object(k)));
          c->send(proto::encode_cache_have(have)); break;
        }
        case proto::Type::Bw: {
          json hdr; proto::unpack(f.payload, &hdr, nullptr);
          size_t down = std::min<size_t>(hdr.value("down_bytes", 0ULL), size_t(64) << 20);   // probes use 4 MiB
          c->send({proto::Type::BwAck, proto::pack(json{{"up_bytes", f.payload.size()}}, std::string(down, '\0'))});
          break;
        }
        // `active` is incremented here, not in the handler, so the reaper below never joins a
        // thread that has not started yet (a header-push job waits for BLOBS that only this loop can read)
        case proto::Type::Compile: c->active++; workers.emplace_back(handle_compile, c, std::move(f)); break;
        case proto::Type::CompileHp: c->active++; workers.emplace_back(handle_compile_hp, c, std::move(f)); break;
        case proto::Type::Prelink: c->active++; workers.emplace_back(handle_prelink, c, std::move(f)); break;
        case proto::Type::CachePut: c->active++; workers.emplace_back(handle_cache_put, c, std::move(f)); break;
        case proto::Type::DirectPut: c->active++; workers.emplace_back(handle_direct_put, c, std::move(f)); break;
        case proto::Type::DirectGet: c->active++; workers.emplace_back(handle_direct_get, c, std::move(f)); break;
        case proto::Type::Prefetch: {   // store inline (frames are small), so a following COMPILE_HP sees it
          try {
            proto::BlobsMsg b = proto::decode_prefetch(f);
            for (auto& [sha, data] : b.blobs) {
              if (g_cas.has(sha)) continue;
              if (util::sha256_hex(data) != sha) { g_prefetch_rejected++; continue; }   // never store unverified content
              g_cas.put(sha, data); g_prefetch_bytes += data.size();
            }
          } catch (const std::exception& e) { log_json({{"event", "bad-prefetch"}, {"error", e.what()}}); }
          break;
        }
        case proto::Type::Blobs: {
          try {
            proto::BlobsMsg b = proto::decode_blobs(f);
            std::shared_ptr<Inbox> ib;
            { std::lock_guard<std::mutex> g(c->jobs_mu); auto it = c->inboxes.find(b.id); if (it != c->inboxes.end()) ib = it->second; }
            if (ib) { std::lock_guard<std::mutex> l(ib->m); ib->msgs.push_back(std::move(b)); ib->cv.notify_all(); }
          } catch (const std::exception& e) { log_json({{"event", "bad-blobs"}, {"error", e.what()}}); }
          break;
        }
        case proto::Type::Cancel: {
          int64_t id = json::parse(f.payload).value("id", int64_t(-1));
          std::lock_guard<std::mutex> g(c->jobs_mu);
          if (c->running.count(id)) { c->cancelled[id] = true; pid_t p = c->running[id]; if (p > 0) kill(p, SIGKILL); }
          break;
        }
        case proto::Type::Bye: bye = true; break;
        default: log_json({{"event", "unexpected-frame"}, {"type", static_cast<int>(f.type)}}); break;
      }
      // reap finished threads opportunistically
      for (auto it = workers.begin(); it != workers.end();) { if (it->joinable() && c->active.load() == 0) { it->join(); it = workers.erase(it); } else ++it; }
    }
  } catch (const std::exception& e) {
    log_json({{"event", "conn-closed"}, {"peer", c->peer}, {"why", e.what()}});
  }
  // Connection gone: wake header-push jobs waiting for blobs, cancel running compiles, then join
  // every handler thread (a joinable std::thread destructor would terminate the whole daemon).
  {
    std::lock_guard<std::mutex> g(c->jobs_mu);
    for (auto& [id, ib] : c->inboxes) { std::lock_guard<std::mutex> l(ib->m); ib->closed = true; ib->cv.notify_all(); }
    for (auto& [id, pid] : c->running) { c->cancelled[id] = true; if (pid > 0) kill(pid, SIGKILL); }
  }
  for (auto& t : workers) if (t.joinable()) t.join();
}

void usage() {
  std::fprintf(stderr, "  compile cache: --object-cache DIR [--object-cache-gb N]  (content-addressed; hits skip the compile)\n");
  std::fprintf(stderr, "usage: opticcd [--listen IP:PORT] [--allow CIDR[,CIDR]] [--jobs N] [--tmp DIR] [--token T] [--zstd L] [--cas-gb G]\n"
                       "               [--key-dir DIR] [--authorized-clients FILE] [--accept noise|plain|both] [--psk FILE] [--exit-with-parent] [-v]\n"
                       "  key-dir defaults to /etc/opticc when writable, else ~/.config/opticc; host.key is created on first start.\n");
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  g_cfg.token = util::getenv_or("OPTICC_TOKEN", "");
  std::string allow = "127.0.0.1/32";
  bool exit_with_parent = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); exit(2); } return argv[++i]; };
    if (a == "--listen") { std::string v = next("--listen"); auto c = v.rfind(':'); if (c == std::string::npos) { usage(); return 2; } g_cfg.bind_ip = v.substr(0, c); g_cfg.port = std::atoi(v.c_str() + c + 1); }
    else if (a == "--allow") allow = next("--allow");
    else if (a == "--jobs") g_cfg.jobs = std::max(1, std::atoi(next("--jobs").c_str()));
    else if (a == "--keep-objects") g_cfg.keep_objects = true;
    else if (a == "--no-cache-uploads") g_cfg.accept_cache_uploads = false;
    else if (a == "--object-cache") g_cfg.object_cache_dir = next("--object-cache");
    else if (a == "--object-cache-gb") g_cfg.object_cache_cap = static_cast<uint64_t>(std::atof(next("--object-cache-gb").c_str()) * 1073741824.0);
    else if (a == "--tmp") g_cfg.tmp = next("--tmp");
    else if (a == "--token") g_cfg.token = next("--token");
    else if (a == "--zstd") g_cfg.zstd_level = std::atoi(next("--zstd").c_str());
    else if (a == "--cas-gb") g_cfg.cas_cap_bytes = static_cast<uint64_t>(std::atof(next("--cas-gb").c_str()) * 1073741824.0);
    else if (a == "--key-dir") g_cfg.key_dir = next("--key-dir");
    else if (a == "--authorized-clients") g_cfg.authorized_clients = next("--authorized-clients");
    else if (a == "--accept") { std::string v = next("--accept"); g_cfg.accept_noise = v == "noise" || v == "both"; g_cfg.accept_plain = v == "plain" || v == "both"; if (!g_cfg.accept_noise && !g_cfg.accept_plain) { usage(); return 2; } }
    else if (a == "--psk") g_cfg.psk_file = next("--psk");
    else if (a == "-v") g_cfg.verbose = true;
    else if (a == "--exit-with-parent") exit_with_parent = true;
    else { usage(); return 2; }
  }
  // Test harnesses start opticcd in the background; when the harness dies without running its
  // cleanup (ctest timeout kills only the shell), the daemon must not survive as an orphan listener. --exit-with-parent
  // (or OPTICC_EXIT_WITH_PARENT=1) makes it exit as soon as its parent process is gone. Not used in deployments.
  if (exit_with_parent || util::getenv_or("OPTICC_EXIT_WITH_PARENT", "") == "1") {
    const pid_t parent = getppid();
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    if (getppid() != parent || parent == 1) _exit(0);   // the parent died before the hook was armed
    std::thread([parent] { for (;;) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); if (getppid() != parent) _exit(0); } }).detach();
  }
  g_cfg.test_corrupt = util::getenv_or("OPTICC_TEST_CORRUPT", "") == "1";
  g_cfg.test_missing_name = util::getenv_or("OPTICC_TEST_MISSING_NAME", "");
  g_cfg.test_missing_includer = util::getenv_or("OPTICC_TEST_MISSING_INCLUDER", "");
  g_cfg.test_slow_ms = std::atoi(util::getenv_or("OPTICC_TEST_SLOW_MS", "0").c_str());
  { double t = std::atof(util::getenv_or("OPTICC_COMPILE_TIMEOUT_S", "1800").c_str()); g_cfg.compile_timeout_s = t > 0 ? t : 1800; }
  if (g_cfg.key_dir.empty()) {
    g_cfg.key_dir = util::getenv_or("OPTICC_KEY_DIR", "");
    if (g_cfg.key_dir.empty()) g_cfg.key_dir = access("/etc/opticc", W_OK) == 0 ? "/etc/opticc" : util::getenv_or("HOME", "/tmp") + "/.config/opticc";
  }
  if (g_cfg.authorized_clients.empty()) g_cfg.authorized_clients = g_cfg.key_dir + "/authorized_clients";
  try {
    bool created = noise::load_or_create_keypair(g_cfg.key_dir, "host", &g_cfg.host_key);
    std::fprintf(stderr, "opticcd: host key %s %s (%s)\n", noise::fingerprint(g_cfg.host_key.pub).c_str(), created ? "created" : "loaded", g_cfg.key_dir.c_str());
  } catch (const std::exception& e) { std::fprintf(stderr, "opticcd: host key: %s\n", e.what()); return 1; }
  if (!g_cfg.psk_file.empty()) {
    try { g_cfg.have_psk = noise::load_psk(g_cfg.psk_file, &g_cfg.psk); if (!g_cfg.have_psk) { std::fprintf(stderr, "opticcd: psk file missing: %s\n", g_cfg.psk_file.c_str()); return 1; } }
    catch (const std::exception& e) { std::fprintf(stderr, "opticcd: %s\n", e.what()); return 1; }
  }
  if (g_cfg.accept_noise && !util::file_exists(g_cfg.authorized_clients))
    std::fprintf(stderr, "opticcd: warning: %s does not exist; every Noise client will be refused until a client public key is added\n", g_cfg.authorized_clients.c_str());
  if (!g_cfg.tmp.empty()) setenv("TMPDIR", g_cfg.tmp.c_str(), 1);
  size_t start = 0;
  while (start <= allow.size()) {
    auto comma = allow.find(',', start); std::string one = allow.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    uint32_t ip, mask; if (!one.empty()) { if (!util::parse_cidr(one, &ip, &mask)) { std::fprintf(stderr, "bad CIDR %s\n", one.c_str()); return 2; } g_cfg.allow.emplace_back(ip, mask); }
    if (comma == std::string::npos) break; start = comma + 1;
  }
  g_slots = std::make_unique<Slots>(g_cfg.jobs);
  if (!g_cfg.object_cache_dir.empty()) { try { g_rdstore.init(g_cfg.object_cache_dir + "/rd"); } catch (...) {} }
  if (!g_cfg.object_cache_dir.empty()) { try { g_objcache.init(g_cfg.object_cache_dir, g_cfg.object_cache_cap); log_json({{"event", "object-cache"}, {"dir", g_cfg.object_cache_dir}, {"objects", g_objcache.object_count()}, {"manifests", g_objcache.manifest_count()}, {"bytes", g_objcache.total_bytes()}}); } catch (const std::exception& e) { log_json({{"event", "object-cache-error"}, {"error", e.what()}}); } }
  {   // system-include fingerprints (memo next to the cache), refreshed every 10 s, on every worker (not only with
      // --object-cache): each header-push result reports the toolchain identity it was produced with,
      // and a client caches a remote result only when that identity equals its own
    const std::string memo = g_objcache.enabled() ? g_cfg.object_cache_dir + "/sysinc" : util::getenv_or("TMPDIR", "/tmp") + "/opticcd-sysinc-" + std::to_string(getuid());
    g_sysinc = std::make_unique<opticc::sysinc::Tracker>(memo, std::atof(util::getenv_or("OPTICC_SYSINC_REFRESH_S", "10").c_str()));
    // warm-up on a background thread: the daemon listens at once; a compile waits only if it comes first
    std::thread([] { for (const char* c : {"clang", "clang++", "cc", "c++"}) (void)sysinc_fp_for(c); }).detach();
    // the compiler identity likewise (computed once; early requests wait for it)
    std::thread([] { for (const char* c : {"clang", "clang++", "cc", "c++"}) (void)cachekey::compiler_id(c); }).detach();
  }
  try { g_cas.init(util::getenv_or("TMPDIR", "/tmp") + "/opticcd-cas-" + std::to_string(getuid()), g_cfg.cas_cap_bytes); }
  catch (const std::exception& e) { std::fprintf(stderr, "opticcd: store: %s\n", e.what()); return 1; }
  net::Socket ls;
  try { ls = net::Socket::listen_tcp(g_cfg.bind_ip, g_cfg.port); } catch (const std::exception& e) { std::fprintf(stderr, "opticcd: %s\n", e.what()); return 1; }
  std::printf("opticcd listening on %s:%d jobs=%d allow=%s token=%s accept=%s%s%s psk=%s pubkey=%s\n", g_cfg.bind_ip.c_str(), ls.local_port(), g_cfg.jobs, allow.c_str(),
              g_cfg.token.empty() ? "none" : "set", g_cfg.accept_noise ? "noise" : "", g_cfg.accept_noise && g_cfg.accept_plain ? "+" : "", g_cfg.accept_plain ? "plain" : "",
              g_cfg.have_psk ? "yes" : "no", g_cfg.host_key.pub_b64().c_str());
  std::fflush(stdout);
  // OPTICC_TEST_COMPILE_BENCH_DELAY_S (tests only): delay the benchmark so a coordinator probes before it is known
  const double bench_delay = std::atof(util::getenv_or("OPTICC_TEST_COMPILE_BENCH_DELAY_S", "0").c_str());
  std::thread([bench_delay] {
    if (bench_delay > 0) std::this_thread::sleep_for(std::chrono::duration<double>(bench_delay));
    double b = util::compile_bench_s(); g_compile_bench_s.store(b); log_json({{"event", "compile-bench"}, {"s", b}});
  }).detach();
  for (;;) {
    uint32_t peer = 0;
    net::Socket s;
    try { s = ls.accept(&peer); } catch (const std::exception& e) { log_json({{"event", "accept-error"}, {"error", e.what()}}); continue; }
    char ip[32]; std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", (peer >> 24) & 255, (peer >> 16) & 255, (peer >> 8) & 255, peer & 255);
    if (!util::cidr_match(peer, g_cfg.allow)) { log_json({{"event", "reject"}, {"peer", ip}, {"why", "not in --allow"}}); continue; }
    auto c = std::make_shared<Conn>(); c->sock = std::move(s); c->peer = ip;
    std::thread(serve, c).detach();
  }
}
