// SPDX-License-Identifier: GPL-2.0-only
#include "coordinator.hpp"
#include "link_pipeline.hpp"

#include <signal.h>
#include <unordered_set>
#include <set>
#include <cmath>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <deque>

#include "cache_index.hpp"
#include "prefetch.hpp"
#include "opticc/argscan.hpp"
#include "opticc/bloom.hpp"
#include "opticc/rdirect.hpp"
#include "opticc/closure.hpp"
#include "opticc/servescope.hpp"
#include "opticc/direct.hpp"
#include "opticc/sysinc.hpp"
#include "opticc/incscan.hpp"
#include "opticc/model.hpp"
#include "opticc/noise.hpp"
#include "opticc/predictor.hpp"
#include "opticc/protocol.hpp"
#include "opticc/sched.hpp"
#include "opticc/speed.hpp"
#include "opticc/probecache.hpp"
#include "objcache.hpp"
#include "opticc/transport.hpp"
#include "opticc/util.hpp"

namespace opticc::coord {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::atomic<int> g_signal{0};
void on_signal(int s) { g_signal = s; }

// per-(cwd, path) memo for the client-cache hit path: absolute and cwd-relative forms, a 64-bit path hash for
// the order-independent shape, and whether the path is in a system directory (never part of keys).
struct PathEnt { std::string abs, rel; uint64_t h = 0; bool sys = false; };

struct JobRec {
  int id = 0;
  std::string src, out, lang, cwd, input_name, cc_local;
  std::vector<std::string> argv;
  args::Scan scan;
  // header-push state
  Mode mode = Mode::Classic;
  std::vector<proto::FileRef> deps;            // known closure (absolute paths), hashed
  bool closure_known = false, closure_from_mm = false;
  std::string cache_status = "off";             // RESULT cache field of the winning attempt: hit | store | miss | off
  std::string cache_key;                        // manifest key (header-push) or classic key, as computed client-side
  std::string lkey;                             // client-cache (L1) manifest key from the comment-insensitive source hash
  std::string dkey;                             // the wrapper's direct key (opticc/direct.hpp); "" = wrapper has no direct path
  std::string dsrc; std::vector<std::string> denv;   // the direct key's source hash and environment (portable key)
  std::vector<std::string> env;   // the client's direct::relevant_env() — sent with EVERY request
  std::string sysinc_sig;                       // the client's system-include stat signature when the request arrived
  std::string full_key;                         // full content key (cachekey::full_key) once the closure is hashed
  int affinity = 0;                             // 0 none, 1 from the result index (certain), 2 from a worker Bloom filter
  std::vector<int> aff_nodes;                   // the holders named by that affinity (guarded by Coordinator::mu_)
  double t_start_wall = 0;                      // wall clock when the request arrived (racy-input guard for direct entries)
  // wall clock just before a LOCAL decision was sent: the client's compiler reads its inputs only after it, so
  // the racy guard of a locally compiled result can use this later time (0 = not a local compile: use t_start_wall)
  double t_go_wall = 0;
  std::string closure_source;                  // "depfile" | "kbuild-cmd" | "scan" | "scan+computed" (MISSING covers it) | "scan+mm" (OPTICC_SCAN_MM=1) | "mm" | ""
  int scan_files = 0;                          // files the include scanner visited for this TU
  double scan_s = 0;                           // q_j: closure lookup + hashing (+ -MM on first miss)
  uint64_t shipped_bytes = 0, miss_bytes = 0; int rounds = 0;
  uint64_t piggy_bytes = 0;                    // headers pushed in a PREFETCH right before this job's COMPILE_HP
  int deps_prefetched = 0;                     // closure files the node already held (background prefetch) at dispatch
  // decision; under late binding `decided` with node == kUnbound means "remote, node chosen later"
  // and `bound` is set when the scheduler picks the node after the client-side work.
  bool decided = false; int node = -1; bool bound = false;
  double t_request = 0, t_decided = 0, t_done = 0;
  // remote state
  std::string ifile; bool preprocessed = false;
  std::mutex m; std::condition_variable cv;
  bool finished = false; bool have_result = false; proto::CompileResult result; int result_node = -1;
  int attempts_outstanding = 0; int attempts_total = 0; int hedges = 0;
  bool redispatched = false;                    // re-bound once after its worker was lost (guarded by mu_)
  std::string last_error;
  std::vector<std::pair<int, int64_t>> attempts;   // (node, remote id)
  uint64_t raw_bytes = 0, zstd_bytes = 0, obj_bytes = 0;
  double w_hat = 0, w_meas = 0, pp_cpu = 0, pre_bytes = 0;
  bool fallback = false; std::string outcome;       // local | remote | fallback
  bool remote_compile_error = false;                 // a worker reported compile-error (re-run locally)
  bool has_include_computed = false;                 // closure has __has_include(MACRO) -> compiled locally
  std::string pp_okey;                               // object key from the preprocessed text (computed __has_include only)
  std::string pp_text_sha;                           // sha256 of that text
  std::vector<std::string> pp_deps;                  // the preprocessing's own file list (absolute, sorted)
  double t_pp_wall = 0;                              // when that preprocessing started
  bool closure_built = false;                        // build_closure already ran (client-cache probe)
  bool closure_computed = false;                     // the SCANNED closure saw a computed #include MACRO
  std::vector<const struct PathEnt*> scan_ents;      // memoised per-path info for the scanner closure (lookup fast path)
  std::vector<std::string> scan_abs;                 // the SCANNER's closure (abs, non-system, without the source) —
                                                     // independent of whether a .d/.cmd file happens to exist; cache shapes use it
  int rc = -1;
  // verify mode: a second worker compiles the same job; objects must match
  bool verify = false;
  struct VResult { int node; std::string status; int rc; std::string sha; std::string err; };
  std::vector<VResult> vresults;
  bool verify_mismatch = false;
  proto::CompileResult verify_alt; int verify_alt_node = -1; bool verify_alt_ok = false;
};

class Coordinator {
 public:
  explicit Coordinator(const Options& o) : o_(o) {}
  int run();

 private:
  // --- setup
  void setup_topology();
  void probe_workers();
  // --- client protocol
  void serve_client(net::Socket s);
  void serve_link(const net::Socket& s, const json& req);   // linker-domain pipeline
  std::vector<json> link_reports_;
  void send(const net::Socket& s, const json& j) { std::string l = j.dump(); l.push_back('\n'); s.send_str(l); }
  bool recv(const net::Socket& s, json* j) { std::string l; if (!s.recv_line(&l)) return false; *j = json::parse(l); return true; }
  // --- scheduler driving (mu_ held)
  double now() const { return util::now_s() - t0_; }
  void run_decide_locked();
  int wait_binding(const std::shared_ptr<JobRec>& r);
  int redispatch_lost(const std::shared_ptr<JobRec>& r, int node);
  void complete_locked(const std::shared_ptr<JobRec>& r, int node, double measured_w);
  void launch_attempt(const std::shared_ptr<JobRec>& r, int node, bool hedge);
  void attempt_thread(std::shared_ptr<JobRec> r, int node, int64_t rid, bool hedge);
  // --- misc
  void write_summary();
  void log(const json& j) { if (!o_.verbose) return; std::string s = j.dump(); s.push_back('\n'); fputs(s.c_str(), stderr); }

  Options o_;
  Topology topo_;
  Workload wl_;
  std::shared_ptr<CostPredictor> pred_;
  std::shared_ptr<CostPredictor> base_pred_;          // the configured predictor, below any wrapper (summary fit)
  std::shared_ptr<HistoryPredictor> hist_;            // persisted per-TU times; null = off
  std::unique_ptr<Scheduler> sched_;
  std::vector<std::unique_ptr<net::WorkerConn>> conns_;   // index = node id - 1
  std::vector<json> probes_;
  std::mutex mu_; std::condition_variable cv_;
  std::vector<std::shared_ptr<JobRec>> jobs_;
  double t0_ = 0, last_activity_ = 0;
  int inflight_ = 0, completed_ = 0;
  std::atomic<int64_t> next_rid_{1};
  // online feature estimators (cost predictor inputs)
  double pre_per_src_c_ = 25, pre_per_src_cxx_ = 80, pp_mean_ = 0.08, obj_per_pre_ = 0.05;
  double scan_mean_ = 0.005, closure_mean_ = 50e3;
  // header-push helpers
  std::string hash_file(const std::string& path, uint64_t* size);
  // comment-insensitive key hash of a file (cachekey::content_key_sha), stat-validated like hash_file
  std::string key_hash_file(const std::string& path, bool raw_only);
  // key hashes of many files: stat all without a lock, match under ONE lock, hash misses outside it
  std::vector<std::string> key_hash_batch(const std::vector<std::string>& abs, bool raw_only);
  // does the file whose key hash is `sha` use __DATE__/__TIME__/__TIMESTAMP__? Memoised per key hash
  // (a comment-only difference cannot change the answer: the scan ignores comments); unreadable counts as yes
  bool time_macros(const std::string& abs, const std::string& sha);
  bool asm_directive(const std::string& abs, const std::string& sha);   // memoised cachekey::file_directive
  std::mutex admu_; std::unordered_map<std::string, bool> adir_;
  std::mutex tmmu_; std::unordered_map<std::string, bool> tmacro_;
  // the scanner's closure only (memoised; no dependency-file read, no raw hashing) for cache lookups
  void scan_only(JobRec& r);
  // fast hit path
  std::unordered_map<std::string, std::unique_ptr<PathEnt>> pmemo_; std::shared_mutex pmu_;
  const PathEnt* path_ent_locked(const std::string& cwd, const std::string& p, const std::vector<std::string>& sysd);
  const PathEnt* path_ent(const std::string& cwd, const std::string& p, const std::vector<std::string>& sysd);   // shared lookup, exclusive insert
  // batch form — ONE shared acquisition for all lookups, ONE exclusive acquisition for the misses
  std::vector<const PathEnt*> path_ents(const std::string& cwd, const std::vector<std::string>& ps, const std::vector<std::string>& sysd);
  std::string shape_fast(const JobRec& r, const std::string& src_rel);
  // persistent hash state (stat-validated key hashes and content hashes), like ccache's inode cache
  std::string state_dir_; std::string khash_log_, rhash_log_; std::mutex statemu_;
  void load_hash_state(); void flush_hash_state();
  std::string state_buf_;   // pending records for the key-hash log (statemu_)
  void persist_khash(const std::string& path, int64_t mt, uint64_t sz, uint64_t ino, int64_t ct, const std::string& sha);
  struct KHashEnt { int64_t mtime_ns; uint64_t size, ino; int64_t ctime_ns; std::string sha; };   // ctime too: mtime can be restored
  std::unordered_map<std::string, KHashEnt> khash_cache_; std::shared_mutex khmu_;
  std::vector<std::string> sys_dirs(const std::string& cc, const std::string& lang, const std::string& cwd);
  bool shippable(const std::string& abs, const std::vector<std::string>& sysd, const JobRec& r) const;
  void build_closure(JobRec& r);                 // from an existing .d file (q_j = hashing time)
  void closure_via_mm(JobRec& r);                // first MISSING without a .d: one -MM run
  void hp_attempt(const std::shared_ptr<JobRec>& r, int node, int64_t rid, bool hedge, proto::CompileResult* res, std::string* error);
  void abandon_attempt(int node, int64_t rid);   // an attempt past OPTICC_ATTEMPT_TIMEOUT_S
  std::mutex hmu_;
  cacheidx::CacheIndex cache_idx_;               // key → workers holding the object (compile cache)
  // per node (index = node id), the Bloom filter of the worker's cache keys from its HELLO_ACK (fresh client)
  std::vector<bloom::Filter> worker_bloom_; std::mutex bloom_mu_;
  std::vector<int> bloom_holders(const std::string& key);
  bool any_worker_keys();
  std::atomic<uint64_t> redispatches_{0};                                // lost attempts re-bound remotely
  std::atomic<uint64_t> sysinc_skipped_wait_{0};                          // waits skipped (no keys anywhere)
  uint64_t bloom_affinity_jobs_ = 0;
  uint64_t missing_refused_ = 0;                 // MISSING names refused as outside the job's scope (under mu_)
  uint64_t attempt_timeouts_ = 0;                // remote attempts abandoned at OPTICC_ATTEMPT_TIMEOUT_S (under mu_)
  uint64_t aff_index_jobs_ = 0, aff_real_hits_ = 0, aff_misses_ = 0;   // affinity outcomes (summary)
  uint64_t bloom_holder_hits_ = 0, bloom_holder_outcomes_ = 0;         // Bloom-hinted jobs that ran on a holder (totals)
  // PER-WORKER learned precision of its Bloom filter, decayed (×0.995 per outcome, half-life ≈ 140
  // outcomes) and halved on a miss, reset when the worker is (re)admitted with a fresh filter; mu_ held
  HolderTrust trust_;                              // logic in libopticc (sched.hpp), unit-tested
  std::atomic<uint64_t> trust_resets_{0};
  std::atomic<uint64_t> aff_probed_{0}, aff_unready_{0};                 // full-key probes made / skipped (no fingerprint yet)
  void set_full_key_affinity(const std::shared_ptr<JobRec>& r);
  incscan::Scanner scanner_;                    // closure without -MM on clean builds
  struct HashEnt { int64_t mtime_ns; uint64_t size; uint64_t ino; int64_t ctime_ns; std::string sha; double validated_at; };   // ctime too: mtime can be restored
  std::unordered_map<std::string, HashEnt> hash_cache_;
  std::map<std::string, std::vector<std::string>> sysdirs_cache_;
  double local_bench_ = 0, local_compile_bench_ = 0;
  // nodes whose startup prior is the microbenchmark fallback (their hello ack had compile_bench_s = 0: opticcd
  // listens before its compile benchmark finishes); the first result carrying the benchmark refreshes the prior
  std::vector<char> prior_micro_;
  json prior_refreshes_ = json::array();
  void refresh_prior_locked(int node, double cbench);
  // online speed estimation: priors from the startup compile benchmark, revised from every
  // completed remote compile; node speeds in topo_ (which the scheduler reads) follow the estimate.
  std::unique_ptr<SpeedEstimator> speed_est_;
  // client-local object cache, first level (second level: the workers' caches). Same keys/determinism as the
  // worker cache (manifest key from compiler id + normalised argv + source sha; entry matches only on an identical
  // closure shape and identical used (path, sha) pairs). A repeat build is answered from local disk like ccache's
  // direct mode, never slower than it. OPTICC_LOCAL_CACHE=off disables; OPTICC_LOCAL_CACHE_DIR / _GB configure.
  objcache::ObjectCache local_cache_;
  // system-include signature of each local compiler, part of every client-cache key and of
  // the wrapper's direct key (published next to the socket as <sock>.sysinc.<id>, see publish_sysinc)
  std::unique_ptr<sysinc::Tracker> sysinc_;
  std::mutex sysinc_mu_; std::unordered_map<std::string, std::string> sysinc_published_;   // canonical compiler -> sig
  std::string sysinc_sig(const std::string& cc_local);           // "" = unavailable (then nothing is cached)
  std::string sysinc_content(const std::string& cc_local);       // content fingerprint (uploads)
  // the time-macro system header this TU may have read ("" = none; "(unknown)" if the
  // fingerprint is unavailable). Per TU: other TUs of the compiler stay cacheable. Waits on the shared sysinc_warm future.
  std::string sysinc_time_input(const JobRec& r, const std::string& depfile_text);
  std::mutex tmf_mu_; std::set<std::string> tm_files_seen_; std::atomic<uint64_t> tm_refusals_{0};   // summary v4.time_macros
  std::vector<std::string> fp_dirs(const std::string& cc_local);   // the fingerprinted system directories
  std::unordered_map<std::string, std::vector<std::string>> fpdirs_cache_;   // guarded by hmu_
  // a compiler's content fingerprint is computed once per coordinator run on
  // its own NORMAL-priority thread (the tracker's thread runs at background priority and, under a build's load, took
  // many seconds on an empty memo: every request before it got no affinity). Started at start-up for the usual
  // drivers; a request that finds it not ready waits for it, bounded by OPTICC_SYSINC_WAIT_S (default 10 s).
  std::shared_future<void> sysinc_warm(const std::string& canonical_cc);
  std::mutex sysinc_warm_mu_;
  std::unordered_map<std::string, std::shared_future<void>> sysinc_warming_;
  struct SysincTiming { double ready_at_s = -1, scan_s = 0; };
  std::unordered_map<std::string, SysincTiming> sysinc_timing_;   // canonical compiler -> readiness (summary)
  std::atomic<uint64_t> sysinc_before_ready_{0}, sysinc_wait_timeouts_{0}, sysinc_wait_ns_{0};
  void publish_sysinc(const std::string& canonical_cc, const std::string& sig);
  bool try_client_cache(const std::shared_ptr<JobRec>& r, const net::Socket& s);
  // zstd level for a classic upload to `node`, from its measured upload bandwidth (unless OPTICC_ZSTD is set):
  // >= 400 Mbit/s raw (no compression), >= 50 Mbit/s zstd --fast=3, >= 5 Mbit/s level 1, else level 3.
  // live per-node upload throughput (bits/s, EMA of the kernel's non-app-limited TCP delivery rate after each
  // upload); 0 = no sample yet. It replaces the start-up probe in topo_ (scheduler) and in the compression choice.
  std::vector<double> live_up_bps_;
  void observe_upload(int node);
  // workers unreachable at start-up or lost mid-build (never verification failures) are retried with backoff
  // during the build and admitted when they answer (fresh probe); summary "late_admissions".
  std::set<int> unreachable_;
  json late_admissions_ = json::array();
  void admission_loop();
  bool admit_worker(int k);
  int classic_zstd_level(int node) {
    if (o_.zstd_explicit || node <= 0) return o_.zstd_level;
    double bw;
    { std::lock_guard<std::mutex> g(mu_);   // live_up_bps_ / topo_ are written under mu_
      bw = (static_cast<size_t>(node) < live_up_bps_.size() && live_up_bps_[static_cast<size_t>(node)] > 0) ? live_up_bps_[static_cast<size_t>(node)] : topo_.node(node).bw_up_bps; }
    if (bw >= 400e6) return proto::kNoCompression;
    if (bw >= 50e6) return -3;
    if (bw >= 5e6) return 1;
    return 3;
  }
  bool store_client_cache(const std::shared_ptr<JobRec>& r, const proto::CompileResult& res);   // true: stored
  // WRITE-BACK of client-local compile results into the workers' compile caches (protocol.hpp CACHE_PUT), so the
  // worker tier covers every cacheable TU after any build (otherwise a TU compiled on the client lives only in the
  // client cache, and a fresh client, such as a new machine or a wiped cache, compiles it again). Queued after the local result is stored; sent by one background thread at the lowest CPU
  // priority, at most OPTICC_UPLOAD_MBPS (default 100) and 20% of the node's measured upload bandwidth, to the worker
  // the key's hash selects among the connected ones with a cache — never on a job's path. Skipped when a worker is known
  // (cache index) or believed (Bloom filter) to hold the key, or when an input changed since before the compile could
  // read it (direct.hpp changed_since). Bounded (OPTICC_UPLOAD_QUEUE_MB, default 512); at exit drained for at most
  // OPTICC_UPLOAD_DRAIN_S (default 20) at full speed, then abandoned. OPTICC_UPLOAD=off disables.
  struct UpTask { std::shared_ptr<JobRec> r; proto::CompileResult res; };
  std::deque<UpTask> up_q_; std::mutex up_mu_; std::condition_variable up_cv_; uint64_t up_q_bytes_ = 0; int up_busy_ = 0;
  std::atomic<bool> up_draining_{false};
  // jobs whose client was already released (result / "stored" sent) but whose store and
  // job record are still running. SIGTERM waits for them (bounded) before draining and writing the summary, so
  // the last TUs of a build are stored and in the summary even when the coordinator is stopped right after make.
  std::atomic<int> post_release_{0};
  std::mutex usr1_mu_;   // serialises SIGUSR1 summary helpers
  std::atomic<uint64_t> watcher_lag_ns_{0};   // summary v4.watcher_lag_max_s
  struct Hold { std::atomic<int>& n; explicit Hold(std::atomic<int>& c) : n(c) { ++n; } ~Hold() { --n; } Hold(const Hold&) = delete; Hold& operator=(const Hold&) = delete; };
  std::atomic<uint64_t> up_queued_{0}, up_sent_{0}, up_bytes_{0}, up_skipped_held_{0}, up_skipped_racy_{0}, up_dropped_{0}, up_failed_{0};
  void upload_async(const std::shared_ptr<JobRec>& r, const proto::CompileResult& res);
  void upload_worker();
  int upload_one(UpTask& t);   // 1 done (sent or skipped), 0 retry later (no worker with a cache connected)
  void drain_uploads();
  void record_direct(const std::shared_ptr<JobRec>& r, const std::string& okey, const std::string& depfile_text);
  // SHARED DIRECT MANIFESTS — fresh-client fast path (rdirect_coord.inc, opticc/rdirect.hpp)
  rdirect::Validator rd_val_; rdirect::Toolchain rd_tc_;
  std::vector<std::pair<bool, bloom::Filter>> rd_bloom_; std::mutex rd_bloom_mu_;
  std::map<std::string, std::vector<std::string>> rd_sysdirs_;   // compiler -> sysinc::dirs_for (rd_bloom_mu_)
  std::atomic<int64_t> rd_next_id_{1};
  std::atomic<uint64_t> rd_hits_{0}, rd_misses_{0}, rd_asked_{0}, rd_absent_{0}, rd_refused_{0}, rd_timeouts_{0}, rd_integrity_{0}, rd_recorded_{0}, rd_ns_{0}, rd_get_ns_{0}, rd_dec_ns_{0}, rd_val_ns_{0}, rd_store_ns_{0};
  struct RdTask { std::shared_ptr<JobRec> r; direct::Entry entry; };
  struct RdLocal { std::string dkey; direct::Entry entry; rdirect::Object obj; };
  std::deque<RdLocal> rd_local_q_;
  void rd_store_local(RdLocal& t);
  std::deque<RdTask> rd_q_; std::mutex rd_mu_; std::condition_variable rd_cv_; int rd_busy_ = 0; std::atomic<bool> rd_draining_{false};
  std::atomic<uint64_t> rd_pub_queued_{0}, rd_pub_sent_{0}, rd_pub_bytes_{0}, rd_pub_dropped_{0}, rd_pub_failed_{0};
  std::string rd_pkey(const JobRec& r);
  std::vector<int> rd_nodes(const std::string& pk, bool filter);
  bool rd_get(int node, const std::string& pk, const std::string& okey, rdirect::ReplyMsg* rep, const std::string& kind = std::string());
  std::map<std::string, double> exit_s_;   // exit phases (written once, at exit)
  bool try_remote_direct(const std::shared_ptr<JobRec>& r, const net::Socket& s);
  void rd_publish_async(const std::shared_ptr<JobRec>& r, const direct::Entry& e);
  void rd_publish_one(RdTask& t);
  void rd_publish_worker();
  void rd_drain();
  void rd_drain_local();   // wait for queued local stores of fetched entries (summary on SIGUSR1)
  json rd_summary();
  // direct entries are recorded OFF the build's critical path: queued, and written by background threads at the
  // lowest CPU priority (they use the cycles the build leaves idle, e.g. during the final link); whatever is still
  // queued at shutdown is drained by normal-priority threads before the summary is written (bounded by
  // OPTICC_RECORD_DRAIN_S, default 120 s). Recording inline would add tens of seconds to a large clean build.
  struct RecTask { std::shared_ptr<JobRec> r; std::string okey, dep; };
  std::deque<RecTask> rec_q_; std::mutex rec_qmu_; std::condition_variable rec_qcv_; int rec_busy_ = 0;
  std::atomic<uint64_t> rec_dropped_{0};
  void record_direct_async(const std::shared_ptr<JobRec>& r, const std::string& okey, const std::string& dep);
  void record_worker(bool background);
  void yield_to_compiles(const std::function<size_t()>& queued);
  std::atomic<uint64_t> bg_defer_ns_{0};
  void drain_records();
  uint64_t fasthits_at_start_ = 0;   // size of <cache>/state/fasthits when the coordinator started
  uint64_t probehits_at_start_ = 0;  // size of <cache>/state/probehits (compiler probes answered by the wrapper)
  std::atomic<uint64_t> rec_calls_{0}, rec_ok_{0}, rec_ns_{0}, unexplained_entries_{0}, local_only_jobs_{0}, directive_skips_{0}, racy_store_skips_{0}, toolchain_skips_{0}, closure_builds_{0};   // profiling: record_direct
  std::atomic<uint64_t> remote_launches_{0};   // remote attempts so far (the background-prefetch demand gate)
  std::atomic<double> pf_gate_open_s_{-1};     // when background prefetch started (-1: never)
  void prefetch_warm_load();                   // hash the warm ranking (moved off the startup path)
  std::atomic<uint64_t> rec_phase_ns_[7] = {};   // scan, direct_inputs, pch+computed, stat, hash, negatives, write
  std::mutex rec_skip_mu_; std::map<std::string, std::pair<int, std::string>> rec_skip_;
  void store_local_result(const std::shared_ptr<JobRec>& r, const json& done, const net::Socket& s);   // cache local compiles too
  void serve_local_only(const std::shared_ptr<JobRec>& r, const net::Socket& s);   // assembler compiles
  void go_local(JobRec& r);   // tell the client to compile locally no earlier than request arrival + racy margin
  bool pp_key(JobRec& r, bool wait);   // preprocessed-text object key of a computed-__has_include TU
  void store_pp(const std::shared_ptr<JobRec>& r, const proto::CompileResult& res);
  std::string pp_marker(const std::string& lkey) const;
  std::atomic<uint64_t> pp_hits_{0}, pp_stores_{0}, pp_runs_{0}, pp_ns_{0};
  void note_skip(const JobRec& r, const char* kind, const std::string& why);
  void note_trace(const JobRec& r, const char* kind, const std::string& what);       // summary v4.skips + trace
  void observe_speed_locked(const std::shared_ptr<JobRec>& r, int node, double cpu_s, bool header_push);
  std::atomic<bool> stopping_{false};
  // security
  noise::KeyPair client_key_; bool have_client_key_ = false;
  std::string key_dir_;
  std::vector<std::string> node_channel_;        // index = node id ("noise" | "plain"), filled by probe
  std::vector<std::string> node_fingerprint_;
  double enc_bps_ = 0;
  std::map<std::string, std::string> known_workers_;   // "host:port" -> pubkey b64
  void load_known_workers();
  void record_known_worker(const std::string& hostport, const std::string& pub);
  bool maybe_verify(const std::shared_ptr<JobRec>& r, int primary);
  void resolve_verify(const std::shared_ptr<JobRec>& r, proto::CompileResult* res, bool* ok);
  std::string local_reference_sha(const JobRec& r);
  std::mt19937_64 vrng_{1};
  int verify_jobs_ = 0, verify_mismatches_ = 0, verify_recompiled_ = 0;
  std::vector<int> node_verify_failed_;
  bool node_flagged(int n) const { return n > 0 && node_verify_failed_.size() > static_cast<size_t>(n) && node_verify_failed_[static_cast<size_t>(n)] > 0; }
  void quarantine(const std::vector<int>& liars);
  // header prefetch
  prefetch::Tracker pf_;
  std::vector<prefetch::TokenBucket> pf_bucket_;     // index = node id
  std::vector<bool> pf_exact_;                       // node's store was empty at HELLO: held-set is exact
  std::vector<uint64_t> pf_piggy_bytes_, pf_bg_bytes_;
  void prefetch_setup();
  void prefetch_loop();
  void prefetch_send(int node, const std::vector<proto::FileRef>& files, bool background, uint64_t* raw_out);
  void prefetch_save_ranking();
};

void Coordinator::load_known_workers() {
  std::string f = key_dir_ + "/known_workers";
  if (!util::file_exists(f)) return;
  std::istringstream is(util::read_file(f)); std::string line;
  while (std::getline(is, line)) {
    auto hash = line.find('#'); if (hash != std::string::npos) line = line.substr(0, hash);
    std::istringstream ls(line); std::string hp, key; if (!(ls >> hp >> key)) continue;
    known_workers_[hp] = key;
  }
}
void Coordinator::record_known_worker(const std::string& hostport, const std::string& pub) {
  known_workers_[hostport] = pub;
  util::mkdir_p(key_dir_);
  std::string f = key_dir_ + "/known_workers";
  std::string cur = util::file_exists(f) ? util::read_file(f) : "";
  if (!cur.empty() && cur.back() != '\n') cur.push_back('\n');
  util::write_file(f, cur + hostport + " " + pub + "\n");
}

// hosts: "host[:port][/cores][@speed][#pubkey][,plain] ..." separated by whitespace (or commas; ",plain" and
// "+plain" are the plain-mode opt-in suffix, not a separator). Security per worker follows noise::decide():
// a known key -> Noise; OPTICC_SECURITY=plain or the suffix -> plain, only to private/VPN addresses;
// unknown key -> TOFU if OPTICC_TOFU=1, else the worker is refused with the remedy printed.
void Coordinator::setup_topology() {
  topo_.client.cores = util::online_cores();
  topo_.client.speed = 1.0;
  topo_.client.mem_bytes = util::total_memory_bytes();
  topo_.params.local_slots = o_.local_slots;
  topo_.params.zstd_level = o_.zstd_level;
  // default mode header-push (a TU that cannot be header-pushed -- unknown source -- runs classic,
  // Scheduler::modes_for). Classic mode is bound by client-side preprocessing; header-push scales further.
  std::string mode = util::getenv_or("OPTICC_MODE", "header-push");
  topo_.params.mode_policy = mode == "classic" ? ModePolicy::Classic : mode == "auto" ? ModePolicy::Auto : ModePolicy::HeaderPush;
  if (const char* nu = std::getenv("OPTICC_NU")) topo_.params.nu = std::atof(nu);
  if (util::getenv_or("OPTICC_PRICED", "on") == "off") topo_.params.priced = false;
  // late binding for every mode by default (in simulation, late beats early binding for header-push as well, on both
  // client-bound and worker-bound traces and across predictor quality)
  std::string binding = o_.binding.empty() ? "late" : o_.binding;
  topo_.params.binding = binding == "early" ? Binding::Early : Binding::Late;
  if (o_.window == "slots") topo_.params.window_scale = 0.0;                      // N_k = ~c_k
  else if (!o_.window.empty()) topo_.params.window_scale = std::max(0.0, std::atof(o_.window.c_str()));
  key_dir_ = o_.key_dir.empty() ? util::getenv_or("HOME", "/tmp") + "/.config/opticc" : o_.key_dir;
  load_known_workers();
  noise::SecMode secmode = noise::parse_sec_mode(o_.security);
  bool have_psk = false; noise::Key psk{};
  if (!o_.psk_file.empty()) { try { have_psk = noise::load_psk(o_.psk_file, &psk); } catch (const std::exception& e) { fputs((std::string("opticc: psk: ") + e.what() + "\n").c_str(), stderr); } }
  std::string h = o_.hosts;
  h = util::replace_all(h, ",plain", "+plain"); for (auto& ch : h) if (ch == ',') ch = ' ';
  std::istringstream is(h); std::string tok; int id = 1;
  while (is >> tok) {
    Node n; n.id = id;
    std::string spec = tok; double speed = 0; int cores = 0; std::string pub; bool spec_plain = false;
    if (auto pl = spec.find("+plain"); pl != std::string::npos) { spec_plain = true; spec.erase(pl, 6); }
    if (auto hs = spec.find('#'); hs != std::string::npos) { pub = spec.substr(hs + 1); spec = spec.substr(0, hs); }
    if (auto at = spec.find('@'); at != std::string::npos) { speed = std::atof(spec.c_str() + at + 1); spec = spec.substr(0, at); }
    if (auto sl = spec.find('/'); sl != std::string::npos) { cores = std::atoi(spec.c_str() + sl + 1); spec = spec.substr(0, sl); }
    int port = proto::kDefaultPort;
    if (auto c = spec.rfind(':'); c != std::string::npos) { port = std::atoi(spec.c_str() + c + 1); spec = spec.substr(0, c); }
    n.name = spec + ":" + std::to_string(port);
    n.cores = cores > 0 ? cores : 0; n.speed = speed > 0 ? speed : 0;
    if (pub.empty()) { auto it = known_workers_.find(n.name); if (it != known_workers_.end()) pub = it->second; }
    uint32_t ip = util::resolve_ipv4(spec);
    bool priv = ip != 0 && util::plaintext_ok_address(ip);
    noise::SecDecision d = noise::decide(secmode, !pub.empty(), spec_plain, o_.tofu, priv);
    if (d.kind == noise::SecDecision::Tofu) {
      try { pub = net::fetch_worker_pubkey(spec, port); record_known_worker(n.name, pub); d.kind = noise::SecDecision::Noise;
            fputs(("opticc: TOFU: recorded " + n.name + " " + noise::fingerprint([&] { noise::Key k; noise::parse_pub(pub, &k); return k; }()) + " in " + key_dir_ + "/known_workers\n").c_str(), stderr); }
      catch (const std::exception& e) { d.kind = noise::SecDecision::Refuse; d.reason = std::string("TOFU key fetch failed: ") + e.what(); }
    }
    if (d.kind == noise::SecDecision::Refuse) {
      fputs(("opticc: worker " + n.name + " not used: " + d.reason + "\n").c_str(), stderr);
      probes_.push_back(json{{"node", n.name}, {"ok", false}, {"error", d.reason}});
      continue;
    }
    net::Security sec;
    if (d.kind == noise::SecDecision::Noise) {
      if (!have_client_key_) {
        try {
          bool created = noise::load_or_create_keypair(key_dir_, "client", &client_key_); have_client_key_ = true;
          if (created) fputs(("opticc: created client key " + noise::fingerprint(client_key_.pub) + " in " + key_dir_ + "; add " + key_dir_ + "/client.pub to each worker's authorized_clients\n").c_str(), stderr);
        } catch (const std::exception& e) { fputs((std::string("opticc: client key: ") + e.what() + "\n").c_str(), stderr); continue; }
      }
      sec.kind = net::Security::Noise; sec.local = &client_key_;
      noise::Key k; if (!noise::parse_pub(pub, &k)) { fputs(("opticc: worker " + n.name + " not used: malformed public key\n").c_str(), stderr); continue; }
      sec.remote_pub = k;
    } else { sec.kind = net::Security::Plain; sec.have_psk = have_psk; sec.psk = psk; }
    // a plaintext connection goes only to the IPv4 literal that passed the private-range
    // check (never to another A/AAAA record of the same name)
    std::string connect_host = spec;
    if (sec.kind == net::Security::Plain && ip != 0) {
      in_addr a{}; a.s_addr = htonl(ip); char buf[INET_ADDRSTRLEN] = {0};
      if (inet_ntop(AF_INET, &a, buf, sizeof buf)) connect_host = buf;
    }
    topo_.nodes.push_back(n); id++;
    conns_.push_back(std::make_unique<net::WorkerConn>(connect_host, port, o_.token, sec));
  }
}

// Fast start: measuring everything before the first job (cpu and compile benchmarks, a cipher benchmark, then per
// worker connect, pings and a bandwidth probe) can take longer than the client's 4 s wait. So workers are probed in
// parallel, and per-worker rtt/bandwidth and the local compile benchmark / cipher rate are cached across coordinator lifetimes
// (~/.cache/opticc/probe-cache.json, keyed by worker fingerprint; entries older than 7 days are re-measured); a cached
// worker costs one connect + one ping. Online speed estimation and the windows' refresh correct stale priors.
static std::string probe_cache_path() {
  std::string p = util::getenv_or("OPTICC_PROBE_CACHE", "");
  return p.empty() ? util::getenv_or("HOME", "/tmp") + "/.cache/opticc/probe-cache.json" : p;
}

void Coordinator::probe_workers() {
  json cache = json::object();
  try { std::string cp = probe_cache_path(); if (util::file_exists(cp)) cache = json::parse(util::read_file(cp)); } catch (...) { cache = json::object(); }
  const double tnow = static_cast<double>(std::time(nullptr)), max_age = 7 * 86400.0;
  auto fresh = [&](const json& e) { return e.is_object() && tnow - e.value("t", 0.0) < max_age; };
  local_bench_ = util::cpu_bench_s();
  std::string bcc = util::getenv_or("OPTICC_BENCH_CC", "default");
  json lc = cache.value("local", json::object());
  bool lc_ok = fresh(lc) && lc.value("bench_cc", std::string()) == bcc && lc.value("compile_bench_s", 0.0) > 0;
  std::thread cbench_thr;
  if (lc_ok) local_compile_bench_ = lc.value("compile_bench_s", 0.0);
  else cbench_thr = std::thread([this] { local_compile_bench_ = util::compile_bench_s(); });   // overlaps the worker probes
  bool any_noise = false; for (auto& c : conns_) any_noise |= c->security().kind == net::Security::Noise;
  if (any_noise) {
    enc_bps_ = (fresh(lc) && lc.value("enc_bps", 0.0) > 0) ? lc.value("enc_bps", 0.0) : noise::benchmark_bytes_per_s(4u << 20);
    topo_.params.enc_bytes_per_s = enc_bps_;
  }
  topo_.params.security = any_noise ? "noise" : "plain";
  json wcache = cache.value("workers", json::object());
  struct PR { bool ok = false; json ack; double rtt = 1e9, up = 0, down = 0; bool cached = false; std::string err, key; };
  std::vector<PR> res(conns_.size());
  std::vector<std::thread> thr;
  for (size_t i = 0; i < conns_.size(); ++i) thr.emplace_back([&, i] {
    PR& r = res[i]; auto& c = conns_[i];
    try {
      c->connect(5.0);
      r.ack = c->hello_ack();
      // key by worker identity AND local route; validate a cached entry against 2 fresh pings (TTL 10 min,
      // RTT within 1.5x) -- otherwise re-probe now, before the first dispatch (probecache.hpp)
      r.key = probecache::key(r.ack.value("fingerprint", std::string()), topo_.nodes[i].name, c->local_addr());
      json wc = wcache.value(r.key, json());
      double rtt_now = std::min(c->ping_rtt_s(), c->ping_rtt_s());
      double ttl = std::atof(util::getenv_or("OPTICC_PROBE_TTL_S", "600").c_str());
      if (probecache::validate(wc, tnow, rtt_now, ttl > 0 ? ttl : 600.0) == probecache::Verdict::Use) {
        r.rtt = rtt_now; r.up = wc.value("up", 0.0); r.down = wc.value("down", 0.0); r.cached = true;
      } else {
        r.rtt = std::min(rtt_now, c->ping_rtt_s());
        auto [up, down] = c->bandwidth_probe(1u << 20, 1u << 20);
        r.up = up; r.down = down;
      }
      r.ok = true;
    } catch (const std::exception& e) { r.err = e.what(); }
  });
  for (auto& t : thr) t.join();
  if (cbench_thr.joinable()) cbench_thr.join();
  std::vector<Node> alive; std::vector<std::unique_ptr<net::WorkerConn>> conns;
  node_channel_.assign(1, "local"); node_fingerprint_.assign(1, ""); pf_exact_.assign(1, false);
  for (size_t i = 0; i < conns_.size(); ++i) {
    Node n = topo_.nodes[i];
    auto& c = conns_[i];
    PR& r = res[i];
    json pr{{"node", n.name}};
    if (!r.ok) {
      // an unreachable worker is KEPT (fixed node id) with placeholder figures and disabled in the scheduler;
      // admission_loop() retries it with backoff and admits it when it comes up
      pr.update({{"ok", false}, {"error", r.err}, {"retrying", true}});
      if (n.cores <= 0) n.cores = 1;
      if (n.speed <= 0) n.speed = 1.0;
      n.rtt_s = 1e-3; n.bw_up_bps = n.bw_down_bps = 1e9;
      n.id = static_cast<int>(alive.size()) + 1;
      unreachable_.insert(n.id);
      alive.push_back(n); conns.push_back(std::move(c));
      node_channel_.push_back(c ? (c->encrypted() ? "noise" : "plain") : "plain"); node_fingerprint_.push_back(""); pf_exact_.push_back(false);
    }
    else {
      const json& ack = r.ack;
      double bench = ack.value("bench_s", 0.0);
      if (n.cores <= 0) n.cores = ack.value("jobs", ack.value("cores", 1));
      if (n.mem_bytes <= 0) n.mem_bytes = ack.value("mem_bytes", 0.0);
      double cbench = ack.value("compile_bench_s", 0.0);
      bool micro = false;
      if (n.speed <= 0) {   // the compile benchmark when both sides have it; the microbenchmark only as a fallback
        if (cbench > 0 && local_compile_bench_ > 0) n.speed = local_compile_bench_ / cbench;
        else { n.speed = (bench > 0 && local_bench_ > 0) ? local_bench_ / bench : 1.0; micro = local_compile_bench_ > 0; }
        n.speed = std::min(8.0, std::max(0.125, n.speed));
      }
      pr["compile_bench_s"] = cbench; pr["micro_speed"] = (bench > 0 && local_bench_ > 0) ? local_bench_ / bench : 0.0;
      n.rtt_s = r.rtt; n.bw_up_bps = r.up; n.bw_down_bps = r.down;
      c->start_demux();
      std::string chan = c->encrypted() ? "noise" : "plain";
      pr.update({{"ok", true}, {"rtt_ms", r.rtt * 1e3}, {"bw_up_mbps", r.up / 1e6}, {"bw_down_mbps", r.down / 1e6}, {"probe_cached", r.cached},
                 {"speed", n.speed}, {"cores", n.cores}, {"mem_gb", n.mem_bytes / 1e9}, {"worker", ack.value("hostname", "")},
                 {"channel", chan}, {"fingerprint", ack.value("fingerprint", "")}, {"enc_MBps", enc_bps_ / 1e6}});
      if (!r.cached) wcache[r.key] = json{{"up", r.up}, {"down", r.down}, {"rtt_s", r.rtt}, {"t", tnow}};
      n.id = static_cast<int>(alive.size()) + 1;
      if (prior_micro_.size() <= static_cast<size_t>(n.id)) prior_micro_.resize(static_cast<size_t>(n.id) + 1, 0);
      prior_micro_[static_cast<size_t>(n.id)] = micro ? 1 : 0;
      alive.push_back(n); conns.push_back(std::move(c));
      node_channel_.push_back(chan); node_fingerprint_.push_back(ack.value("fingerprint", ""));
      pf_exact_.push_back(ack.value("cas_entries", int64_t(-1)) == 0);
      {
        std::lock_guard<std::mutex> g(bloom_mu_);
        if (worker_bloom_.size() <= static_cast<size_t>(n.id)) worker_bloom_.resize(static_cast<size_t>(n.id) + 1);
        worker_bloom_[static_cast<size_t>(n.id)] = bloom::Filter::from_json(ack.value("objcache_bloom", json()));
        pr["objcache_keys"] = worker_bloom_[static_cast<size_t>(n.id)].n;
      }
    }
    { std::string s2 = pr.dump(); s2.push_back('\n'); fputs(("opticc: probe " + s2).c_str(), stderr); }
    probes_.push_back(pr);
  }
  topo_.nodes = alive; conns_ = std::move(conns);
  node_verify_failed_.assign(topo_.nodes.size() + 1, 0);
  live_up_bps_.assign(topo_.nodes.size() + 1, 0.0);   // live upload throughput per node (sized once)
  { std::vector<double> pri(topo_.nodes.size() + 1, 1.0); for (auto& nd : topo_.nodes) pri[static_cast<size_t>(nd.id)] = nd.speed;
    speed_est_ = std::make_unique<SpeedEstimator>(pri); }
  vrng_.seed(o_.verify_seed);
  try {   // persist the cache (atomic replace)
    if (!lc_ok && local_compile_bench_ > 0) cache["local"] = json{{"compile_bench_s", local_compile_bench_}, {"bench_cc", bcc}, {"enc_bps", enc_bps_}, {"t", tnow}};
    json pruned = json::object();   // drop stale fingerprints (every test run makes new keys); keep at most 256
    for (auto it = wcache.begin(); it != wcache.end() && pruned.size() < 256; ++it) if (fresh(it.value())) pruned[it.key()] = it.value();
    cache["workers"] = pruned;
    std::string cp = probe_cache_path(); util::mkdir_p(util::dirname(cp));
    util::write_file(cp + ".tmp", cache.dump()); std::rename((cp + ".tmp").c_str(), cp.c_str());
  } catch (...) {}
}


// does any worker advertise a cached key (HELLO Bloom filter or a result indexed this session)?
bool Coordinator::any_worker_keys() {
  if (cache_idx_.size() > 0) return true;
  std::lock_guard<std::mutex> g(bloom_mu_);
  for (size_t k = 1; k < worker_bloom_.size(); ++k) if (worker_bloom_[k].n > 0) return true;
  return false;
}

std::vector<int> Coordinator::bloom_holders(const std::string& key) {
  std::vector<int> h;
  std::lock_guard<std::mutex> g(bloom_mu_);
  for (size_t k = 1; k < worker_bloom_.size(); ++k)
    if (worker_bloom_[k].n > 0 && worker_bloom_[k].maybe(key) && !sched_->node_disabled(static_cast<int>(k))) h.push_back(static_cast<int>(k));
  return h;
}

// retry unreachable / lost workers with exponential backoff (0.5 s .. 10 s) for the coordinator's lifetime.
void Coordinator::admission_loop() {
  std::map<int, std::pair<double, int>> next;   // node -> (next attempt time, attempts)
  while (!stopping_.load()) {
    std::vector<int> todo;
    { std::lock_guard<std::mutex> g(mu_); todo.assign(unreachable_.begin(), unreachable_.end()); }
    double t = util::now_s();
    for (int k : todo) {
      auto& nx = next[k];
      if (nx.first > t) continue;
      ++nx.second;
      if (admit_worker(k)) { next.erase(k); continue; }
      nx.first = util::now_s() + std::min(10.0, 0.5 * std::pow(2.0, std::min(nx.second - 1, 5)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

bool Coordinator::admit_worker(int k) {
  if (k <= 0 || static_cast<size_t>(k) > conns_.size()) return false;
  auto& c = conns_[static_cast<size_t>(k - 1)];
  json ack; double rtt = 1e9, up = 0, down = 0;
  try {
    c->connect(3.0);
    ack = c->hello_ack();
    for (int i = 0; i < 3; ++i) rtt = std::min(rtt, c->ping_rtt_s());
    auto bw = c->bandwidth_probe(1u << 20, 1u << 20); up = bw.first; down = bw.second;
  } catch (const std::exception&) { return false; }
  double bench = ack.value("bench_s", 0.0), cbench = ack.value("compile_bench_s", 0.0);
  double speed = (cbench > 0 && local_compile_bench_ > 0) ? local_compile_bench_ / cbench : (bench > 0 && local_bench_ > 0) ? local_bench_ / bench : 1.0;
  speed = std::min(8.0, std::max(0.125, speed));
  const bool micro = !(cbench > 0) && local_compile_bench_ > 0;   // refreshed by the first result carrying the benchmark
  c->start_demux();
  std::lock_guard<std::mutex> g(mu_);
  Node& n = topo_.nodes[static_cast<size_t>(k - 1)];
  int cores = ack.value("jobs", ack.value("cores", 1));
  if (cores > 0) n.cores = cores;
  if (ack.value("mem_bytes", 0.0) > 0) n.mem_bytes = ack.value("mem_bytes", 0.0);
  n.speed = speed; n.rtt_s = rtt; n.bw_up_bps = up; n.bw_down_bps = down;
  node_channel_[static_cast<size_t>(k)] = c->encrypted() ? "noise" : "plain";
  node_fingerprint_[static_cast<size_t>(k)] = ack.value("fingerprint", "");
  pf_exact_[static_cast<size_t>(k)] = ack.value("cas_entries", int64_t(-1)) == 0;
  {   // a (re)admitted worker brings a new cache: replace its Bloom filter from this HELLO (a stale filter would keep
      // attracting jobs) and forget the trust learned on the old one
    std::lock_guard<std::mutex> gb(bloom_mu_);
    if (worker_bloom_.size() <= static_cast<size_t>(k)) worker_bloom_.resize(static_cast<size_t>(k) + 1);
    worker_bloom_[static_cast<size_t>(k)] = bloom::Filter::from_json(ack.value("objcache_bloom", json()));
  }
  if (trust_.reset(k)) trust_resets_++;
  cache_idx_.forget_node(k);
  if (speed_est_) speed_est_->set_prior(k, speed);
  if (prior_micro_.size() <= static_cast<size_t>(k)) prior_micro_.resize(static_cast<size_t>(k) + 1, 0);
  prior_micro_[static_cast<size_t>(k)] = micro ? 1 : 0;
  unreachable_.erase(k);
  sched_->enable_node(k); sched_->refresh(now());
  late_admissions_.push_back({{"node", n.name}, {"t", now()}, {"cores", n.cores}, {"speed", speed}, {"rtt_ms", rtt * 1e3}, {"bw_up_mbps", up / 1e6}});
  fputs(("opticc: worker " + n.name + " admitted late at t=" + std::to_string(now()) + " s\n").c_str(), stderr);
  run_decide_locked(); cv_.notify_all();
  return true;
}

// a worker whose hello ack carried no compile benchmark (opticcd listens before it finishes) was rated with the
// microbenchmark, which can mis-rate real compile speed by up to ~2x. The first compile result that carries the
// benchmark replaces that prior once; the estimator re-anchors without discarding its samples. mu_ held.
void Coordinator::refresh_prior_locked(int node, double cbench) {
  if (node <= 0 || static_cast<size_t>(node) >= prior_micro_.size() || !prior_micro_[static_cast<size_t>(node)]) return;
  if (!(cbench > 0) || !(local_compile_bench_ > 0)) return;
  prior_micro_[static_cast<size_t>(node)] = 0;   // once
  double speed = std::min(8.0, std::max(0.125, local_compile_bench_ / cbench));
  Node& n = topo_.nodes[static_cast<size_t>(node - 1)];
  double before = n.speed;
  if (speed_est_) { speed_est_->set_prior(node, speed); for (auto& nd : topo_.nodes) nd.speed = speed_est_->speed(nd.id); }
  else n.speed = speed;
  prior_refreshes_.push_back({{"node", n.name}, {"t", now()}, {"compile_bench_s", cbench}, {"local_compile_bench_s", local_compile_bench_}, {"prior", speed}, {"speed_before", before}, {"speed_after", n.speed}});
  sched_->refresh(now());
  if (util::getenv_or("OPTICC_VERBOSE", "") == "1")
    fprintf(stderr, "opticc: worker %s compile benchmark arrived (%.3f s): prior %.3f -> speed %.3f (was %.3f)\n", n.name.c_str(), cbench, speed, n.speed, before);
}

// one live throughput sample after an upload to `node` (non-app-limited TCP delivery rate, Linux); EMA 0.3.
void Coordinator::observe_upload(int node) {
  if (node <= 0 || static_cast<size_t>(node) > conns_.size()) return;
  double bps = 0;
  if (!conns_[static_cast<size_t>(node - 1)]->tcp_delivery_rate(&bps) || !(bps > 0)) return;
  std::lock_guard<std::mutex> g(mu_);
  if (live_up_bps_.size() < topo_.nodes.size() + 1) return;   // sized once in probe_workers
  double& v = live_up_bps_[static_cast<size_t>(node)];
  v = v > 0 ? 0.7 * v + 0.3 * bps : bps;
  topo_.nodes[static_cast<size_t>(node - 1)].bw_up_bps = v;   // the scheduler's link model follows the live estimate
}

// one completed remote compile -> one speed sample for its node; mu_ held.
void Coordinator::observe_speed_locked(const std::shared_ptr<JobRec>& r, int node, double cpu_s, bool header_push) {
  // node 0: a clean local compile of the whole TU (the compiler child's rusage, preprocessing included) is a sample
  // of the client, the reference node -- callers pass header_push = true (the whole w_hat, from source)
  if (!speed_est_ || node < 0 || cpu_s <= 0 || r->id < 0 || static_cast<size_t>(r->id) >= wl_.jobs.size()) return;
  static const bool off = util::getenv_or("OPTICC_SPEED_EST", "on") == "off";   // kill switch: keep the startup priors
  if (off) return;
  const Job& j = wl_.jobs[static_cast<size_t>(r->id)];
  // w_hat is from source; a header-push worker (and a local compile) does all of it, a classic worker w - p
  double w_hat = remote_compile_work(pred_->predict(j), j.preprocess_cpu_s, header_push ? Mode::HeaderPush : Mode::Classic);
  speed_est_->observe(node, w_hat, cpu_s);
  for (auto& nd : topo_.nodes) nd.speed = speed_est_->speed(nd.id);
  if (speed_est_->updates() % 16 == 0) sched_->refresh(now());
}

// client-local object cache ------------------------------------------------------------------------------
// the cache SHAPE is the scanner's closure (+ source) — the same whether or not a .d/.cmd file exists, so an entry
// stored after a compile (when the compiler's .d is present and adds e.g. computed-include targets) still matches after
// `make clean`. Content is checked per used file in the lookup (hash on demand for files outside this view).
static std::unordered_map<std::string, std::string> client_view(const JobRec& r, const std::function<std::vector<std::string>(const std::vector<std::string>&)>& kbatch) {
  std::unordered_map<std::string, std::string> view;
  std::vector<std::string> all; all.reserve(r.scan_abs.size() + 1);
  std::string src_abs = closure::absolutize(r.src, r.cwd);
  all.push_back(src_abs); all.insert(all.end(), r.scan_abs.begin(), r.scan_abs.end());
  auto shas = kbatch(all);
  if (shas.empty() || shas[0].empty()) return {};
  view.reserve(all.size());
  for (size_t i = 0; i < all.size(); ++i) if (!shas[i].empty()) view[cachekey::relativise(all[i], r.cwd)] = shas[i];   // vanished: skipped
  return view;
}

bool Coordinator::try_client_cache(const std::shared_ptr<JobRec>& r, const net::Socket& s) {
  static const bool dbg = util::getenv_or("OPTICC_DEBUG_CACHE", "") == "1";
  auto miss = [&](const char* why) { if (dbg) std::fprintf(stderr, "client-cache miss %s: %s\n", r->src.c_str(), why); return false; };
  if (!local_cache_.enabled() || r->lkey.empty() || !(r->scan.distributable || r->scan.local_cacheable)) return miss("disabled/no-key/not-cacheable");
  if (topo_.params.mode_policy == ModePolicy::Classic) return miss("classic");   // classic keys need the .i (not before preprocessing)
  // a computed-__has_include TU is never in a manifest; a marker says it was stored under a preprocessed key
  const bool pp_marked = !state_dir_.empty() && util::file_exists(pp_marker(r->lkey));
  if (!pp_marked && !local_cache_.has_manifest(r->lkey)) return miss("no manifest");   // cheap pre-check: the closure is hashed only now
  double tA = util::now_s();
  scan_only(*r);   // lookups need only the scanner's path set (no dependency-file read, no raw hashing)
  double tB = util::now_s();
  if (r->has_include_computed) {
    if (!r->env.empty()) return miss("computed __has_include under an include-affecting environment");   // pp runs in OUR env
    if (!pp_marked || !pp_key(*r, true)) return miss("computed __has_include");
    objcache::Hit hit;
    if (!local_cache_.lookup_object(r->pp_okey, &hit) || hit.rc != 0) return miss("computed __has_include: preprocessed key not cached");
    util::write_file(closure::absolutize(r->out, r->cwd), hit.object);
    if (r->scan.has_md) {
      std::string tgt = r->scan.dep_target.empty() ? r->out : r->scan.dep_target;
      std::string dp = r->scan.dep_file.empty() ? args::default_dep_name(r->out) : r->scan.dep_file;
      util::write_file(closure::absolutize(dp, r->cwd), tgt + ":" + hit.depfile_body);
    }
    send(s, {{"op", "decision"}, {"node", -3}, {"name", "client-cache"}, {"mode", "header_push"}});
    send(s, {{"op", "result"}, {"rc", hit.rc}, {"stdout", hit.out}, {"stderr", hit.err}, {"node", 0}, {"mode", "header_push"}});
    pp_hits_++;
    std::lock_guard<std::mutex> g(mu_);
    Job j; j.id = static_cast<int>(wl_.jobs.size()); j.name = r->src; j.src_size_bytes = 0; j.pre_size_bytes = 0;
    wl_.jobs.push_back(j); r->id = j.id; sched_->on_jobs_added();   // accounted, never offered to the scheduler
    r->t_request = r->t_decided = r->t_done = now(); r->rc = 0; r->outcome = "client-cache"; r->cache_status = "client-hit-pp"; r->obj_bytes = hit.object.size();
    jobs_.push_back(r); last_activity_ = now();
    return true;
  }
  const bool raw_only = cachekey::keeps_comments(r->argv);
  // fast path: order-independent shape from memoised path hashes; ONE batched, stat-validated key-hash check of
  // exactly the files the cached compile read (no per-lookup view map, no sorting, no per-file locks)
  std::string src_rel = cachekey::relativise(closure::absolutize(r->src, r->cwd), r->cwd);
  std::string shape = shape_fast(*r, src_rel);
  auto sysd = sys_dirs(r->cc_local, r->lang, r->cwd);
  double tv0 = 0, tv1 = 0, tv2 = 0; size_t nv = 0;
  auto verify = [&](const std::vector<std::pair<std::string, std::string>>& closure) -> bool {
    tv0 = util::now_s();
    // the pseudo-input "@sysinc" (system-include signature) must equal the current one; an entry without it never
    // matches
    std::vector<std::pair<std::string, std::string>> files; files.reserve(closure.size());
    bool have_sys = false;
    for (auto& pr : closure) {
      if (pr.first == "@sysinc") { if (r->sysinc_sig.empty() || pr.second != r->sysinc_sig) return false; have_sys = true; }
      else files.push_back(pr);
    }
    if (!have_sys) return false;
    std::vector<std::string> abs; abs.reserve(files.size());
    std::vector<std::string> rels; rels.reserve(files.size()); for (auto& [rel, sha] : files) rels.push_back(rel);
    for (const PathEnt* e : path_ents(r->cwd, rels, sysd)) abs.push_back(e->abs);
    tv1 = util::now_s();
    auto now = key_hash_batch(abs, raw_only);
    tv2 = util::now_s(); nv = files.size();
    for (size_t i = 0; i < files.size(); ++i) if (now[i].empty() || now[i] != files[i].second) return false;
    return true;
  };
  objcache::Hit hit;
  if (!local_cache_.lookup_manifest_batch(r->lkey, shape, verify, &hit)) {
    if (dbg) std::fprintf(stderr, "client-cache view %s: %zu paths shape %s\n", r->src.c_str(), r->scan_ents.size(), shape.c_str());
    return miss("manifest lookup");
  }
  if (hit.rc != 0) return miss("rc");
  double tC = util::now_s();
  util::write_file(closure::absolutize(r->out, r->cwd), hit.object);
  if (r->scan.has_md) {
    std::string tgt = r->scan.dep_target.empty() ? r->out : r->scan.dep_target;
    std::string dp = r->scan.dep_file.empty() ? args::default_dep_name(r->out) : r->scan.dep_file;
    util::write_file(closure::absolutize(dp, r->cwd), tgt + ":" + hit.depfile_body);
  }
  if (dbg) std::fprintf(stderr, "client-cache hit-timing %s: closure %.2f ms view+lookup %.2f ms write %.2f ms | paths %.2f ms keyhash %.2f ms n %zu\n", r->src.c_str(), (tB - tA) * 1e3, (tC - tB) * 1e3, (util::now_s() - tC) * 1e3, (tv1 - tv0) * 1e3, (tv2 - tv1) * 1e3, nv);
  send(s, {{"op", "decision"}, {"node", -3}, {"name", "client-cache"}, {"mode", "header_push"}});
  send(s, {{"op", "result"}, {"rc", hit.rc}, {"stdout", hit.out}, {"stderr", hit.err}, {"node", 0}, {"mode", "header_push"}});
  // the next identical compile is answered by the wrapper itself (after the client already has its answer)
  if (!r->dkey.empty()) record_direct_async(r, hit.obj_key, hit.depfile_body.empty() ? std::string() : "x:" + hit.depfile_body);
  std::lock_guard<std::mutex> g(mu_);
  Job j; j.id = static_cast<int>(wl_.jobs.size()); j.name = r->src; j.src_size_bytes = 0; j.pre_size_bytes = 0;
  wl_.jobs.push_back(j); r->id = j.id; sched_->on_jobs_added();   // accounted, never offered to the scheduler
  r->t_request = r->t_decided = r->t_done = now(); r->rc = 0; r->outcome = "client-cache"; r->cache_status = "client-hit"; r->obj_bytes = hit.object.size();
  jobs_.push_back(r); last_activity_ = now();
  return true;
}

static std::string file_directive_in(const std::string& lang, const std::string& src_abs, const std::vector<std::string>& inputs);

// a TU whose include graph has __has_include(MACRO) (e.g. LLVM's TargetPassRegistry.inc) has no scanner-derived key: the
// macro's operand is unknown, and so is the file whose presence the test decided. Such a TU is keyed instead by its
// PREPROCESSED TEXT (ccache's preprocessor mode, for these TUs only): K = H("pp1" | lkey | H(text)). lkey covers the
// compiler, the normalised argv and the source; the text is the result of every #include, #if and __has_include as
// evaluated when it was produced, and its linemarkers name every file entered, so equal texts imply the same object (a
// deterministic compiler) and the same dependency list. Storing: the object must have been compiled from the
// bytes that were preprocessed — every input in the compile's dependency file must be unchanged since before both
// (ctime < min(t_go, t_pp) - m, stat taken after the preprocessing). Cost: one preprocessing per lookup.
static std::vector<std::string> pp_only_argv(const std::vector<std::string>& argv, const std::string& ofile, const std::string& dfile) {
  std::vector<std::string> out;
  for (size_t i = 0; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-c" || a == "-MD" || a == "-MMD" || a == "-MP" || a == "-M" || a == "-MM") continue;
    if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ") { ++i; continue; }
    if (i > 0 && a.size() > 2 && a.rfind("-o", 0) == 0) return {};   // joined -oFILE: not handled, no preprocessed key
    if (a.rfind("-MF", 0) == 0 || a.rfind("-MT", 0) == 0 || a.rfind("-MQ", 0) == 0) continue;
    if (a.rfind("-Wp,", 0) == 0 && (a.find("-MD") != std::string::npos || a.find("-MMD") != std::string::npos)) continue;
    out.push_back(a);
  }
  out.push_back("-E"); out.push_back("-o"); out.push_back(ofile);
  // the files THIS preprocessing read, in the compile's dependency mode (-MD lists system
  // headers, -MMD does not), compared with the compile's own dependency file before anything is stored or served
  if (!dfile.empty()) { out.push_back(cachekey::dep_lists_system_headers(argv) ? "-MD" : "-MMD"); out.push_back("-MF"); out.push_back(dfile); }
  return out;
}

std::string Coordinator::pp_marker(const std::string& lkey) const { return state_dir_ + "/pp/" + lkey; }

bool Coordinator::pp_key(JobRec& r, bool wait) {
  if (!r.pp_okey.empty()) return true;
  if (r.lkey.empty() || !r.scan.has_md || r.cc_local.empty()) return false;   // the store checks the dependency file
  // equal preprocessed TEXT implies an equal object only without debug information (DWARF
  // line tables carry an MD5 of every header, -g3/-gmacro the macro table, neither is in the text) and without
  // comment-keeping preprocessing; keeps_comments() is true for exactly those argument sets.
  if (cachekey::keeps_comments(r.argv)) return false;
  // these change what -E prints (no line markers, unexpanded or dumped macros), so equal text
  // would no longer imply equal inputs
  for (auto& a : r.argv)
    if (a == "-P" || a == "-Wp,-P" || a == "-Xpreprocessor" || a == "-traditional" || a == "-fdirectives-only" || a == "-dD" || a == "-dM" || a == "-dN" || a == "-dI" || a == "-dU" || a == "-traditional-cpp") {
      note_skip(r, "pp-key-refused", "preprocessor output flag " + a); return false; }
  if (wait) {   // as go_local: inputs generated right before the request then pass the store rule
    const double m = direct::racy_margin_s(), w = r.t_start_wall + m - direct::stamp_now();
    if (m <= 0.02 && w > 0) std::this_thread::sleep_for(std::chrono::duration<double>(w + 1e-4));
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::string dir;
  try { dir = util::make_temp_dir("opticc-pp-"); } catch (...) { return false; }
  auto argv = pp_only_argv(r.argv, dir + "/pp.i", dir + "/pp.d");
  if (argv.empty()) { util::remove_tree(dir); return false; }
  argv[0] = r.cc_local;
  r.t_pp_wall = direct::stamp_now();
  util::RunResult rr = util::run_process(argv, "", dir + "/err", r.cwd);
  std::string text, pp_err, pp_dep;
  if (rr.rc == 0) { try { text = util::read_file(dir + "/pp.i"); pp_dep = util::read_file(dir + "/pp.d"); } catch (...) { text.clear(); } }
  try { pp_err = util::read_file(dir + "/err"); } catch (...) {}
  // preprocessing-phase diagnostics (-Wunused-macros, macro redefinition, #warning, ...) are not determined
  // by the text; a TU whose preprocessing says anything gets no preprocessed key
  if (!pp_err.empty()) text.clear();
  util::remove_tree(dir);
  pp_runs_++; pp_ns_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  if (rr.rc != 0 || text.empty() || pp_dep.empty()) { r.t_pp_wall = 0; return false; }
  // -E collapses intra-line whitespace and comments, so a column the program observes
  // (__builtin_COLUMN, std::source_location, GCC's __builtin_source_location) is not determined by the text
  // .incbin (or an inline-asm .include) reads a file the text only names
  if (text.find(".incbin") != std::string::npos || cachekey::file_directive(text, false)) {
    directive_skips_++; note_skip(r, "pp-key-refused", "assembler file directive (.incbin/.include) in the preprocessed text"); r.t_pp_wall = 0; return false; }
  if (text.find("__builtin_COLUMN") != std::string::npos || text.find("source_location") != std::string::npos) {
    note_skip(r, "pp-key-refused", "column-dependent text (__builtin_COLUMN / source_location)"); r.t_pp_wall = 0; return false; }
  {
    std::vector<std::string> ds;
    for (auto& p : closure::parse_depfile(pp_dep)) ds.push_back(closure::absolutize(p, r.cwd));
    std::sort(ds.begin(), ds.end()); ds.erase(std::unique(ds.begin(), ds.end()), ds.end());
    r.pp_deps = std::move(ds);
  }
  r.pp_text_sha = util::sha256_hex(text);
  r.pp_okey = util::sha256_hex("pp1|" + r.lkey + "|" + r.pp_text_sha);
  return true;
}

void Coordinator::store_pp(const std::shared_ptr<JobRec>& r, const proto::CompileResult& res) {
  if (!local_cache_.enabled() || state_dir_.empty() || res.depfile.empty() || res.rc != 0) return;
  // diagnostics name ORIGINAL source locations (columns move with comments the text no longer has)
  if (!res.out.empty() || !res.err.empty()) { note_skip(*r, "store-skipped", "compiler output under the preprocessed-text key"); return; }
  { std::string bad; if (!direct::safe_args(r->argv, &bad)) { note_skip(*r, "store-skipped", "uncacheable-flag " + bad); return; } }
  if (!r->env.empty()) { note_skip(*r, "store-skipped", "preprocessed key under an include-affecting environment"); return; }
  if (!pp_key(*r, false)) return;   // computed after the compile when the lookup did not run it: the stat below follows it
  {   // a file only TESTED by __has_include is in no dependency file; if it appeared or vanished
      // between the compile and the preprocessing (either order), the two read different file sets and the text does
      // not describe the object. Stored (and published) only when the preprocessing read exactly the compile's files.
    std::vector<std::string> cd;
    for (auto& p : closure::parse_depfile(res.depfile)) cd.push_back(closure::absolutize(p, r->cwd));
    std::sort(cd.begin(), cd.end()); cd.erase(std::unique(cd.begin(), cd.end()), cd.end());
    if (cd != r->pp_deps) { racy_store_skips_++; note_trace(*r, "pp-store-decision", "refused file-set"); note_skip(*r, "store-skipped", "preprocessing and compile read different files"); return; }
  }
  // __DATE__/__TIME__ are expanded at preprocessing time but reach the object at compile time
  if (cachekey::argv_uses_time_macros(r->argv)) { note_skip(*r, "store-skipped", "time macro in a -D value"); return; }
  for (auto& p : r->pp_deps) {
    std::string sha; try { sha = util::sha256_hex(util::read_file(p)); } catch (...) { return; }
    if (time_macros(p, sha)) { note_skip(*r, "store-skipped", "time macro in " + p); return; }
  }
  { const std::string tin = sysinc_time_input(*r, res.depfile);
    if (!tin.empty()) { tm_refusals_++; note_skip(*r, "store-skipped", "time macro in a system header the TU may read: " + tin); return; } }
  const double tgo = r->t_go_wall > 0 ? r->t_go_wall : r->t_start_wall;
  const double t0 = r->t_pp_wall > 0 ? std::min(tgo, r->t_pp_wall) : tgo;
  std::vector<std::string> ps{closure::absolutize(r->src, r->cwd)};
  for (auto& p : closure::parse_depfile(res.depfile)) ps.push_back(closure::absolutize(p, r->cwd));
  const auto sts = direct::stat_many(ps);
  int64_t max_ct = 0; for (auto& st : sts) max_ct = std::max(max_ct, st.ctime_ns);
  char dec[256];   // decision record (trace only): every time involved, all on the file-stamping clock
  std::snprintf(dec, sizeof dec, "t_start=%.6f t_pp=%.6f t_go=%.6f t0=%.6f margin=%.4f max_input_ctime=%.6f", r->t_start_wall, r->t_pp_wall,
                r->t_go_wall, t0, direct::racy_margin_s(), static_cast<double>(max_ct) / 1e9);
  for (size_t i = 0; i < ps.size(); ++i)
    if (!sts[i].exists || direct::changed_since(sts[i], t0)) {
      note_trace(*r, "pp-store-decision", std::string("refused ") + dec);
      racy_store_skips_++; note_skip(*r, "store-skipped", "input changed after the compile could read it: " + ps[i]); return;
    }
  note_trace(*r, "pp-store-decision", std::string("accepted ") + dec);
  auto colon = res.depfile.find(':');
  if (!local_cache_.store(r->pp_okey, res.object, colon == std::string::npos ? std::string() : res.depfile.substr(colon + 1), res.out, res.err, res.rc)) return;
  try { util::mkdir_p(state_dir_ + "/pp"); util::write_file(pp_marker(r->lkey), "pp1\n"); } catch (...) {}
  pp_stores_++;
}

static bool in_dirs(const std::string& abs, const std::vector<std::string>& dirs) {
  for (auto& d : dirs) if (util::path_within(abs, d)) return true;
  return false;
}

bool Coordinator::store_client_cache(const std::shared_ptr<JobRec>& r, const proto::CompileResult& res) {
  if (!local_cache_.enabled() || r->lkey.empty() || r->has_include_computed) return false;
  // a flag that makes the compiler read a file no key can name (profile data, plugins, sanitizer
  // ignore-lists, response files, -B, --config, PCH/modules …) is refused by the direct path (direct::safe_args); the
  // client cache — and with it the write-back and the shared manifests — must refuse it as well
  { std::string bad; if (!direct::safe_args(r->argv, &bad)) { note_skip(*r, "store-skipped", "uncacheable-flag " + bad); return false; } }
  try {
    const bool raw_only = cachekey::keeps_comments(r->argv);
    // the comment-insensitive key maps comment-edited variants of a file to one entry. They
    // compile to the same object (no debug info, no comment-reading diagnostics — keeps_comments, content_key_sha),
    // but a diagnostic's COLUMN can move when a comment before it on the same line changes length: a result with any
    // compiler output is therefore never stored under that key (it is still served by the worker cache, raw-keyed).
    if (!raw_only && (!res.out.empty() || !res.err.empty())) { note_skip(*r, "store-skipped", "compiler output under the comment-insensitive key"); return false; }
    auto khash = [&](const std::string& abs) { return key_hash_file(abs, raw_only); };
    if (r->scan_abs.empty()) scan_only(*r);
    auto view = client_view(*r, [&](const std::vector<std::string>& v) { return key_hash_batch(v, raw_only); });
    if (view.empty()) return false;
    std::string src_abs = closure::absolutize(r->src, r->cwd);
    std::string src_sha = view[cachekey::relativise(src_abs, r->cwd)];
    std::unordered_map<std::string, std::string> abs2sha;   // key hashes (comment-insensitive) of the scanned closure
    for (auto& [rel, sh] : view) abs2sha[closure::absolutize(rel, r->cwd)] = sh;
    cachekey::Closure used; bool ok_dep = false;
    std::string body;
    if (!res.depfile.empty()) {   // the worker's depfile: exactly the inputs the compile read (as the worker cache does)
      ok_dep = true;
      const std::vector<std::string> sysd_fp = fp_dirs(r->cc_local);
      for (auto& pth : closure::parse_depfile(res.depfile)) {
        std::string ap = closure::absolutize(pth, r->cwd);
        if (ap == src_abs) continue;
        auto it = abs2sha.find(ap);
        if (it != abs2sha.end()) used.emplace_back(cachekey::relativise(ap, r->cwd), it->second);
        else if (!in_dirs(ap, sysd_fp)) {   // only the fingerprinted system directories are exempt
          // an input outside the scanned closure (computed include etc.): hash it now (stat-validated cache)
          try { used.emplace_back(cachekey::relativise(ap, r->cwd), khash(ap)); } catch (...) { ok_dep = false; break; }
        }
      }
      auto colon = res.depfile.find(':'); body = colon == std::string::npos ? "" : res.depfile.substr(colon + 1);
      // a -MMD dependency file omits the headers found through the project's own -isystem /
      // -idirafter directories. Outside the fingerprinted directories nothing else covers them: every scanned file
      // under such a directory enters the key (a superset of what the compile read there).
      if (ok_dep && !cachekey::dep_lists_system_headers(r->argv)) {
        const auto xs = sysinc::explicit_system_dirs(r->argv, r->cwd);
        if (!xs.empty()) {
          std::set<std::string> have; for (auto& u : used) have.insert(u.first);
          for (auto& [ap, sh] : abs2sha) {
            if (ap == src_abs || in_dirs(ap, sysd_fp) || !in_dirs(ap, xs)) continue;
            const std::string rel = cachekey::relativise(ap, r->cwd);
            if (have.insert(rel).second) used.emplace_back(rel, sh);
          }
        }
      }
    }
    // without a dependency file the key is the scanned closure, which never contains the target of a
    // computed #include MACRO — editing only that header would then hit the old object. Refused, as record_direct does.
    if (!ok_dep && r->closure_computed) { note_skip(*r, "store-skipped", "computed include without a dependency file"); return false; }
    // the scanner resolved includes in the coordinator's environment, not the client's
    if (!ok_dep && !r->env.empty()) { note_skip(*r, "store-skipped", "include-affecting environment without a dependency file"); return false; }
    if (!ok_dep) { used.clear(); for (auto& d : r->deps) { try { used.emplace_back(cachekey::relativise(d.path, r->cwd), khash(d.path)); } catch (...) {} } }
    used.emplace_back(cachekey::relativise(src_abs, r->cwd), src_sha);
    // a TU that expands __DATE__/__TIME__/__TIMESTAMP__ is not a function of its inputs' bytes (ccache
    // without sloppiness=time_macros refuses it too); before the stat check below, which also covers these reads
    for (auto& u : used)
      if (time_macros(closure::absolutize(u.first, r->cwd), u.second)) { note_skip(*r, "store-skipped", "time macro in " + u.first); return false; }
    // an inline-asm .incbin/.include in ANY input, not only the source
    if (r->scan.lang != "asm")
      for (auto& u : used)
        if (asm_directive(closure::absolutize(u.first, r->cwd), u.second)) { directive_skips_++; note_skip(*r, "store-skipped", "assembler file directive (.incbin/.include) in: " + u.first); return false; }
    // the system headers are left out of the key, so they are scanned by the fingerprint tracker
    // instead; if any of them uses a time macro, no TU of this compiler is stored (the dependency file need not list them)
    if (cachekey::argv_uses_time_macros(r->argv)) { note_skip(*r, "store-skipped", "time macro in a -D value"); return false; }
    { const std::string tin = sysinc_time_input(*r, res.depfile);
      if (!tin.empty()) { tm_refusals_++; note_skip(*r, "store-skipped", "time macro in a system header the TU may read: " + tin); return false; } }
    {   // the key must describe the bytes the compile READ. The hashes above were taken after the compile (local) or
        // after the worker got its copy (remote); they describe those bytes only if no input changed since before the
        // compile could read them: ctime < t0 - margin (direct.hpp changed_since; ccache's "too new" rule with a
        // margin for the coarse ctime clock), checked by a stat taken AFTER the hashes. t0: for a local compile, when
        // the client was told to run it; for a remote one, the request's arrival (the closure is hashed after it).
        // Otherwise nothing is stored — client cache, direct entry and worker write-back all follow from this store.
        const double t0 = r->t_go_wall > 0 ? r->t_go_wall : r->t_start_wall;
        std::vector<std::string> ps; ps.reserve(used.size());
        for (auto& u : used) ps.push_back(closure::absolutize(u.first, r->cwd));
        const auto sts = direct::stat_many(ps);
        for (size_t i = 0; i < ps.size(); ++i)
          if (!sts[i].exists || direct::changed_since(sts[i], t0)) {
            racy_store_skips_++; note_skip(*r, "store-skipped", "input changed after the compile could read it: " + ps[i]); return false;
          }
    }
    {   // never cache what an assembler file directive read (see file_directive_in)
      std::vector<std::string> ins;
      if (r->scan.lang == "asm") for (auto& u : used) ins.push_back(closure::absolutize(u.first, r->cwd));
      const std::string f = file_directive_in(r->scan.lang, src_abs, ins);
      if (!f.empty()) { directive_skips_++; note_skip(*r, "store-skipped", "assembler file directive (.incbin/.include) in: " + f); return false; }
    }
    {   // the system headers the key omits are pinned by the signature taken when the request arrived; a change
        // since then (an upgrade during the compile) means the object may not match either signature: nothing stored
      const std::string sig = sysinc_sig(r->cc_local);
      if (sig.empty() || sig != r->sysinc_sig) { note_skip(*r, "store-skipped", "system include signature changed or unavailable"); return false; }
      used.emplace_back("@sysinc", sig);
    }
    std::string okey = cachekey::object_key(r->lkey, used, r->cwd);
    if (!local_cache_.store(okey, res.object, body, res.out, res.err, res.rc)) return false;   // nothing written
    if (r->scan_ents.empty()) scan_only(*r);
    local_cache_.add_manifest_entry(r->lkey, used, okey, shape_fast(*r, cachekey::relativise(src_abs, r->cwd)));
    if (!r->dkey.empty()) record_direct_async(r, okey, res.depfile);
    if (util::getenv_or("OPTICC_DEBUG_CACHE", "") == "1") std::fprintf(stderr, "client-cache store %s: %zu paths used %zu dep_ok %d\n", r->src.c_str(), view.size(), used.size(), int(ok_dep));
    return true;
  } catch (...) {}
  return false;
}

// record a DIRECT entry (opticc/direct.hpp) mapping the wrapper's direct key to object `okey`, so the next identical
// compile is answered inside the compiler wrapper (no coordinator round trip, no scan, no hashing of unchanged files).
// Nothing is recorded on any doubt; the coordinator path stays the fallback:
//   * __has_include(MACRO) anywhere in the include graph (an absent name nobody can know), or a memoised parse that
//     no longer matches its file;
//   * a computed #include (#include MACRO) without a dependency file, or with one that omits system headers (-MMD)
//     unless the compile uses -nostdinc (see below);
//   * an input that vanished, is unreadable, or changed (ctime) less than kRacyS before the request arrived, or while
//     it was being hashed;
//   * a probed-absent include candidate that exists now;
//   * a negative directory that changed recently (VOLATILE) when the compile left no dependency file: a header that
//     was there during the compile and deleted since would not be among the positives otherwise.
// POSITIVES = source ∪ scanner closure ∪ files reachable only through system headers ∪ every path of the compile's
// dependency file (system headers included under -MD).
// Computed includes: their operand is a macro, so the scanner cannot resolve them; what they resolved to is in the
// dependency file. Every dependency the literal include graph does not explain (U) may have been reached through a
// computed include, under any name N = U relative to a candidate directory D (every search directory and the directory
// of every file holding a computed include), so X/N for every other candidate directory X is a negative (absent) — or,
// when it exists, it must be old (it existed during the compile and the compiler preferred U, so it is not earlier in
// the search order). Under -MMD the dependency file omits system headers; with -nostdinc the only system directories
// searched are the explicit -isystem/-I/-idirafter ones inside the compiler's system directories (the compiler's own
// resource directory in kbuild), so all files there are added to U and to the positives. Otherwise nothing is recorded.
static bool dep_lists_system_headers(const std::vector<std::string>& argv) { return cachekey::dep_lists_system_headers(argv); }   // shared with the worker

static void list_files_rec(const std::string& dir, std::vector<std::string>* out, size_t cap) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) { out->push_back(it->path().lexically_normal().string()); if (out->size() > cap) return; }
  }
}

// why an entry was not recorded (summary v4.skips: count + first example per reason; the reason is the text before
// the first ':' or '('), and one line in the OPTICC_DIRECT_TRACE file the wrappers write (kind: record-skipped |
// store-skipped).
// trace only (OPTICC_DIRECT_TRACE), no skip counting: decision records for diagnosis
void Coordinator::note_trace(const JobRec& r, const char* kind, const std::string& what) {
  static const std::string tp = util::getenv_or("OPTICC_DIRECT_TRACE", "");
  if (tp.empty()) return;
  char t[32]; std::snprintf(t, sizeof t, "%.3f", direct::wall_now());
  std::string line = std::string(t) + "\t" + kind + "\t" + what + "\t" + r.src + "\t" + r.dkey.substr(0, 12) + "\n";
  int fd = ::open(tp.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd >= 0) { (void)!::write(fd, line.data(), line.size()); ::close(fd); }
}

void Coordinator::note_skip(const JobRec& r, const char* kind, const std::string& why) {
  {
    std::lock_guard<std::mutex> g(rec_skip_mu_);
    std::string k = why.substr(0, why.find_first_of(":("));
    while (!k.empty() && k.back() == ' ') k.pop_back();
    auto& e = rec_skip_[k]; if (e.first++ == 0) e.second = r.src + ": " + why;
  }
  static const std::string tp = util::getenv_or("OPTICC_DIRECT_TRACE", "");
  if (tp.empty()) return;
  char t[32]; std::snprintf(t, sizeof t, "%.3f", direct::wall_now());
  std::string line = std::string(t) + "\t" + kind + "\t" + why + "\t" + r.src + "\t" + r.dkey.substr(0, 12) + "\n";
  int fd = ::open(tp.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd >= 0) { (void)!::write(fd, line.data(), line.size()); ::close(fd); }
}

// assembler file directives (.incbin, and in assembler files .include) read files that neither the include scan
// nor the dependency file lists, so no cache could see them change: such compiles are never cached (ccache refuses
// .incbin too). C/C++: the source is checked for .incbin (kernel/configs.c embeds config_data.gz with inline asm);
// assembler: the source and every file it read. Returns the offending file, or "".
static bool asm_include_directive(const std::string& c) {   // `.include` where the assembler reads it as a directive
  for (size_t p = c.find(".include"); p != std::string::npos; p = c.find(".include", p + 1)) {
    const size_t e = p + 8;
    if (e < c.size() && !std::isspace(static_cast<unsigned char>(c[e])) && c[e] != '"') continue;   // .included, .include_x
    size_t b = p; while (b > 0 && (c[b - 1] == ' ' || c[b - 1] == '\t')) --b;
    if (b == 0 || c[b - 1] == '\n' || c[b - 1] == ';' || c[b - 1] == '"') return true;   // line start, after ';', in asm("...")
  }
  return false;
}
static std::string file_directive_in(const std::string& lang, const std::string& src_abs, const std::vector<std::string>& inputs) {
  auto bad = [&](const std::string& f) {
    std::string c; try { c = util::read_file(f); } catch (...) { return true; }   // unreadable: do not cache
    return c.find(".incbin") != std::string::npos || (lang == "asm" && asm_include_directive(c));
  };
  if (bad(src_abs)) return src_abs;
  if (lang == "asm") for (auto& f : inputs) if (f != src_abs && bad(f)) return f;
  return {};
}

// the moment a local compile may start reading its inputs. The store rule (direct.hpp changed_since) accepts an
// input whose ctime C satisfies C < t_go - m. An input written at real time w has C <= w (ctime is stamped with the
// coarse real-time clock, which lags, never leads). Every input the build wrote BEFORE this request arrived (w <
// t_start_wall: make starts a compile only after the commands generating its inputs have exited) therefore passes when
// t_go >= t_start_wall + m: C <= w < t_start_wall <= t_go - m. Without the wait, a file generated just before its
// consumer (kbuild: .tmp_vmlinux0.kallsyms.S written by KSYMS right before AS, zoffset.h before header.S) would be
// refused on every build although its content never changes. Inputs written after t_go are still refused
// (C > w - lag >= t_go - m), so no assumption is added. Cost: at most m (≈ 4 ms on Linux) of latency, only when a
// store is possible and only when m is small (where m is the 0.25 s fallback, e.g. macOS, nothing waits).
void Coordinator::go_local(JobRec& r) {
  const double m = direct::racy_margin_s();
  if (local_cache_.enabled() && !r.lkey.empty() && m <= 0.02 && r.t_start_wall > 0) {
    const double wait = r.t_start_wall + m - direct::stamp_now();
    if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait + 1e-4));
  }
  r.t_go_wall = direct::stamp_now();
}

// a compile that is never distributed (assembler, Scan::local_cacheable) comes here only to use and fill the
// client cache: it is answered from it, or the client compiles it at once (no scheduling, no slot), and the result
// is stored like any other local compile.
void Coordinator::serve_local_only(const std::shared_ptr<JobRec>& r, const net::Socket& s) {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (inflight_ == 0) scanner_.new_epoch(now() - last_activity_);   // the build-epoch rule of scheduled jobs
  }
  // a distributable compile with an include-affecting environment is served here as well (cached, keyed
  // with the environment, never scheduled to a worker)
  const bool cacheable = r->scan.local_cacheable || (r->scan.distributable && !r->env.empty());
  if (cacheable && try_client_cache(r, s)) return;
  if (cacheable && try_remote_direct(r, s)) return;
  local_only_jobs_++;
  Hold hold(post_release_);   // the store and the job record below finish before a SIGTERM exit
  try {
    if (cacheable) go_local(*r); else r->t_go_wall = direct::stamp_now();   // the client's compiler reads its inputs after this
    send(s, {{"op", "decision"}, {"node", 0}, {"ack", true}});
    json done; if (!recv(s, &done)) return;
    r->rc = done.value("rc", -1); r->obj_bytes = done.value("obj_bytes", 0ULL);
    if (cacheable) store_local_result(r, done, s);
    else { try { send(s, {{"op", "stored"}}); } catch (...) {} }
  } catch (...) { return; }
  std::lock_guard<std::mutex> g(mu_);
  Job j; j.id = static_cast<int>(wl_.jobs.size()); j.name = r->src; j.src_size_bytes = 0; j.pre_size_bytes = 0;
  wl_.jobs.push_back(j); r->id = j.id; sched_->on_jobs_added();   // accounted, never offered to the scheduler
  r->t_request = r->t_decided = r->t_done = now(); r->outcome = "local-only"; r->cache_status = "local-only";
  jobs_.push_back(r); last_activity_ = now();
}

void Coordinator::record_direct(const std::shared_ptr<JobRec>& r, const std::string& okey, const std::string& depfile_text) {
  static const bool dbg = util::getenv_or("OPTICC_DEBUG_CACHE", "") == "1";
  auto skip = [&](const std::string& why) {
    if (dbg) std::fprintf(stderr, "direct: not recorded %s: %s\n", r->src.c_str(), why.c_str());
    note_skip(*r, "record-skipped", why);
  };
  if (r->dkey.empty() || okey.empty() || !local_cache_.enabled()) return;
  // the wrapper put the system-include signature it read into dkey; it must still be the current one
  if (r->sysinc_sig.empty() || r->sysinc_sig != sysinc_sig(r->cc_local)) return skip("system include signature changed or absent");
  const auto t0 = std::chrono::steady_clock::now(); rec_calls_++;
  struct Acc { std::atomic<uint64_t>& ns; std::chrono::steady_clock::time_point t; ~Acc() { ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count()); } } acc{rec_ns_, t0};
  auto tp = t0;
  auto mark = [&](int k) { auto n = std::chrono::steady_clock::now(); rec_phase_ns_[k] += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(n - tp).count()); tp = n; };
  try {
    if (r->scan_ents.empty() && r->scan_abs.empty()) scan_only(*r);
    mark(0);
    if (r->has_include_computed) return skip("computed __has_include");
    auto sysd = sys_dirs(r->cc_local, r->lang, r->cwd);
    const std::string src_abs = closure::absolutize(r->src, r->cwd);
    const auto dirs = args::include_dirs(r->argv);
    std::vector<std::string> extra, absent, computed; std::vector<std::pair<int, int>> negp; bool hic = false;
    scanner_.direct_inputs(src_abs, r->scan_abs, dirs, sysd, r->cwd, &extra, &negp, &computed, &hic);
    mark(1);
    if (hic) return skip("__has_include(MACRO) in the include graph");
    std::unordered_set<std::string> parsed; parsed.reserve(r->scan_abs.size() + extra.size() + 1);   // must match their memoised parse
    parsed.insert(src_abs); parsed.insert(r->scan_abs.begin(), r->scan_abs.end()); parsed.insert(extra.begin(), extra.end());
    std::vector<std::string> paths; paths.reserve(r->scan_abs.size() + extra.size() + 64);
    paths.push_back(src_abs);
    paths.insert(paths.end(), r->scan_abs.begin(), r->scan_abs.end());
    paths.insert(paths.end(), extra.begin(), extra.end());
    std::vector<std::string> deps;
    if (!depfile_text.empty()) for (auto& p : closure::parse_depfile(depfile_text)) deps.push_back(closure::absolutize(p, r->cwd));
    const bool have_dep = !deps.empty();
    // inputs must have been last changed before the compile could read them (direct.hpp changed_since): the
    // request's arrival, or for a local compile the moment the client was told to run it
    const double t0 = r->t_go_wall > 0 ? r->t_go_wall : r->t_start_wall;
    auto racy_why = [&](const direct::FStat& a, const std::string& p) {
      char b[96]; std::snprintf(b, sizeof b, "racy input (changed %.4f s before the compile could read it, margin %.4f s): ",
                                t0 - static_cast<double>(a.ctime_ns) / 1e9, direct::racy_margin_s());
      return std::string(b) + p;
    };
    const bool nostdinc = std::find(r->argv.begin(), r->argv.end(), "-nostdinc") != r->argv.end();
    // system headers outside the dependency file are found by the scanner in the coordinator's DEFAULT system
    // directories: only valid when the compile searches those (no sysroot / target / toolchain switches)
    if (!dep_lists_system_headers(r->argv) && !nostdinc && direct::changes_system_search(r->argv)) return skip("system include search differs from the default");
    {   // precompiled headers: GCC (and clang for -include) use X/N.gch|.pch next to a header N included at the top of the
        // source or forced with -include; every such candidate must be absent (a pch in use is not supported here)
      std::vector<std::string> names;
      // clang consults X.pch/X.gch only for -include X (GCC also for the #includes at the top of the source)
      const bool is_clang = util::basename(r->cc_local).find("clang") != std::string::npos || util::basename(r->argv[0]).find("clang") != std::string::npos;
      if (!is_clang) { try { for (auto& d : incscan::parse_text(util::read_file(src_abs)).incs) names.push_back(d.name); } catch (...) { return skip("source unreadable"); } }
      for (auto& f : dirs.forced) names.push_back(f);
      std::vector<std::string> cdirs{util::dirname(src_abs), closure::absolutize(".", r->cwd)};
      for (const auto* lst : {&dirs.quote, &dirs.bracket, &dirs.system, &dirs.after}) for (auto& d : *lst) cdirs.push_back(closure::absolutize(d, r->cwd));
      for (auto& d : sysd) cdirs.push_back(d);
      std::vector<std::string> pch;
      for (auto& n : names) {
        if (!n.empty() && n[0] == '/') { pch.push_back(n + ".gch"); pch.push_back(n + ".pch"); continue; }
        for (auto& x : cdirs) { std::string c = fs::path(x + "/" + n).lexically_normal().string(); pch.push_back(c + ".gch"); pch.push_back(c + ".pch"); }
      }
      std::sort(pch.begin(), pch.end()); pch.erase(std::unique(pch.begin(), pch.end()), pch.end());
      for (auto& c : pch) { if (::access(c.c_str(), F_OK) == 0) return skip("precompiled header candidate exists: " + c); absent.push_back(c); }
    }
    // a dependency the literal include graph does not explain gets the computed-include treatment even when the
    // scanner saw no computed include (C23 #embed, an include the scanner could not parse — e.g. in an assembler
    // file, where cpp tolerates unbalanced quotes in comments): otherwise nothing would probe the names that could
    // shadow it, and a header created earlier in the search path later would be missed.
    bool unexplained_dep = false;
    if (computed.empty() && have_dep) {
      std::unordered_set<std::string> known(paths.begin(), paths.end());
      for (auto& p : deps) if (!known.count(p)) { unexplained_dep = true; break; }
      if (unexplained_dep) unexplained_entries_++;
    }
    if (!computed.empty() || unexplained_dep) {
      if (!have_dep) return skip("computed include without a dependency file");
      const bool full = dep_lists_system_headers(r->argv);
      if (!full && std::find(r->argv.begin(), r->argv.end(), "-nostdinc") == r->argv.end()) return skip(computed.empty() ? "dependency not explained by the include graph, -MMD without -nostdinc" : "computed include, -MMD without -nostdinc");
      std::vector<std::string> cand;   // candidate directories (absolute, normalised)
      for (auto& d : dirs.quote) cand.push_back(closure::absolutize(d, r->cwd));
      for (auto& d : dirs.bracket) cand.push_back(closure::absolutize(d, r->cwd));
      for (auto& d : dirs.system) cand.push_back(closure::absolutize(d, r->cwd));
      for (auto& d : sysd) cand.push_back(d);
      for (auto& d : dirs.after) cand.push_back(closure::absolutize(d, r->cwd));
      for (auto& f : computed) cand.push_back(util::dirname(f));
      for (auto& d : cand) d = fs::path(d).lexically_normal().string();
      std::sort(cand.begin(), cand.end()); cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
      std::unordered_set<std::string> known(paths.begin(), paths.end());
      std::vector<std::string> unexplained;
      for (auto& p : deps) if (!known.count(p)) unexplained.push_back(p);
      if (!full) {   // -MMD + -nostdinc: every file of the explicitly searched system directories
        std::vector<std::string> sysfiles;
        for (const auto* lst : {&dirs.bracket, &dirs.system, &dirs.after})
          for (auto& d : *lst) {
            std::string a = closure::absolutize(d, r->cwd);
            bool in_sys = false; for (auto& sd : sysd) if (util::path_within(a, sd)) { in_sys = true; break; }
            if (in_sys) list_files_rec(a, &sysfiles, 20000);
          }
        if (sysfiles.size() > 20000) return skip(computed.empty() ? "dependency not explained by the include graph, system directory too large" : "computed include, system directory too large");
        for (auto& f : sysfiles) if (!known.count(f)) { unexplained.push_back(f); paths.push_back(f); }
      }
      for (auto& u : unexplained) {
        for (auto& d : cand) {
          if (!util::path_within(u, d) || u.size() <= d.size() + 1) continue;
          const std::string name = u.substr(d.size() + 1);
          for (auto& x : cand) {
            if (x == d) continue;
            std::string c = fs::path(x + "/" + name).lexically_normal().string();
            if (c == u) continue;
            direct::FStat st = direct::stat_path(c);
            if (!st.exists) absent.push_back(c);
            else if (direct::changed_since(st, t0)) return skip("computed-include candidate changed recently: " + c);
          }
        }
      }
      std::sort(absent.begin(), absent.end()); absent.erase(std::unique(absent.begin(), absent.end()), absent.end());
    }
    paths.insert(paths.end(), deps.begin(), deps.end());
    mark(2);
    // grouped by directory (then name): the wrapper stats them with one directory fd per group (direct.cpp DirStatter)
    std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
      const auto sa = a.rfind('/'), sb = b.rfind('/'); const int c = a.compare(0, sa, b, 0, sb); return c != 0 ? c < 0 : a.compare(sa, std::string::npos, b, sb, std::string::npos) < 0; });
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    direct::Entry e; e.obj = okey; e.recorded_wall = direct::wall_now(); e.pos.reserve(paths.size());
    e.keysha = !cachekey::keeps_comments(r->argv);   // must match the wrapper's source hash in the direct key
    // one stat() per input; hashes come from the stat-validated caches under ONE lock (exact (mtime, size, inode,
    // ctime) match with that stat); only cache misses are read, then re-stat()ed to prove they did not change meanwhile
    e.pos.resize(paths.size());
    {
      const std::vector<direct::FStat> sts = direct::stat_many(paths);   // one directory fd per directory run
      std::vector<char> check(paths.size(), 0);
      for (size_t i = 0; i < paths.size(); ++i) {
        const std::string& p = paths[i]; const direct::FStat& a = sts[i];
        if (!a.exists || a.is_dir) return skip("input missing: " + p);
        if (direct::changed_since(a, t0)) return skip(racy_why(a, p));
        check[i] = parsed.count(p) ? 1 : 0;
        e.pos[i].path = p; e.pos[i].st = a;
      }
      const long bad = scanner_.first_stale_parse(paths, sts, check);   // one shared lock for all
      if (bad >= 0) return skip("memoised parse is stale: " + paths[static_cast<size_t>(bad)]);
    }
    mark(3);
    std::vector<size_t> miss;
    if (e.keysha) {
      std::shared_lock<std::shared_mutex> g(khmu_);
      for (size_t i = 0; i < e.pos.size(); ++i) {
        auto it = khash_cache_.find(e.pos[i].path); const auto& a = e.pos[i].st;
        if (it != khash_cache_.end() && it->second.mtime_ns == a.mtime_ns && it->second.size == a.size && it->second.ino == a.ino && it->second.ctime_ns == a.ctime_ns) e.pos[i].sha = it->second.sha;
        else miss.push_back(i);
      }
    } else {
      std::lock_guard<std::mutex> g(hmu_);
      for (size_t i = 0; i < e.pos.size(); ++i) {
        auto it = hash_cache_.find(e.pos[i].path); const auto& a = e.pos[i].st;
        if (it != hash_cache_.end() && it->second.mtime_ns == a.mtime_ns && it->second.size == a.size && it->second.ino == a.ino && it->second.ctime_ns == a.ctime_ns) e.pos[i].sha = it->second.sha;
        else miss.push_back(i);
      }
    }
    for (size_t i : miss) {
      auto& ps = e.pos[i];
      ps.sha = e.keysha ? key_hash_file(ps.path, false) : hash_file(ps.path, nullptr);
      if (!direct::same(direct::stat_path(ps.path), ps.st)) return skip("input changed while hashing: " + ps.path);
    }
    mark(4);
    scanner_.intern_candidates(absent, &negp);   // computed-include candidates
    if (!scanner_.negative_groups(negp, r->t_start_wall, &e.neg)) return skip("an absent include candidate exists now");
    if (!have_dep) for (auto& g : e.neg) if (g.volatile_dir) return skip("volatile include directory without a dependency file: " + g.dir);
    mark(5);
    direct::record(local_cache_.dir(), r->dkey, e); rec_ok_++;
    rd_publish_async(r, e);   // share it (machine-independent form) through the worker tier
    mark(6);
    if (dbg) std::fprintf(stderr, "direct: recorded %s: %zu positives, %zu negative dirs, %zu absent names\n", r->src.c_str(), e.pos.size(), e.neg.size(), negp.size());
  } catch (const std::exception& ex) { skip(ex.what()); }
}

void Coordinator::record_direct_async(const std::shared_ptr<JobRec>& r, const std::string& okey, const std::string& dep) {
  std::lock_guard<std::mutex> g(rec_qmu_);
  if (rec_q_.size() > 100000) { rec_dropped_++; return; }   // never unbounded
  rec_q_.push_back(RecTask{r, okey, dep}); rec_qcv_.notify_one();
}

// background bookkeeping (direct-entry recording, write-back uploads) runs at nice 19, but on a small client it
// still competes with the local compiles, so it waits while every client compile slot is taken. Bounded: never once the queue holds 4,096 tasks
// (then it runs as before), never at exit (stopping_ / up_draining_: the exit drains keep every entry for the next
// build), and never longer than 60 s for one task. OPTICC_BG_DEFER=off disables it. Summary v4.bg_deferred_s.
void Coordinator::yield_to_compiles(const std::function<size_t()>& queued) {
  static const bool on = util::getenv_or("OPTICC_BG_DEFER", "on") != "off";
  if (!on) return;
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (stopping_.load() || up_draining_.load() || queued() >= 4096) break;
    bool full; { std::lock_guard<std::mutex> g(mu_); full = sched_ && sched_->client_saturated(); }
    if (!full || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  bg_defer_ns_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

void Coordinator::record_worker(bool background) {
  if (background) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#else
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);   // this thread only (Linux)
#endif
  }
  for (;;) {
    RecTask t;
    if (background) {
      { std::unique_lock<std::mutex> l(rec_qmu_); rec_qcv_.wait(l, [&] { return !rec_q_.empty(); }); }
      yield_to_compiles([&] { std::lock_guard<std::mutex> g(rec_qmu_); return rec_q_.size(); });
    }
    {
      std::unique_lock<std::mutex> l(rec_qmu_);
      if (background) rec_qcv_.wait(l, [&] { return !rec_q_.empty(); });
      else if (rec_q_.empty()) return;   // drain helpers stop when the queue is empty
      t = std::move(rec_q_.front()); rec_q_.pop_front(); rec_busy_++;
    }
    record_direct(t.r, t.okey, t.dep);
    { std::lock_guard<std::mutex> g(rec_qmu_); rec_busy_--; } rec_qcv_.notify_all();
  }
}

void Coordinator::drain_records() {
  const double limit = std::atof(util::getenv_or("OPTICC_RECORD_DRAIN_S", "120").c_str());
  const double t0 = util::now_s();
  std::vector<std::thread> helpers;
  const unsigned n = std::max(2u, std::thread::hardware_concurrency());
  for (unsigned i = 0; i < n; ++i) helpers.emplace_back(&Coordinator::record_worker, this, false);
  for (auto& h : helpers) h.join();
  std::unique_lock<std::mutex> l(rec_qmu_);
  rec_qcv_.wait_for(l, std::chrono::duration<double>(std::max(0.0, limit - (util::now_s() - t0))), [&] { return rec_q_.empty() && rec_busy_ == 0; });
}

// a successful LOCAL compile (localhost-last or fallback) is stored in the client cache exactly like a
// remote result: object from the output path, the compiler's own dependency file (the inputs it actually read), and the
// stdout/stderr the client captured. Not stored when the client could not capture output (terminal), in classic mode
// (classic keys need the .i), or when the closure has a computed __has_include. Runs after the client has exited.
void Coordinator::store_local_result(const std::shared_ptr<JobRec>& r, const json& done, const net::Socket& s) {
  // the object and the dependency file are read while the client still waits (see wait_stored in main.cpp), then
  // the client is released and the (slower) store + direct recording run afterwards
  proto::CompileResult res; bool have = false;
  if (local_cache_.enabled() && !r->lkey.empty() && done.value("rc", -1) == 0 && done.value("captured", false) &&
      topo_.params.mode_policy != ModePolicy::Classic && (r->scan.distributable || r->scan.local_cacheable)) {
    try {
      res.object = util::read_file(closure::absolutize(r->out, r->cwd));
      if (r->scan.has_md) {
        std::string dp = r->scan.dep_file.empty() ? args::default_dep_name(r->out) : r->scan.dep_file;
        res.depfile = util::read_file(closure::absolutize(dp, r->cwd));
      }
      res.out = done.value("stdout", std::string()); res.err = done.value("stderr", std::string());
      res.rc = 0; res.status = "ok"; have = true;
    } catch (...) {}
  }
  try { send(s, {{"op", "stored"}}); } catch (...) {}
  if (!have) return;
  { static const int dly = std::atoi(util::getenv_or("OPTICC_TEST_STORE_DELAY_MS", "0").c_str());   // test hook
    if (dly > 0) std::this_thread::sleep_for(std::chrono::milliseconds(dly)); }
  try {
    if (!r->closure_built) { build_closure(*r); r->closure_built = true; }
    if (r->has_include_computed) { store_pp(r, res); return; }   // preprocessed key; never uploaded (no closure key)
    if (store_client_cache(r, res) && r->scan.distributable) upload_async(r, res);   // write-back to a worker
  } catch (...) {}
}

static uint64_t fnv64(const std::string& s);

void Coordinator::upload_async(const std::shared_ptr<JobRec>& r, const proto::CompileResult& res) {
  static const bool on = util::getenv_or("OPTICC_UPLOAD", "on") != "off";
  static const uint64_t cap = static_cast<uint64_t>(std::max(1.0, std::atof(util::getenv_or("OPTICC_UPLOAD_QUEUE_MB", "512").c_str())) * 1048576.0);
  if (!on || conns_.empty() || r->cache_key.empty() || res.object.empty() || !r->env.empty()) return;   // never under the worker's key
  std::lock_guard<std::mutex> g(up_mu_);
  const uint64_t b = res.object.size() + res.depfile.size() + 256 * r->deps.size();
  if (up_q_bytes_ + b > cap) { up_dropped_++; return; }   // never unbounded
  up_q_.push_back(UpTask{r, res}); up_q_bytes_ += b; up_queued_++; up_cv_.notify_one();
}

int Coordinator::upload_one(UpTask& t) {
  JobRec& r = *t.r;
  {   // a worker already holds it (this session's results) or very likely does (its HELLO Bloom filter)
    // "held" means the exact closure content (full key); unknown full key -> upload
    bool held = false;
    if (!r.full_key.empty()) { { std::lock_guard<std::mutex> g(mu_); held = !cache_idx_.holders(r.full_key).empty(); } if (!held) held = !bloom_holders(r.full_key).empty(); }
    if (held) { up_skipped_held_++; return 1; }
  }
  std::vector<int> cands;
  for (size_t k = 1; k <= conns_.size(); ++k) {
    auto& c = conns_[k - 1];
    if (!c->connected() || sched_->node_disabled(static_cast<int>(k))) continue;
    if (!c->hello_ack().contains("objcache_bloom")) continue;   // a worker without a compile cache
    cands.push_back(static_cast<int>(k));
  }
  if (cands.empty()) return 0;
  const int node = cands[fnv64(r.cache_key) % cands.size()];
  // the inputs must be unchanged since before the compile could read them (direct.hpp changed_since): then the shas the
  // closure was hashed with (after the compile) describe the bytes the compile used
  const double t0 = r.t_go_wall > 0 ? r.t_go_wall : r.t_start_wall;
  std::vector<proto::FileRef> deps; { std::lock_guard<std::mutex> g(r.m); deps = r.deps; }
  const std::string src_abs = closure::absolutize(r.src, r.cwd);
  proto::CachePutMsg m;
  // HASH FIRST, then stat (the stat must follow every hash it vouches for). The closure's shas were taken when the job's closure was built; the source is hashed here; the stat
  // below then proves that no input changed since before the compile could read it (ctime < t0 - m).
  // EVERY closure hash the upload vouches for is (re)taken now — after the compile — and must equal
  // the one the closure carries; the stat below then follows all of them. Otherwise a scan-time hash of a header
  // edited before the compile read it could be uploaded.
  // hash_file is stat-validated (cached only for an exact stat), so this costs one stat per input in the common case.
  if (util::getenv_or("OPTICC_TEST_UPLOAD_STALE_DEPS", "") == "1")   // test hook
    for (auto& d : deps) d.sha = std::string(64, '0');
  for (auto& d : deps) {
    std::string now;
    try { now = hash_file(d.path, nullptr); } catch (...) { up_skipped_racy_++; return 1; }
    if (now != d.sha) { up_skipped_racy_++; return 1; }
  }
  try { m.src_sha = hash_file(src_abs, nullptr); } catch (...) { up_skipped_racy_++; return 1; }
  {   // test hook: announce, then pause between the hash and the stat
    static const int gap_ms = std::atoi(util::getenv_or("OPTICC_TEST_UPLOAD_GAP_MS", "0").c_str());
    if (gap_ms > 0) {
      try { util::write_file(util::getenv_or("OPTICC_TEST_UPLOAD_GAP_FILE", "/dev/null"), "gap\n"); } catch (...) {}
      std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
    }
  }
  {
    std::vector<std::string> paths; paths.reserve(deps.size() + 1); paths.push_back(src_abs);
    for (auto& d : deps) paths.push_back(d.path);
    for (auto& st : direct::stat_many(paths)) if (!st.exists || direct::changed_since(st, t0)) { up_skipped_racy_++; return 1; }
  }
  // the identities of the compiler that produced the object and of its system headers; the worker
  // stores the upload only when both equal its own (it files the object under ITS compiler's key)
  m.compiler_id = cachekey::compiler_id(r.cc_local.empty() ? (r.argv.empty() ? std::string() : r.argv[0]) : r.cc_local);
  m.sysinc_fp = sysinc_content(r.cc_local);
  if (m.compiler_id.empty() || m.sysinc_fp.empty()) { up_failed_++; return 1; }
  m.argv = r.argv; m.cwd = r.cwd; m.src_path = r.src; m.deps = std::move(deps); m.depfile = t.res.depfile;
  m.out = t.res.out; m.err = t.res.err; m.rc = t.res.rc; m.object = t.res.object;
  m.obj_sha = util::sha256_hex(m.object);
  uint64_t sent = 0;
  try { conns_[static_cast<size_t>(node - 1)]->send_cache_put(m, 1, &sent); }
  catch (const std::exception&) { up_failed_++; return 1; }
  up_sent_++; up_bytes_ += sent;
  // NOT recorded as held: CACHE_PUT has no acknowledgement and the worker may refuse the store
  // (toolchain or system-include mismatch, disk), so later jobs of this session must not see a certain (p = 1) hit
  // there. The next HELLO's Bloom filter tells.
  if (!up_draining_.load()) {   // pace: the build's own traffic first
    static const double cap_bps = std::max(1.0, std::atof(util::getenv_or("OPTICC_UPLOAD_MBPS", "100").c_str())) * 1e6;
    double bw; { std::lock_guard<std::mutex> g(mu_);
      bw = (static_cast<size_t>(node) < live_up_bps_.size() && live_up_bps_[static_cast<size_t>(node)] > 0) ? live_up_bps_[static_cast<size_t>(node)] : topo_.node(node).bw_up_bps; }
    const double rate = std::min(cap_bps, bw > 0 ? 0.2 * bw : cap_bps);
    std::this_thread::sleep_for(std::chrono::duration<double>(8.0 * static_cast<double>(sent) / rate));
  }
  return 1;
}

std::string Coordinator::sysinc_sig(const std::string& cc_local) {
  if (!sysinc_ || cc_local.empty()) return "";
  std::error_code ec; std::string c = fs::canonical(cc_local, ec).string(); if (ec || c.empty()) c = cc_local;
  try {   // the stat walk (one lstat per system header, ≈50-100 ms on Linux) runs once per compiler per coordinator run,
          // normally already done by the start-up pre-warm; blocking here keeps a cold build's first compiles cacheable
    const sysinc::State st = sysinc_->get(c, false);
    if (!st.ready) return "";
    publish_sysinc(c, st.stat_sig);
    return st.stat_sig;
  } catch (...) { return ""; }
}

// worker-cache affinity from the FULL content key — the exact view the worker matches
// (source, shipped closure, its "@sysinc"), so a header edit changes the key of every TU that includes the header and
// no stale holder attracts them. A key the worker reported this session (result index) is a certain hit (p = 1); a
// Bloom-filter hit has p < 1: the filter's false-positive rate at 10 bits/key is ≈0.8 % and an entry can have been
// evicted since the worker's HELLO. p is LEARNED (Scheduler::learned_hit_p): 0.95 before any evidence, then the
// observed precision of Bloom hints on jobs that ran on a holder. Below Scheduler::kCertainHit the scheduler uses the
// EXPECTED completion p·T_hit + (1-p)·T_miss and reserves a core and a window slot as for a miss: stale hints (misses)
// keep it there, so they never skew placement. Once hints prove right (~30 real hits, the fresh-client case) they are
// trusted like certain hits and no longer fill the holders' windows.
void Coordinator::set_full_key_affinity(const std::shared_ptr<JobRec>& r) {
  if (r->cache_key.empty() || r->mode != Mode::HeaderPush) return;
  // the content fingerprint comes from the start-up pre-warm (sysinc_warm); a request that arrives before it is ready
  // waits for it (bounded), so a cold coordinator's first requests are probed like the rest. Past the bound the job
  // simply gets no affinity: placement only, every cache key and store rule is unchanged.
  std::string sys;
  {
    std::error_code ec; std::string c = fs::canonical(r->cc_local, ec).string(); if (ec || c.empty()) c = r->cc_local;
    if (sysinc_ && !c.empty()) {
      try { sys = sysinc_->peek(c, true).content_fp; } catch (...) {}
      if (sys.empty()) {
        sysinc_before_ready_++;
        // (1) when no worker advertised any cached key (every HELLO Bloom filter empty, no result
        // indexed: a clean build against cold workers) there is nothing affinity could find — no wait at all;
        // (2) otherwise the job gives its N^pp admission slot back while it waits (Scheduler::park), so a cold start
        // does not hold every admission and stall the whole build for the scan. The fingerprint is still warmed
        // (sysinc_warm) so later requests find it ready.
        if (!any_worker_keys()) { sysinc_skipped_wait_++; (void)sysinc_warm(c); aff_unready_++; return; }
        { std::lock_guard<std::mutex> g(mu_); sched_->park(r->id); run_decide_locked(); }
        static const double wait_s = std::max(0.0, std::atof(util::getenv_or("OPTICC_SYSINC_WAIT_S", "10").c_str()));
        const auto w0 = std::chrono::steady_clock::now();
        if (sysinc_warm(c).wait_for(std::chrono::duration<double>(wait_s)) != std::future_status::ready) sysinc_wait_timeouts_++;
        sysinc_wait_ns_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count());
        try { sys = sysinc_->peek(c, true).content_fp; } catch (...) {}
      }
    }
  }
  if (sys.empty()) { aff_unready_++; return; }
  aff_probed_++;
  std::vector<std::pair<std::string, std::string>> view;
  try {
    const std::string src_abs = closure::absolutize(r->src, r->cwd);
    view.emplace_back(cachekey::relativise(src_abs, r->cwd), hash_file(src_abs, nullptr));
    std::lock_guard<std::mutex> g(r->m);
    for (auto& d : r->deps) view.emplace_back(cachekey::relativise(d.path, r->cwd), d.sha);
  } catch (...) { return; }
  view.emplace_back("@sysinc", sys);
  r->full_key = cachekey::full_key(r->cache_key, view);
  std::lock_guard<std::mutex> g(mu_);
  auto h = cache_idx_.holders(r->full_key);
  double p = 1.0;
  if (!h.empty()) { r->affinity = 1; aff_index_jobs_++; }
  std::vector<double> ps;
  if (!h.empty()) ps.assign(h.size(), p);
  else {
    h = bloom_holders(r->full_key);
    for (int k : h) ps.push_back(trust_.p(k));   // each holder's own learned precision
    if (!h.empty()) { r->affinity = 2; bloom_affinity_jobs_++; }
  }
  r->aff_nodes = h;
  if (!h.empty()) sched_->set_affinity(r->id, h, cacheidx::kDefaultHitCost, ps);
}

std::shared_future<void> Coordinator::sysinc_warm(const std::string& c) {
  std::lock_guard<std::mutex> g(sysinc_warm_mu_);
  auto it = sysinc_warming_.find(c);
  if (it != sysinc_warming_.end()) return it->second;
  auto f = std::async(std::launch::async, [this, c] {
    const double t0 = now();
    try { (void)sysinc_->get(c, true); } catch (...) {}   // stat signature + content fingerprint (memo-assisted)
    std::lock_guard<std::mutex> g2(sysinc_warm_mu_);
    sysinc_timing_[c] = SysincTiming{now(), now() - t0};
  }).share();
  sysinc_warming_[c] = f;
  return f;
}

// the system directories the system-include fingerprint covers for `cc_local` (sysinc::dirs_for of the
// canonical compiler, cached per coordinator run); only inputs inside them may be left out of a key
std::vector<std::string> Coordinator::fp_dirs(const std::string& cc_local) {
  if (cc_local.empty()) return {};
  std::error_code ec; std::string c = fs::canonical(cc_local, ec).string(); if (ec || c.empty()) c = cc_local;
  { std::lock_guard<std::mutex> g(hmu_); auto it = fpdirs_cache_.find(c); if (it != fpdirs_cache_.end()) return it->second; }
  std::vector<std::string> d;
  try { d = sysinc::dirs_for(c); } catch (...) { return {}; }   // not cached on failure: the next call retries
  if (d.empty()) return d;
  std::lock_guard<std::mutex> g(hmu_); fpdirs_cache_[c] = d; return d;
}

// a fingerprinted system header of this compiler uses a time macro (or the state is unknown)
std::string Coordinator::sysinc_time_input(const JobRec& r, const std::string& depfile_text) {
  if (!sysinc_ || r.cc_local.empty()) return "(unknown)";
  std::error_code ec; std::string c = fs::canonical(r.cc_local, ec).string(); if (ec || c.empty()) c = r.cc_local;
  try {
    sysinc_warm(c).wait();   // one computation per compiler, shared (no get() per store)
    const sysinc::State st = sysinc_->peek(c, true);
    if (!st.ready || st.content_fp.empty()) return "(unknown)";
    if (st.time_macro_files.empty()) return "";
    { std::lock_guard<std::mutex> g(tmf_mu_); for (auto& f : st.time_macro_files) if (tm_files_seen_.size() < 64) tm_files_seen_.insert(f); }
    std::vector<std::string> dep_abs;
    try { for (auto& p : closure::parse_depfile(depfile_text)) dep_abs.push_back(closure::absolutize(p, r.cwd)); } catch (...) { return "(unparsable dependency file)"; }
    return sysinc::time_macro_input(st.time_macro_files, fp_dirs(r.cc_local), r.argv, r.cwd, dep_abs,
                                    !depfile_text.empty() && dep_lists_system_headers(r.argv));
  } catch (...) { return "(unknown)"; }
}

std::string Coordinator::sysinc_content(const std::string& cc_local) {
  if (!sysinc_ || cc_local.empty()) return "";
  std::error_code ec; std::string c = fs::canonical(cc_local, ec).string(); if (ec || c.empty()) c = cc_local;
  // wait on the shared warm-up future (computed once per compiler), never a synchronous get() per call
  try { sysinc_warm(c).wait(); const sysinc::State st = sysinc_->peek(c, true); return st.ready ? st.content_fp : std::string(); } catch (...) { return ""; }
}

// <sock>.sysinc.<H(compiler identity)> = "<coordinator pid>\n<stat signature>\n". The wrapper puts the signature
// into its direct key and trusts the file only while that pid is alive (a coordinator removes its files at start and at
// exit; the tracker rewrites them within its refresh period after a system-header change).
void Coordinator::publish_sysinc(const std::string& canonical_cc, const std::string& sig) {
  {
    std::lock_guard<std::mutex> g(sysinc_mu_);
    auto it = sysinc_published_.find(canonical_cc);
    if (it != sysinc_published_.end() && it->second == sig) return;
    sysinc_published_[canonical_cc] = sig;
  }
  try {
    const std::string path = o_.sock_path + ".sysinc." + util::sha256_hex(direct::compiler_identity(canonical_cc)).substr(0, 24);
    util::write_file(path + ".tmp", std::to_string(::getpid()) + "\n" + sig + "\n");
    std::rename((path + ".tmp").c_str(), path.c_str());
  } catch (...) {}
}

static void unlink_sysinc_files(const std::string& sock_path) {
  const std::string dir = util::dirname(sock_path), prefix = util::basename(sock_path) + ".sysinc.";
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d)) if (std::string(e->d_name).rfind(prefix, 0) == 0) ::unlink((dir + "/" + e->d_name).c_str());
    ::closedir(d);
  }
}

void Coordinator::upload_worker() {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#else
  ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
#endif
  for (;;) {
    UpTask t;
    { std::unique_lock<std::mutex> l(up_mu_); up_cv_.wait(l, [&] { return !up_q_.empty(); }); }
    yield_to_compiles([&] { std::lock_guard<std::mutex> g(up_mu_); return up_q_.size(); });
    {
      std::unique_lock<std::mutex> l(up_mu_);
      up_cv_.wait(l, [&] { return !up_q_.empty(); });
      t = std::move(up_q_.front()); up_q_.pop_front(); up_busy_++;
    }
    int rc = 0;
    try { rc = upload_one(t); } catch (...) { up_failed_++; rc = 1; }
    std::unique_lock<std::mutex> l(up_mu_);
    up_busy_--;
    if (rc == 0) {   // no worker with a cache connected yet (late admission): keep it, retry in a while
      up_q_.push_front(std::move(t));
      l.unlock(); up_cv_.notify_all();
      std::this_thread::sleep_for(std::chrono::milliseconds(up_draining_.load() ? 100 : 500));
      continue;
    }
    up_q_bytes_ -= std::min(up_q_bytes_, static_cast<uint64_t>(t.res.object.size() + t.res.depfile.size() + 256 * t.r->deps.size()));
    l.unlock(); up_cv_.notify_all();
  }
}

void Coordinator::drain_uploads() {
  const double limit = std::atof(util::getenv_or("OPTICC_UPLOAD_DRAIN_S", "20").c_str());
  up_draining_ = true;
  std::unique_lock<std::mutex> l(up_mu_);
  up_cv_.wait_for(l, std::chrono::duration<double>(std::max(0.0, limit)), [&] { return up_q_.empty() && up_busy_ == 0; });
  up_dropped_ += up_q_.size();   // abandoned (reported in the summary)
}

// ---- verify mode ------------------------------------------------------------------------
// Launch a verifying duplicate of a remote job on another worker with probability verify_frac.
// The duplicate is not visible to the scheduler's admission window (kept small by construction).
bool Coordinator::maybe_verify(const std::shared_ptr<JobRec>& r, int primary) {
  if (o_.verify_frac <= 0 || topo_.nodes.size() < 2) return false;
  int alt = -1;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (std::uniform_real_distribution<double>(0, 1)(vrng_) >= o_.verify_frac) return false;
    std::vector<int> cands;
    for (auto& n : topo_.nodes) if (n.id != primary && !sched_->node_disabled(n.id) && conns_[static_cast<size_t>(n.id - 1)]->connected()) cands.push_back(n.id);
    if (cands.empty()) return false;
    alt = cands[std::uniform_int_distribution<size_t>(0, cands.size() - 1)(vrng_)];
    verify_jobs_++;
  }
  { std::lock_guard<std::mutex> g(r->m); r->verify = true; }
  launch_attempt(r, alt, true);
  log({{"event", "verify-launch"}, {"job", r->id}, {"primary", primary}, {"verifier", alt}});
  return true;
}

// Reference object from a local compile of the same input (classic: the .i file with the worker's argv
// rewrite; header-push: the original command with a temporary output). Empty on failure.
std::string Coordinator::local_reference_sha(const JobRec& r) {
  try {
    std::string dir = util::make_temp_dir("opticc-verify-");
    std::string ofile = dir + "/ref.o";
    std::vector<std::string> argv;
    if (r.mode == Mode::HeaderPush || r.ifile.empty()) {
      argv = r.argv; argv[0] = r.cc_local;
      for (size_t i = 0; i + 1 < argv.size(); ++i) if (argv[i] == "-o") { argv[i + 1] = ofile; break; }
    } else {
      argv = args::remote_argv(r.argv, r.scan, r.ifile, ofile); argv[0] = r.cc_local;
    }
    util::RunResult rr = util::run_process(argv, dir + "/out", dir + "/err", r.cwd);
    std::string sha = (rr.rc == 0 && util::file_exists(ofile)) ? util::sha256_hex(util::read_file(ofile)) : "";
    util::remove_tree(dir);
    return sha;
  } catch (...) { return ""; }
}

// A worker caught lying may have delivered wrong objects before detection (verification samples).
// Quarantine: every completed job whose object came from a liar is recompiled locally in place with
// its original command line. Later results from the liar are rejected (see serve_client).
void Coordinator::quarantine(const std::vector<int>& liars) {
  std::vector<std::shared_ptr<JobRec>> redo;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& j : jobs_) {
      if (j->outcome != "remote" || j->t_done == 0) continue;
      for (int n : liars) if (j->result_node == n) { redo.push_back(j); break; }
    }
  }
  for (auto& j : redo) {
    std::vector<std::string> argv = j->argv; argv[0] = j->cc_local;
    util::RunResult rr = util::run_process(argv, "", "", j->cwd);
    fputs(("opticc: quarantine: recompiled " + j->src + " locally (rc " + std::to_string(rr.rc) + ")\n").c_str(), stderr);
    std::lock_guard<std::mutex> g(mu_);
    verify_recompiled_++;
    j->outcome = rr.rc == 0 ? "quarantined" : "quarantine-failed";
  }
}

// Called once all attempts of a verify job are in. Picks the result to deliver and disables liars.
void Coordinator::resolve_verify(const std::shared_ptr<JobRec>& r, proto::CompileResult* res, bool* ok) {
  std::vector<JobRec::VResult> vr; { std::lock_guard<std::mutex> g(r->m); vr = r->vresults; }
  if (vr.size() < 2) return;   // verifier failed to run at all: nothing to compare
  bool agree = vr[0].status == vr[1].status && vr[0].rc == vr[1].rc && vr[0].sha == vr[1].sha;
  if (agree) { log({{"event", "verify-ok"}, {"job", r->id}}); return; }
  std::string ref = local_reference_sha(*r);
  std::vector<int> liars;
  for (auto& v : vr) if (ref.empty() || v.sha != ref || v.status != "ok") liars.push_back(v.node);
  {
    std::lock_guard<std::mutex> g(mu_);
    verify_mismatches_++;
    for (int n : liars) {
      node_verify_failed_[static_cast<size_t>(n)]++;
      if (!sched_->node_disabled(n)) { sched_->disable_node(n); cache_idx_.forget_node(n); pf_.forget_node(n); }
      fputs(("opticc: VERIFY_MISMATCH worker=" + topo_.node(n).name + " job=" + r->src + " (worker disabled for this build)\n").c_str(), stderr);
    }
  }
  { std::lock_guard<std::mutex> g(r->m); r->verify_mismatch = true; }
  quarantine(liars);
  // deliver the attempt that agrees with the local reference, else fall back to local
  for (auto& v : vr) if (!ref.empty() && v.sha == ref && v.status == "ok") {
    std::lock_guard<std::mutex> g(r->m);
    if (r->have_result && r->result_node == v.node) { *res = r->result; *ok = true; return; }
    if (r->verify_alt_ok && r->verify_alt_node == v.node) { *res = r->verify_alt; *ok = true; return; }
  }
  *ok = false;
  { std::lock_guard<std::mutex> g(r->m); r->last_error = "verify mismatch: no worker agrees with the local reference"; }
}

void Coordinator::run_decide_locked() {
  // decide until quiescent: a binding frees admission capacity for further decisions at the same instant
  for (int round = 0; round < 64; ++round) {
    std::vector<Decision> ds = sched_->decide(now());
    if (ds.empty()) break;
    for (const Decision& d : ds) {
      auto r = jobs_.at(static_cast<size_t>(d.job));
      if (d.bind) { r->node = d.node; if (d.node == 0) r->mode = Mode::Classic; r->bound = true; r->t_decided = now(); }
      else if (!d.hedge) {
        r->node = d.node; r->mode = d.node == 0 ? Mode::Classic : d.mode; r->decided = true; r->t_decided = now();
        r->bound = d.node != kUnbound;
      } else {
        bool can; { std::lock_guard<std::mutex> g(r->m); can = r->preprocessed && !r->finished; }
        if (can && d.node > 0) { r->hedges++; launch_attempt(r, d.node, true); }
      }
    }
  }
  cv_.notify_all();
}

// Late binding: the client-side work of r is done; hand it to the scheduler and wait for its node.
// Returns the bound node (0 = compile locally after all: every remote window stayed full).
int Coordinator::wait_binding(const std::shared_ptr<JobRec>& r) {
  std::unique_lock<std::mutex> l(mu_);
  if (!r->bound) { sched_->on_preprocessed(r->id, now()); run_decide_locked(); }
  cv_.wait(l, [&] { return r->bound || stopping_.load(); });
  return r->bound ? r->node : 0;
}

// every attempt of r died with its worker `node` (attempt_thread disabled it: transport loss, not a
// compile error). Under late binding the job is staged again with its mode and bound to another node — once — instead
// of compiling on the client, where a lost window of jobs would otherwise land at once while other workers
// have free slots. Returns the new node (0 = the scheduler chose the client), or -1 = not re-dispatched (early
// binding, already re-dispatched once, the node is still enabled, or no remote node is left): the caller falls back.
int Coordinator::redispatch_lost(const std::shared_ptr<JobRec>& r, int node) {
  std::unique_lock<std::mutex> l(mu_);
  if (r->redispatched || node <= 0 || !sched_->node_disabled(node) || stopping_) return -1;
  bool any = false;
  for (auto& n : topo_.nodes) if (n.id > 0 && !sched_->node_disabled(n.id)) { any = true; break; }
  if (!any || !sched_->requeue_lost(r->id, r->mode)) return -1;
  {
    std::lock_guard<std::mutex> g(r->m);
    r->finished = false; r->have_result = false; r->last_error.clear(); r->attempts.clear(); r->attempts_outstanding = 0;
  }
  r->redispatched = true; r->bound = false; redispatches_++;
  log({{"event", "redispatch"}, {"job", r->id}, {"lost_node", node}});
  run_decide_locked();
  cv_.wait(l, [&] { return r->bound || stopping_.load(); });
  return r->bound ? r->node : 0;
}

void Coordinator::complete_locked(const std::shared_ptr<JobRec>& r, int node, double measured_w) {
  Job& j = wl_.jobs[static_cast<size_t>(r->id)];
  if (r->pre_bytes > 0) j.pre_size_bytes = r->pre_bytes;
  if (r->pp_cpu > 0) j.preprocess_cpu_s = r->pp_cpu;
  if (r->obj_bytes > 0) j.obj_size_bytes = static_cast<double>(r->obj_bytes);
  if (r->mode == Mode::HeaderPush) {
    j.scan_cpu_s = r->scan_s; scan_mean_ = 0.9 * scan_mean_ + 0.1 * r->scan_s;
    double cl = 0; for (auto& d : r->deps) cl += static_cast<double>(d.size);
    j.closure_bytes = cl; closure_mean_ = 0.9 * closure_mean_ + 0.1 * cl;
  }
  j.work_ref_cpu_s = measured_w;
  // only real compiles enter the persisted per-TU times (a cache hit's time says nothing about the TU)
  if (hist_ && r->rc == 0 && measured_w > 0 && r->cache_status != "hit" && (r->outcome == "local" || r->outcome == "remote"))
    hist_->record(j.name, measured_w);
  r->w_meas = measured_w; r->t_done = now();
  sched_->on_job_done(r->id, node, now(), measured_w);
  // feature estimators (EMA)
  if (r->pre_bytes > 0 && j.src_size_bytes > 0) {
    double& ratio = r->lang == "c++" ? pre_per_src_cxx_ : pre_per_src_c_;
    ratio = 0.9 * ratio + 0.1 * (r->pre_bytes / j.src_size_bytes);
    if (r->pp_cpu > 0) pp_mean_ = 0.9 * pp_mean_ + 0.1 * r->pp_cpu;
    if (r->obj_bytes > 0) obj_per_pre_ = 0.9 * obj_per_pre_ + 0.1 * (static_cast<double>(r->obj_bytes) / r->pre_bytes);
  }
  inflight_--; completed_++; last_activity_ = now();
  if (completed_ % 25 == 0) sched_->refresh(now());
  run_decide_locked();
}

void Coordinator::launch_attempt(const std::shared_ptr<JobRec>& r, int node, bool hedge) {
  int64_t rid = next_rid_++;
  { std::lock_guard<std::mutex> g(r->m); r->attempts_outstanding++; r->attempts_total++; r->attempts.emplace_back(node, rid); }
  remote_launches_++;
  std::thread(&Coordinator::attempt_thread, this, r, node, rid, hedge).detach();
}

// ---- header-push helpers --------------------------------------------------------
// Persistent key-hash state: <client cache>/state/khash7.log (logs of older key-hash formats are ignored),
// append-only records
//   u32 path_len | path | i64 mtime_ns | u64 size | u64 inode | i64 ctime_ns | u8 sha_len | sha
// Loaded at start (later records win); every entry is still validated by an exact (mtime_ns, size, inode, ctime_ns)
// stat match before use, so the log can never make content stale (ctime is included because `touch -r`,
// rsync -t and cp -p restore mtime, never ctime). "Racy" files — changed (ctime) less than kRacyS before they were hashed (coarse kernel
// timestamps; git's racy-clean rule) — are never cached, in memory or on disk.
static bool racy_ctime(int64_t ct) { return opticc::direct::stamp_now() - static_cast<double>(ct) / 1e9 < opticc::direct::kRacyS; }

void Coordinator::persist_khash(const std::string& path, int64_t mt, uint64_t sz, uint64_t ino, int64_t ct, const std::string& sha) {
  if (state_dir_.empty() || racy_ctime(ct)) return;
  auto put = [](std::string& b, const void* p, size_t n) { b.append(static_cast<const char*>(p), n); };
  std::lock_guard<std::mutex> g(statemu_);
  uint32_t pl = static_cast<uint32_t>(path.size()); uint8_t sl = static_cast<uint8_t>(std::min<size_t>(sha.size(), 255));
  put(state_buf_, &pl, 4); state_buf_ += path; put(state_buf_, &mt, 8); put(state_buf_, &sz, 8); put(state_buf_, &ino, 8); put(state_buf_, &ct, 8);
  put(state_buf_, &sl, 1); state_buf_.append(sha, 0, sl);
  if (state_buf_.size() > (1u << 20)) {
    std::FILE* f = std::fopen((state_dir_ + "/khash7.log").c_str(), "ab");
    if (f) { std::fwrite(state_buf_.data(), 1, state_buf_.size(), f); std::fclose(f); }
    state_buf_.clear();
  }
}

void Coordinator::flush_hash_state() {
  if (state_dir_.empty()) return;
  try {   // the scanner's parse memo, rewritten atomically (racy files excluded by the scanner)
    std::string blob = scanner_.export_state();
    if (!blob.empty()) { util::write_file(state_dir_ + "/scan.bin.tmp", blob); std::rename((state_dir_ + "/scan.bin.tmp").c_str(), (state_dir_ + "/scan.bin").c_str()); }
  } catch (...) {}
  std::lock_guard<std::mutex> g(statemu_);
  if (state_buf_.empty()) return;
  std::FILE* f = std::fopen((state_dir_ + "/khash7.log").c_str(), "ab");
  if (f) { std::fwrite(state_buf_.data(), 1, state_buf_.size(), f); std::fclose(f); }
  state_buf_.clear();
}

void Coordinator::load_hash_state() {
  std::string data;
  try { data = util::read_file(state_dir_ + "/khash7.log"); } catch (...) { return; }
  size_t i = 0, n = 0;
  auto get = [&](void* p, size_t k) { if (i + k > data.size()) return false; std::memcpy(p, data.data() + i, k); i += k; return true; };
  std::lock_guard<std::shared_mutex> g(khmu_);
  while (i < data.size()) {
    uint32_t pl; if (!get(&pl, 4) || pl > 65536 || i + pl > data.size()) break;
    std::string path = data.substr(i, pl); i += pl;
    int64_t mt, ct; uint64_t sz, ino; uint8_t sl;
    if (!get(&mt, 8) || !get(&sz, 8) || !get(&ino, 8) || !get(&ct, 8) || !get(&sl, 1) || i + sl > data.size()) break;
    std::string sha = data.substr(i, sl); i += sl;
    khash_cache_[path] = KHashEnt{mt, sz, ino, ct, sha}; ++n;
  }
  if (util::getenv_or("OPTICC_DEBUG_CACHE", "") == "1") std::fprintf(stderr, "client-cache: loaded %zu persisted key hashes\n", n);
  if (data.size() > (64u << 20)) {   // compact: rewrite the current map
    std::string out;
    auto put = [&](const void* p, size_t k) { out.append(static_cast<const char*>(p), k); };
    for (auto& [path, e] : khash_cache_) { uint32_t pl = static_cast<uint32_t>(path.size()); uint8_t sl = static_cast<uint8_t>(e.sha.size()); put(&pl, 4); out += path; put(&e.mtime_ns, 8); put(&e.size, 8); put(&e.ino, 8); put(&e.ctime_ns, 8); put(&sl, 1); out += e.sha; }
    try { util::write_file(state_dir_ + "/khash7.log.tmp", out); std::rename((state_dir_ + "/khash7.log.tmp").c_str(), (state_dir_ + "/khash7.log").c_str()); } catch (...) {}
  }
}

std::vector<std::string> Coordinator::key_hash_batch(const std::vector<std::string>& abs, bool raw_only) {
  std::vector<std::string> out(abs.size());
  if (raw_only) { for (size_t i = 0; i < abs.size(); ++i) { try { out[i] = hash_file(abs[i], nullptr); } catch (...) {} } return out; }
  struct St { int64_t mt; uint64_t sz, ino; int64_t ct; bool ok; };
  std::vector<St> st(abs.size());
  for (size_t i = 0; i < abs.size(); ++i) {
    direct::FStat f = direct::stat_path(abs[i]);
    if (!f.exists || f.is_dir) { st[i].ok = false; continue; }
    st[i] = St{f.mtime_ns, f.size, f.ino, f.ctime_ns, true};
  }
  std::vector<size_t> miss;
  {
    std::shared_lock<std::shared_mutex> g(khmu_);   // concurrent hits read the key-hash cache together
    for (size_t i = 0; i < abs.size(); ++i) {
      if (!st[i].ok) continue;
      auto it = khash_cache_.find(abs[i]);
      if (it != khash_cache_.end() && it->second.mtime_ns == st[i].mt && it->second.size == st[i].sz && it->second.ino == st[i].ino && it->second.ctime_ns == st[i].ct) out[i] = it->second.sha;
      else miss.push_back(i);
    }
  }
  for (size_t i : miss) {
    try {
      std::string sha = cachekey::content_key_sha(util::read_file(abs[i]));
      if (!racy_ctime(st[i].ct)) {
        { std::lock_guard<std::shared_mutex> g(khmu_); khash_cache_[abs[i]] = KHashEnt{st[i].mt, st[i].sz, st[i].ino, st[i].ct, sha}; }
        persist_khash(abs[i], st[i].mt, st[i].sz, st[i].ino, st[i].ct, sha);
      }
      out[i] = std::move(sha);
    } catch (...) {}
  }
  return out;
}

static uint64_t fnv64(const std::string& s) { uint64_t h = 1469598103934665603ULL; for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; } return h; }

const PathEnt* Coordinator::path_ent_locked(const std::string& cwd, const std::string& p, const std::vector<std::string>& sysd) {
  std::string k; k.reserve(cwd.size() + p.size() + 1); k += cwd; k.push_back('\0'); k += p;
  auto it = pmemo_.find(k);
  if (it != pmemo_.end()) return it->second.get();
  auto e = std::make_unique<PathEnt>();
  e->abs = closure::absolutize(p, cwd);
  for (auto& d : sysd) if (util::path_within(e->abs, d)) { e->sys = true; break; }
  e->rel = cachekey::relativise(e->abs, cwd);
  e->h = fnv64(e->rel);
  const PathEnt* out = e.get(); pmemo_.emplace(std::move(k), std::move(e)); return out;
}

const PathEnt* Coordinator::path_ent(const std::string& cwd, const std::string& p, const std::vector<std::string>& sysd) {
  std::string k; k.reserve(cwd.size() + p.size() + 1); k += cwd; k.push_back('\0'); k += p;
  { std::shared_lock<std::shared_mutex> g(pmu_); auto it = pmemo_.find(k); if (it != pmemo_.end()) return it->second.get(); }
  std::unique_lock<std::shared_mutex> g(pmu_); return path_ent_locked(cwd, p, sysd);
}

std::vector<const PathEnt*> Coordinator::path_ents(const std::string& cwd, const std::vector<std::string>& ps, const std::vector<std::string>& sysd) {
  std::vector<const PathEnt*> out(ps.size(), nullptr);
  std::vector<std::string> keys(ps.size());
  for (size_t i = 0; i < ps.size(); ++i) { keys[i].reserve(cwd.size() + ps[i].size() + 1); keys[i] += cwd; keys[i].push_back('\0'); keys[i] += ps[i]; }
  bool missing = false;
  { std::shared_lock<std::shared_mutex> g(pmu_);
    for (size_t i = 0; i < ps.size(); ++i) { auto it = pmemo_.find(keys[i]); if (it != pmemo_.end()) out[i] = it->second.get(); else missing = true; } }
  if (missing) { std::unique_lock<std::shared_mutex> g(pmu_); for (size_t i = 0; i < ps.size(); ++i) if (!out[i]) out[i] = path_ent_locked(cwd, ps[i], sysd); }
  return out;
}

void Coordinator::scan_only(JobRec& r) {
  auto sysd = sys_dirs(r.cc_local, r.lang, r.cwd);
  bool computed = false; int sf = 0; bool hic = false;
  std::string src_abs = closure::absolutize(r.src, r.cwd);
  auto sp = scanner_.closure(src_abs, args::include_dirs(r.argv), sysd, r.cwd, &computed, &sf, &hic);
  r.has_include_computed = hic;
  r.scan_abs.clear(); r.scan_ents.clear(); r.scan_abs.reserve(sp.size()); r.scan_ents.reserve(sp.size());
  auto ents = path_ents(r.cwd, sp, sysd);   // batched; PathEnt pointers are stable (never erased)
  for (const PathEnt* e : ents) {
    if (e->sys || e->abs.empty() || e->abs == r.src) continue;
    r.scan_ents.push_back(e); r.scan_abs.push_back(e->abs);
  }
}

// order-independent shape of the scanner closure + source: two 64-bit sums of per-path hashes and the count.
// O(n) without sorting or concatenating ~600 strings per lookup.
std::string Coordinator::shape_fast(const JobRec& r, const std::string& src_rel) {
  uint64_t a = fnv64(src_rel), b = a * 0x9E3779B97F4A7C15ULL; size_t n = 1;
  for (auto* e : r.scan_ents) { a += e->h; b += (e->h ^ (e->h >> 29)) * 0xBF58476D1CE4E5B9ULL; ++n; }
  char buf[64]; std::snprintf(buf, sizeof buf, "v33:%016llx%016llx:%zu", (unsigned long long)a, (unsigned long long)b, n);
  return buf;
}

bool Coordinator::time_macros(const std::string& abs, const std::string& sha) {
  { std::lock_guard<std::mutex> g(tmmu_); auto it = tmacro_.find(sha); if (it != tmacro_.end()) return it->second; }
  bool t = true;
  try {
    const std::string txt = util::read_file(abs);
    // the bytes read must be the bytes `sha` names (else the file changed: answer yes and memoise nothing)
    if (cachekey::content_key_sha(txt) != sha && util::sha256_hex(txt) != sha) return true;
    t = cachekey::uses_time_macros(txt);
    // the assembler-file-directive memo is filled from the same read (asm_directive)
    { const bool fd = cachekey::file_directive(txt, false); std::lock_guard<std::mutex> g(admu_); if (adir_.size() > (1u << 20)) adir_.clear(); adir_[sha] = fd; }
  } catch (...) { return true; }
  std::lock_guard<std::mutex> g(tmmu_); if (tmacro_.size() > (1u << 20)) tmacro_.clear(); tmacro_[sha] = t; return t;
}

bool Coordinator::asm_directive(const std::string& abs, const std::string& sha) {
  { std::lock_guard<std::mutex> g(admu_); auto it = adir_.find(sha); if (it != adir_.end()) return it->second; }
  // store_client_cache calls time_macros() on the same inputs first; it reads each content once and fills this memo too
  bool t = true;
  try {
    const std::string txt = util::read_file(abs);
    if (cachekey::content_key_sha(txt) != sha && util::sha256_hex(txt) != sha) return true;   // changed: refuse, memoise nothing
    t = cachekey::file_directive(txt, false);
  } catch (...) { return true; }
  std::lock_guard<std::mutex> g(admu_); if (adir_.size() > (1u << 20)) adir_.clear(); adir_[sha] = t; return t;
}

std::string Coordinator::key_hash_file(const std::string& path, bool raw_only) {
  if (raw_only) return hash_file(path, nullptr);
  direct::FStat f = direct::stat_path(path);
  if (!f.exists) throw std::runtime_error("stat " + path);
  {
    std::lock_guard<std::shared_mutex> g(khmu_);
    auto it = khash_cache_.find(path);
    if (it != khash_cache_.end() && it->second.mtime_ns == f.mtime_ns && it->second.size == f.size && it->second.ino == f.ino && it->second.ctime_ns == f.ctime_ns) return it->second.sha;
  }
  std::string sha = cachekey::content_key_sha(util::read_file(path));
  if (!racy_ctime(f.ctime_ns)) {
    { std::lock_guard<std::shared_mutex> g(khmu_); khash_cache_[path] = KHashEnt{f.mtime_ns, f.size, f.ino, f.ctime_ns, sha}; }
    persist_khash(path, f.mtime_ns, f.size, f.ino, f.ctime_ns, sha);
  }
  return sha;
}

std::string Coordinator::hash_file(const std::string& path, uint64_t* size) {
  // Every lookup re-stat()s: the shipped hash decides which bytes the worker compiles, so it must never be stale
  // (a time-based trust window would ship an old header after a quick edit-rebuild). Cache key (path, mtime_ns,
  // size, inode); a stat is ~1 µs, so a 670-file kernel closure costs well under 1 ms. Staleness is tolerated only in
  // the include *scan* (incscan.cpp), where it can at worst cost a MISSING round, never wrong content.
  // plus ctime; racy files (changed < kRacyS ago) are not cached (coarse kernel timestamps).
  direct::FStat f = direct::stat_path(path);
  if (!f.exists) throw std::runtime_error("stat " + path);
  {
    std::lock_guard<std::mutex> g(hmu_);
    auto it = hash_cache_.find(path);
    if (it != hash_cache_.end() && it->second.mtime_ns == f.mtime_ns && it->second.size == f.size && it->second.ino == f.ino && it->second.ctime_ns == f.ctime_ns) { if (size) *size = f.size; return it->second.sha; }
  }
  std::string sha = util::sha256_hex(util::read_file(path));
  if (!racy_ctime(f.ctime_ns)) { std::lock_guard<std::mutex> g(hmu_); hash_cache_[path] = HashEnt{f.mtime_ns, f.size, f.ino, f.ctime_ns, sha, 0}; }
  if (size) *size = f.size;
  return sha;
}

std::vector<std::string> Coordinator::sys_dirs(const std::string& cc, const std::string& lang, const std::string& cwd) {
  std::string key = cc + "\x01" + lang;
  { std::lock_guard<std::mutex> g(hmu_); auto it = sysdirs_cache_.find(key); if (it != sysdirs_cache_.end()) return it->second; }
  std::vector<std::string> d = closure::system_include_dirs(cc, lang, cwd);
  // NO fallback directories. A file under a "system" directory is kept out of every key, hash and ship
  // list; that is sound only for directories the system-include fingerprint (sysinc::dirs_for: the compiler's own
  // search list) covers. Fixed fallbacks (/usr/lib, /usr/include, /Library/Developer, …) would also exclude headers
  // reached through user flags into those trees (pkg-config / llvm-config -I paths such as /usr/lib/llvm-17/include)
  // that no fingerprint covers, so a package upgrade could give a stale hit. An unparsable search list means "no system directory":
  // everything is an ordinary input (hashed, shipped) and no store happens (the sysinc signature is unavailable).
  std::lock_guard<std::mutex> g(hmu_); sysdirs_cache_[key] = d; return d;
}

bool Coordinator::shippable(const std::string& abs, const std::vector<std::string>& sysd, const JobRec& r) const {
  if (abs.empty() || abs == r.src) return false;
  for (auto& d : sysd) if (util::path_within(abs, d)) return false;
  return util::file_exists(abs);
}

void Coordinator::build_closure(JobRec& r) {
  double t0 = util::now_s();
  closure_builds_++;   // counted (summary v4.closure_builds)
  r.deps.clear(); r.scan_abs.clear();   // idempotent (a second call must not append the closure twice)
  std::vector<std::string> cands;
  if (!r.scan.dep_file.empty()) cands.push_back(closure::absolutize(r.scan.dep_file, r.cwd));
  cands.push_back(closure::absolutize(args::default_dep_name(r.out), r.cwd));
  cands.push_back(closure::absolutize(r.out + ".d", r.cwd));   // ninja style
  std::string kcmd = closure::kbuild_cmd_path(r.out);           // kbuild: dir/.foo.o.cmd (fixdep output)
  std::vector<std::string> paths;
  for (auto& c : cands) if (util::file_exists(c)) { try { paths = closure::parse_depfile(util::read_file(c)); } catch (...) {} break; }
  if (paths.empty() && !kcmd.empty()) {
    std::string kc = closure::absolutize(kcmd, r.cwd);
    if (util::file_exists(kc)) { try { paths = closure::parse_kbuild_cmd(util::read_file(kc)); if (!paths.empty()) r.closure_source = "kbuild-cmd"; } catch (...) {} }
  }
  if (!paths.empty()) r.closure_source = r.closure_source.empty() ? "depfile" : r.closure_source;
  // distributed ThinLTO backend (lang "ir"): the closure is the per-module index plus the bitcode modules named in its
  // <module>.imports sibling (lld --thinlto-emit-imports-files); no include scan, no system directories.
  if (r.lang == "ir") {
    paths.clear();
    if (!r.scan.thinlto_index.empty()) {
      std::string idx = closure::absolutize(r.scan.thinlto_index, r.cwd); paths.push_back(idx);
      std::string imp = idx; const std::string sfx = ".thinlto.bc";
      if (imp.size() > sfx.size() && imp.compare(imp.size() - sfx.size(), sfx.size(), sfx) == 0) imp = imp.substr(0, imp.size() - sfx.size()) + ".imports";
      else imp += ".imports";
      if (util::file_exists(imp)) {
        std::string txt = util::read_file(imp); size_t a = 0;
        while (a < txt.size()) { size_t e = txt.find('\n', a); if (e == std::string::npos) e = txt.size(); std::string ln = txt.substr(a, e - a); if (!ln.empty()) paths.push_back(ln); a = e + 1; }
      }
    }
    r.closure_source = "thinlto-imports";
  }
  auto sysd = r.lang == "ir" ? std::vector<std::string>{} : sys_dirs(r.cc_local, r.lang, r.cwd);
  bool computed = false;
  if (paths.empty() && r.lang != "ir") {
    // no dependency file (clean build): scan the includes ourselves, -MM only for
    // translation units with a computed include
    std::string src_abs = closure::absolutize(r.src, r.cwd);
    auto dirs = args::include_dirs(r.argv);
    paths = scanner_.closure(src_abs, dirs, sysd, r.cwd, &computed, &r.scan_files, &r.has_include_computed);
    r.scan_abs = paths;
    r.closure_computed = computed;   // this closure lacks the computed-include targets
    r.closure_source = computed ? (std::getenv("OPTICC_SCAN_MM") && std::string(std::getenv("OPTICC_SCAN_MM")) == "1" ? "scan+mm" : "scan+computed") : "scan";
  } else if (r.lang != "ir") {
    // a dependency file lists only the headers that were INCLUDED; a header merely tested with __has_include is
    // absent from it, and on the worker the test would silently flip. The (memoised, ~ms) scan adds literal
    // __has_include targets and detects __has_include(MACRO); the union is a superset of what the TU can read.
    bool c2 = false, hic = false; int sf = 0;
    std::string src_abs = closure::absolutize(r.src, r.cwd);
    auto sp = scanner_.closure(src_abs, args::include_dirs(r.argv), sysd, r.cwd, &c2, &sf, &hic);
    r.has_include_computed = hic;
    r.scan_abs = sp;
    std::unordered_set<std::string> have(paths.begin(), paths.end());
    for (auto& x : sp) if (have.insert(x).second) paths.push_back(x);
  }
  // Hash the closure: stat() every file (no lock; exactness — see hash_file), then ONE hash-cache lock to match all
  // (mtime, size, inode) triples, then read+hash only the misses. Per-file locking convoyed at 8 concurrent TUs.
  struct Ent { std::string abs; int64_t mt; uint64_t sz, ino; int64_t ct; };
  std::vector<Ent> ents; ents.reserve(paths.size());
  for (auto& p : paths) {
    std::string abs = closure::absolutize(p, r.cwd);
    if (abs.empty() || abs == r.src) continue;
    bool sys = false; for (auto& d : sysd) if (util::path_within(abs, d)) { sys = true; break; }
    if (sys) continue;
    direct::FStat f = direct::stat_path(abs);
    if (!f.exists) continue;   // a vanished file is simply skipped
    ents.push_back(Ent{std::move(abs), f.mtime_ns, f.size, f.ino, f.ctime_ns});
  }
  std::vector<size_t> miss;
  {
    std::lock_guard<std::mutex> g(hmu_);
    for (size_t i = 0; i < ents.size(); ++i) {
      auto it = hash_cache_.find(ents[i].abs);
      if (it != hash_cache_.end() && it->second.mtime_ns == ents[i].mt && it->second.size == ents[i].sz && it->second.ino == ents[i].ino && it->second.ctime_ns == ents[i].ct)
        r.deps.push_back(proto::FileRef{ents[i].abs, it->second.sha, ents[i].sz});
      else miss.push_back(i);
    }
  }
  for (size_t i : miss) {
    uint64_t sz = 0; std::string sha;
    try { sha = hash_file(ents[i].abs, &sz); } catch (...) { continue; }
    r.deps.push_back(proto::FileRef{ents[i].abs, sha, sz});
  }
  {   // normalise the scanner closure exactly like the hashed one (absolute, non-system, without the source)
    std::vector<std::string> keep; keep.reserve(r.scan_abs.size());
    for (auto& p : r.scan_abs) {
      std::string abs = closure::absolutize(p, r.cwd);
      if (abs.empty() || abs == r.src) continue;
      bool sys = false; for (auto& d : sysd) if (util::path_within(abs, d)) { sys = true; break; }
      if (!sys) keep.push_back(std::move(abs));
    }
    r.scan_abs.swap(keep);
  }
  r.closure_known = true;
  r.scan_s += util::now_s() - t0;
  if (o_.prefetch) for (auto& d : r.deps) pf_.note(d.sha, d.path, d.size);   // prefetch popularity ranking
  // A full -MM run for a computed include (`#include MACRO`) costs ~0.3–0.5 s of client CPU. The worker's MISSING
  // round already covers anything the scan missed at the cost of one round trip, so the -MM fallback is opt-in
  // (OPTICC_SCAN_MM=1).
  static const bool mm_fallback = std::getenv("OPTICC_SCAN_MM") && std::string(std::getenv("OPTICC_SCAN_MM")) == "1";
  if (computed && mm_fallback) closure_via_mm(r);   // its own timing is added to scan_s
}

void Coordinator::closure_via_mm(JobRec& r) {
  double t0 = util::now_s();
  r.closure_from_mm = true;
  if (r.closure_source.empty()) r.closure_source = "mm";
  try {
    std::string dir = util::make_temp_dir("opticc-mm-");
    auto argv = args::mm_argv(r.argv, r.scan); argv[0] = r.cc_local;
    util::run_process(argv, dir + "/out", dir + "/err", r.cwd);
    std::string out = util::file_exists(dir + "/out") ? util::read_file(dir + "/out") : "";
    util::remove_tree(dir);
    auto sysd = sys_dirs(r.cc_local, r.lang, r.cwd);
    for (auto& p : closure::parse_depfile(out)) {
      std::string abs = closure::absolutize(p, r.cwd);
      if (!shippable(abs, sysd, r)) continue;
      bool have = false; for (auto& d : r.deps) if (d.path == abs) { have = true; break; }
      if (have) continue;
      uint64_t sz = 0; std::string sha;
      try { sha = hash_file(abs, &sz); } catch (...) { continue; }
      r.deps.push_back(proto::FileRef{abs, sha, sz});
    }
    r.closure_known = true;
  } catch (const std::exception& e) { log({{"event", "mm-failed"}, {"job", r.id}, {"error", e.what()}}); }
  r.scan_s += util::now_s() - t0;
}

// One header-push attempt: COMPILE_HP, then answer NEED / MISSING until RESULT.
void Coordinator::hp_attempt(const std::shared_ptr<JobRec>& r, int node, int64_t rid, bool hedge, proto::CompileResult* res, std::string* error) {
  auto& conn = conns_.at(static_cast<size_t>(node - 1));
  proto::HpRequest req;
  req.id = rid; req.argv = r->argv; req.lang = r->lang; req.cwd = r->cwd; req.src_path = r->src;
  req.source = util::read_file(r->src);
  { std::lock_guard<std::mutex> g(r->m); req.deps = r->deps; }
  uint64_t sent = 0, total = 0;
  // piggyback: push the closure files this node is not known to hold right before the job on the same FIFO
  // connection; the worker stores PREFETCH frames inline, so the job finds them and never sends NEED. Only for nodes
  // whose held-set is exact (empty store at HELLO); otherwise NEED stays the exact mechanism.
  if (o_.prefetch && static_cast<size_t>(node) < pf_exact_.size() && pf_exact_[static_cast<size_t>(node)]) {
    std::vector<proto::FileRef> miss; int have = 0;
    for (auto& d : req.deps) { if (pf_.held(node, d.sha)) have++; else miss.push_back(d); }
    uint64_t raw = 0;
    if (!miss.empty()) { try { prefetch_send(node, miss, false, &raw); } catch (const std::exception& e) { *error = e.what(); return; } }
    std::lock_guard<std::mutex> g(r->m);
    if (!hedge) { r->piggy_bytes = raw; r->deps_prefetched = have; }
  }
  auto session = conn->compile_hp(req, o_.zstd_level, &sent); total += sent;
  observe_upload(node);
  std::vector<proto::FileRef> sent_files = req.deps;
  auto already = [&](const std::string& path) { for (auto& f : sent_files) if (f.path == path) return true; return false; };
  proto::Frame ev;
  // the whole attempt (NEED/MISSING rounds included) has a deadline; past it the request is cancelled
  // and abandoned and the job takes the error path (another copy or the local fallback)
  const double deadline = util::now_s() + net::WorkerConn::attempt_timeout_s();
  bool timed_out = false;
  while (session->wait_event_for(&ev, deadline - util::now_s(), &timed_out)) {
    proto::BlobsMsg b; b.id = rid;
    if (ev.type == proto::Type::Need) {
      proto::NeedMsg need = proto::decode_need(ev);
      for (auto& sha : need.shas) {
        std::string path; { std::lock_guard<std::mutex> g(r->m); for (auto& d : r->deps) if (d.sha == sha) { path = d.path; break; } }
        if (path.empty()) continue;
        try { b.blobs.emplace_back(sha, util::read_file(path)); } catch (...) {}
      }
    } else if (ev.type == proto::Type::Missing) {
      proto::MissingMsg m = proto::decode_missing(ev);
      double t0 = util::now_s();
      auto dirs = args::include_dirs(r->argv);
      auto sysd = sys_dirs(r->cc_local, r->lang, r->cwd);
      // the worker's names are untrusted. The includer only widens the search when it is already one
      // of this job's files, and whatever resolves is shipped only when it lies inside the job's scope
      // (opticc/servescope.hpp): never the client's keys, ~/.ssh, its caches, /proc, ...
      const std::string includer_ok = (!m.includer.empty() && (m.includer == r->src || already(m.includer))) ? m.includer : std::string();
      const auto scope = servescope::build(r->argv, r->cwd, r->src, includer_ok,
                                           {util::getenv_or("HOME", "/nonexistent") + "/.cache/opticc"});
      std::string found = closure::resolve_include(m.name, includer_ok, dirs, sysd, r->cwd);
      std::string scope_why;
      if (!found.empty() && !servescope::allowed(scope, found, &scope_why)) {
        log({{"event", "missing-refused"}, {"job", r->id}, {"name", m.name}, {"resolved", found}, {"why", scope_why}});
        { std::lock_guard<std::mutex> g(mu_); missing_refused_++; }
        found.clear();
      }
      std::vector<proto::FileRef> add;
      if (!found.empty() && shippable(found, sysd, *r) && !already(found)) {
        uint64_t sz = 0; std::string sha = hash_file(found, &sz);
        add.push_back(proto::FileRef{found, sha, sz});
      }
      // first miss without a dependency file: learn the whole closure once (q_j)
      bool need_mm; { std::lock_guard<std::mutex> g(r->m); need_mm = !r->closure_from_mm; }
      if (need_mm) {
        std::lock_guard<std::mutex> g(r->m);
        closure_via_mm(*r);
        for (auto& d : r->deps) if (!already(d.path)) { bool dup = false; for (auto& a : add) if (a.path == d.path) dup = true; if (!dup) add.push_back(d); }
      }
      { std::lock_guard<std::mutex> g(r->m); r->scan_s += util::now_s() - t0; r->rounds = std::max(r->rounds, m.round); }
      for (auto& fr : add) {
        try { b.blobs.emplace_back(fr.sha, util::read_file(fr.path)); b.files.push_back(fr); sent_files.push_back(fr); } catch (...) {}
      }
      { std::lock_guard<std::mutex> g(r->m); for (auto& fr : add) { bool have = false; for (auto& d : r->deps) if (d.path == fr.path) have = true; if (!have) r->deps.push_back(fr); } }
      log({{"event", "missing"}, {"job", r->id}, {"name", m.name}, {"includer", m.includer}, {"resolved", found}, {"sent", add.size()}, {"round", m.round}});
    } else continue;
    try { conn->send_blobs(b, o_.zstd_level, &sent); total += sent; } catch (const std::exception& e) { *error = e.what(); break; }
    for (auto& [sha, data] : b.blobs) pf_.mark(node, sha, data.size());
  }
  if (timed_out && error->empty()) { abandon_attempt(node, rid); *error = "attempt deadline passed (OPTICC_ATTEMPT_TIMEOUT_S)"; }
  if (error->empty()) { if (!session->error.empty()) *error = session->error; else *res = session->result; }
  {
    std::lock_guard<std::mutex> g(r->m);
    if (!hedge) { r->shipped_bytes = total; r->raw_bytes = req.source.size(); r->zstd_bytes = total; r->miss_bytes = res->miss_bytes; }
  }
  std::lock_guard<std::mutex> g(mu_);
  sched_->on_shipped(r->id, node, Mode::HeaderPush, static_cast<double>(total));
}

// an attempt whose deadline passed: the worker is told to kill the compile, the connection stops
// tracking the request (a late RESULT is dropped), and the timeout is counted (summary "attempt_timeouts")
void Coordinator::abandon_attempt(int node, int64_t rid) {
  auto& conn = conns_.at(static_cast<size_t>(node - 1));
  conn->cancel(rid); conn->abandon(rid);
  log({{"event", "attempt-timeout"}, {"node", node}, {"rid", rid}});
  std::lock_guard<std::mutex> g(mu_); attempt_timeouts_++;
}

// true when the wrapper has closed its end of the connection (it gave up after
// OPTICC_WRAPPER_TIMEOUT_S and compiled locally): the output must then not be written a second time
static bool client_gone(const net::Socket& s) {
  pollfd p{s.fd(), POLLIN, 0};
  if (::poll(&p, 1, 0) <= 0) return false;
  if (p.revents & (POLLHUP | POLLERR)) return true;
  char c; return ::recv(s.fd(), &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

void Coordinator::attempt_thread(std::shared_ptr<JobRec> r, int node, int64_t rid, bool hedge) {
  proto::CompileResult res; std::string error;
  try {
    if (r->mode == Mode::HeaderPush) {
      hp_attempt(r, node, rid, hedge, &res, &error);
      if (!error.empty()) throw std::runtime_error(error);
    } else {
      proto::CompileRequest req;
      req.id = rid; req.argv = r->argv; req.lang = r->lang; req.input_name = r->input_name;
      req.preprocessed = util::read_file(r->ifile);
      uint64_t z = 0;
      auto fut = conns_.at(static_cast<size_t>(node - 1))->compile(req, classic_zstd_level(node), &z);
      observe_upload(node);
      { std::lock_guard<std::mutex> g(r->m); if (!hedge) { r->raw_bytes = req.preprocessed.size(); r->zstd_bytes = z; } }
      if (fut.wait_for(std::chrono::duration<double>(net::WorkerConn::attempt_timeout_s())) != std::future_status::ready) {
        abandon_attempt(node, rid); throw std::runtime_error("attempt deadline passed (OPTICC_ATTEMPT_TIMEOUT_S)");
      }
      res = fut.get();
    }
  } catch (const std::exception& e) {
    error = e.what();
    std::lock_guard<std::mutex> g(mu_);
    if (!sched_->node_disabled(node) && !conns_.at(static_cast<size_t>(node - 1))->connected()) {
      sched_->disable_node(node); cache_idx_.forget_node(node); pf_.forget_node(node);
      unreachable_.insert(node);   // connection loss (not a verification failure) -> retried, re-admitted when back
      fputs(("opticc: worker " + topo_.node(node).name + " lost: " + error + " (disabled, retrying)\n").c_str(), stderr);
    }
  }
  std::string status = error.empty() ? res.status : "error";
  double cpu = res.cpu_s;
  if (error.empty() && res.compile_bench_s > 0) { std::lock_guard<std::mutex> g(mu_); refresh_prior_locked(node, res.compile_bench_s); }
  if (error.empty() && !res.cache_key.empty()) cache_idx_.record_result(res.cache_key, node, res.cache);
  if (error.empty() && r->affinity) {
    std::lock_guard<std::mutex> g(mu_);
    if (res.cache == "hit") aff_real_hits_++; else aff_misses_++;
    // only a Bloom-hinted job that ran on a holder tests the hint (one placed elsewhere misses for that reason)
    if (r->affinity == 2 && std::find(r->aff_nodes.begin(), r->aff_nodes.end(), node) != r->aff_nodes.end()) {
      bloom_holder_outcomes_++; if (res.cache == "hit") bloom_holder_hits_++;
      trust_.outcome(node, res.cache == "hit");   // per-worker evidence
      if (res.cache != "hit") cache_idx_.invalidate(r->full_key);
    }
  }
  if (error.empty()) { std::lock_guard<std::mutex> g(r->m); if (!r->finished) r->cache_status = res.cache; }
  std::lock_guard<std::mutex> g(r->m);
  r->attempts_outstanding--;
  // safety net: ANY remote compile error -- classic (.i artefacts) or header-push (e.g. a header the closure missed
  // through a computed include that no MISSING round recovered) -- is re-run locally before it is reported. A genuine error simply reproduces locally (cost only on failing
  // TUs); a remote-environment artefact never fails the build. Counted: summary "remote_error_local_retries".
  bool usable = error.empty() && res.status == "ok";
  if (error.empty() && res.status == "compile-error") r->remote_compile_error = true;
  if (r->verify) {
    // verify mode: keep every result, never cancel, finish when all copies are in
    if (usable) r->vresults.push_back({node, res.status, res.rc, res.status == "ok" ? util::sha256_hex(res.object) : "", res.err});
    bool primary = !r->attempts.empty() && r->attempts.front().first == node;
    if (usable && (primary || !r->have_result)) { r->result = res; r->result_node = node; r->have_result = true; }
    else if (usable) { r->verify_alt = res; r->verify_alt_node = node; r->verify_alt_ok = true; }
    if (!usable) r->last_error = error.empty() ? (res.status + ": " + res.err) : error;
    if (r->attempts_outstanding == 0) r->finished = true;
  } else if (!r->finished && usable) {
    r->result = std::move(res); r->result_node = node; r->have_result = true; r->finished = true;
    // cancel the other copies
    for (auto& [n2, rid2] : r->attempts) if (rid2 != rid) conns_.at(static_cast<size_t>(n2 - 1))->cancel(rid2);
  } else if (!r->finished) {
    r->last_error = error.empty() ? (res.status + ": " + res.err) : error;
    if (r->attempts_outstanding == 0) r->finished = true;   // all copies failed -> fallback
  }
  r->cv.notify_all();
  log({{"event", hedge ? "hedge-done" : "attempt-done"}, {"job", r->id}, {"node", node}, {"status", status}, {"cpu_s", cpu}});
}

void Coordinator::serve_client(net::Socket s) {
  json req;
  try { if (!recv(s, &req)) return; } catch (...) { return; }
  if (req.value("op", "") == "link") { serve_link(s, req); return; }
  if (req.value("op", "") != "request") return;
  auto r = std::make_shared<JobRec>();
  r->t_start_wall = direct::stamp_now();   // the file-stamping clock (direct.hpp stamp_now)
  {   // the wrapper's direct key (64 hex digits; anything else is ignored)
    std::string dk = req.value("dkey", "");
    if (dk.size() == 64 && dk.find_first_not_of("0123456789abcdef") == std::string::npos) r->dkey = dk;
    r->env = req.value("env", std::vector<std::string>{});
    if (!r->dkey.empty()) { r->dsrc = req.value("dsrc", ""); r->denv = req.value("denv", r->env); }
  }
  r->src = req.value("src", ""); r->out = req.value("out", ""); r->lang = req.value("lang", "c");
  r->cwd = req.value("cwd", ""); r->argv = req.value("argv", std::vector<std::string>{});
  r->cc_local = req.value("cc_local", r->argv.empty() ? std::string() : r->argv[0]);
  if (local_cache_.enabled()) r->sysinc_sig = sysinc_sig(r->cc_local);   // first request per compiler walks (once)
  // the wrapper sent its key WITHOUT the system-include signature; the entry is filed under H(key | signature)
  if (!r->dkey.empty()) r->dkey = r->sysinc_sig.empty() ? std::string() : direct::with_sysinc(r->dkey, r->sysinc_sig);
  r->scan = args::scan(r->argv);
  r->input_name = util::basename(r->src);
  double src_bytes = req.value("src_bytes", 0.0);
  // compile-cache affinity: the manifest key needs only the source bytes, the normalised argv and cwd, so it is
  // known before the decision; the index says which workers already hold the object.
  if (r->scan.source_index >= 0) {
    try {
      std::string src_abs = closure::absolutize(r->src, r->cwd);
      r->cache_key = cacheidx::client_manifest_key(r->argv, r->scan.source_index, r->cwd, util::sha256_hex(util::read_file(src_abs)), r->cc_local);
      // both client keys (affinity above, own here) take the identity of the compiler the wrapper resolved (cc_local), not of
      // argv[0] resolved in the coordinator's PATH (where it may not be found: no identity, no cache)
      r->lkey = cacheidx::client_manifest_key(r->argv, r->scan.source_index, r->cwd, key_hash_file(src_abs, cachekey::keeps_comments(r->argv)), r->cc_local);
      // the cached dependency-file body depends on the dependency flags, which the normalised argv drops: -MD lists
      // system headers, -MMD does not, -MP adds phony targets (otherwise a -Wp,-MMD compile could be
      // answered with the body of an identical -MD compile: right object, wrong .d)
      if (r->scan.has_md) {
        bool mp = false; for (auto& a : r->argv) if (a == "-MP" || (a.rfind("-Wp,", 0) == 0 && a.find(",-MP") != std::string::npos)) mp = true;
        r->lkey = util::sha256_hex(r->lkey + (dep_lists_system_headers(r->argv) ? "|dep:MD" : "|dep:MMD") + (mp ? "+MP" : ""));
      }
    }
    catch (...) { r->cache_key.clear(); r->lkey.clear(); }
  }
  // an environment that changes what the compiler reads or emits (CPATH, C_INCLUDE_PATH, SDKROOT,
  // COMPILER_PATH, SOURCE_DATE_EPOCH, TZ … direct::relevant_env) must be part of every key (otherwise CPATH=dirA then
  // dirB would serve the dirA object). It is folded into the client keys, and the compile runs on the client only (a worker has
  // neither the variables nor the directories they name; the coordinator's scanner and preprocessor run in ITS
  // environment, so the scanned closure and the preprocessed key are refused below and only a dependency file keys it)
  if (!r->env.empty()) {
    std::string e; for (auto& v : r->env) { e += v; e.push_back('\0'); }
    if (!r->lkey.empty()) r->lkey = util::sha256_hex(r->lkey + "|env:" + e);
    if (!r->cache_key.empty()) r->cache_key = util::sha256_hex(r->cache_key + "|env:" + e);
  }
  if (!r->scan.distributable || !r->env.empty()) { serve_local_only(r, s); return; }   // assembler: cache only, never scheduled
  if (try_client_cache(r, s)) return;   // answered from the client-local object cache, no dispatch
  if (try_remote_direct(r, s)) return;  // answered from a shared direct manifest (fresh client), no dispatch
  int node;
  {
    std::unique_lock<std::mutex> l(mu_);
    Job j; j.id = static_cast<int>(wl_.jobs.size()); j.name = r->src;
    j.src_size_bytes = src_bytes;
    j.pre_size_bytes = std::max(1.0, src_bytes * (r->lang == "c++" ? pre_per_src_cxx_ : pre_per_src_c_));
    j.preprocess_cpu_s = pp_mean_;
    j.scan_cpu_s = scan_mean_; j.closure_bytes = closure_mean_;
    j.obj_size_bytes = obj_per_pre_ * j.pre_size_bytes;
    wl_.jobs.push_back(j);
    r->id = j.id; r->t_request = now();
    jobs_.push_back(r);
    sched_->on_jobs_added();
    r->w_hat = pred_->predict(j);
    if (inflight_ == 0) {   // build-epoch boundary: nothing cached may outlive an edit between builds (incscan.cpp)
      const double gap = now() - last_activity_;
      scanner_.new_epoch(gap);
    }
    inflight_++; last_activity_ = now();
    // no affinity from the MANIFEST key (compiler + argv + source): it ignores the closure, so after a header
    // edit every job would "hit" the old holder's Bloom filter and be routed there as a free cache hit. Affinity is
    // set from the FULL content key
    // once the closure is hashed (set_full_key_affinity), before the job is bound.
    sched_->on_job_ready(j.id, now());
    run_decide_locked();
    cv_.wait(l, [&] { return r->decided || stopping_.load(); });
    node = r->decided ? r->node : 0;
  }
  try {
    if (node == 0) {
      go_local(*r);   // the client's compiler reads its inputs after this
      send(s, {{"op", "decision"}, {"node", 0}, {"ack", true}});   // "stored" follows the client's done
      json done; if (!recv(s, &done)) throw std::runtime_error("client vanished");
      r->rc = done.value("rc", -1); r->obj_bytes = done.value("obj_bytes", 0ULL); r->outcome = "local";
      Hold hold(post_release_);   // store + record survive a SIGTERM that follows the release
      store_local_result(r, done, s);
      std::lock_guard<std::mutex> g(mu_);
      if (r->rc == 0) observe_speed_locked(r, 0, done.value("cpu_s", 0.0), true);   // client reference sample
      complete_locked(r, 0, done.value("cpu_s", 0.0));
      return;
    }
    if (r->mode == Mode::HeaderPush) {
      // header-push: no client preprocessing; the coordinator ships source + closure and the client just waits
      send(s, {{"op", "decision"}, {"node", node}, {"name", node > 0 ? topo_.node(node).name : std::string("pending")}, {"mode", "header_push"}});
      if (!r->closure_built) build_closure(*r);
      // a job that turns local after this (computed __has_include, a late-local binding) no longer
      // re-scans in store_local_result when nothing needs a post-compile closure: a computed-__has_include TU is stored
      // under its preprocessed key (no closure), and a TU with a dependency file keys its store on that file's inputs,
      // hashed after the compile; the write-back re-hashes every closure input after the compile and the worker
      // refuses an upload whose dependency file names an input the closure lacks. Without a dependency file the
      // store keys on the closure itself, which must then be taken after the compile: rebuilt as before.
      if (r->scan.has_md || r->has_include_computed) r->closure_built = true;
      if (node == kUnbound) set_full_key_affinity(r);   // affinity only for the exact closure content
      { std::lock_guard<std::mutex> g(r->m); r->preprocessed = true; }
      if (node == kUnbound) node = wait_binding(r);   // late binding: node chosen now that the closure is hashed
      if (r->has_include_computed && node > 0) {
        // correctness: __has_include(MACRO) somewhere in the closure -- the worker cannot know which file the test
        // needs and would silently evaluate it false; compile locally (rare: e.g. LLVM's TargetPassRegistry.inc)
        // a local compile like any other (go_local, "stored" handshake), so its result is cached under the
        // preprocessed key (store_pp) instead of being compiled again in every build
        { std::lock_guard<std::mutex> g(mu_); sched_->rebind_local(r->id, now()); }   // free node's slot now
        go_local(*r);
        send(s, {{"op", "fallback"}, {"reason", "computed __has_include"}, {"ack", true}});
        json done; recv(s, &done);
        r->rc = done.value("rc", -1); r->outcome = "fallback"; r->fallback = true; r->obj_bytes = done.value("obj_bytes", 0ULL);
        Hold hold(post_release_);
        store_local_result(r, done, s);
        std::lock_guard<std::mutex> g(mu_);
        complete_locked(r, node, done.value("cpu_s", 0.0));
        return;
      }
      if (node == 0) {
        go_local(*r);   // as above
        send(s, {{"op", "fallback"}, {"reason", "late-local"}, {"ack", true}});
        json done; recv(s, &done);
        r->rc = done.value("rc", -1); r->outcome = "local"; r->obj_bytes = done.value("obj_bytes", 0ULL);
        Hold hold(post_release_);
        store_local_result(r, done, s);
        std::lock_guard<std::mutex> g(mu_);
        if (r->rc == 0) observe_speed_locked(r, 0, done.value("cpu_s", 0.0), true);   // client reference sample
        complete_locked(r, 0, done.value("cpu_s", 0.0));
        return;
      }
      launch_attempt(r, node, false);
      maybe_verify(r, node);
      proto::CompileResult res; bool ok;
      {
        std::unique_lock<std::mutex> l(r->m);
        r->cv.wait(l, [&] { return r->finished; });
        ok = r->have_result; if (ok) res = r->result;
      }
      if (r->verify) resolve_verify(r, &res, &ok);
      if (ok && node_flagged(r->result_node)) { ok = false; r->last_error = "result from a worker that failed verification"; }
      if (!ok && !r->verify && !r->remote_compile_error) {   // a lost worker's job goes to another worker
        int n2 = redispatch_lost(r, node);
        if (n2 > 0) {
          node = n2;
          launch_attempt(r, node, false);
          std::unique_lock<std::mutex> l(r->m);
          r->cv.wait(l, [&] { return r->finished; });
          ok = r->have_result; if (ok) res = r->result;
          if (ok && node_flagged(r->result_node)) { ok = false; r->last_error = "result from a worker that failed verification"; }
        } else if (n2 == 0) node = 0;   // the scheduler chose the client: the fallback below compiles locally
      }
      if (!ok) {
        { std::lock_guard<std::mutex> g(mu_); sched_->rebind_local(r->id, now()); }   // the client compiles it
        send(s, {{"op", "fallback"}, {"reason", r->last_error}});
        json done; recv(s, &done);
        r->rc = done.value("rc", -1); r->outcome = "fallback"; r->fallback = true; r->obj_bytes = done.value("obj_bytes", 0ULL);
        std::lock_guard<std::mutex> g(mu_);
        complete_locked(r, node, done.value("cpu_s", 0.0));
        return;
      }
      if (client_gone(s)) throw std::runtime_error("client gone before delivery (wrapper timeout)");
      if (res.status == "ok") {
        util::write_file(closure::absolutize(r->out, r->cwd), res.object);
        r->obj_bytes = res.object.size();
        if (!res.depfile.empty()) {
          std::string dp = r->scan.dep_file.empty() ? args::default_dep_name(r->out) : r->scan.dep_file;
          util::write_file(closure::absolutize(dp, r->cwd), res.depfile);
        }
      }
      { std::lock_guard<std::mutex> g(r->m); r->rounds = std::max(r->rounds, res.rounds); r->miss_bytes = res.miss_bytes; }
      Hold hold(post_release_);   // the client cache store and the job record below survive a SIGTERM
      send(s, {{"op", "result"}, {"rc", res.rc}, {"stdout", res.out}, {"stderr", res.err}, {"node", r->result_node}, {"mode", "header_push"}});
      r->rc = res.rc; r->outcome = "remote";
      // only where a store would otherwise happen (client cache on, the result cacheable), so a run without a cache
      // neither counts toolchain skips nor pays for the identity checks
      if (res.status == "ok" && res.rc == 0 && local_cache_.enabled() && !r->lkey.empty() && !r->has_include_computed) {
        // the object was produced (or served) by the WORKER's toolchain; it is filed under this client's
        // compiler and system-include identity (client cache, direct entry, shared manifest) only when the worker reports
        // the same compiler id and system-include content fingerprint — a worker left on another compiler or another
        // libc-dev otherwise launders its objects into every client's caches. An old worker reports nothing: not cached.
        const std::string cid = cachekey::compiler_id(r->cc_local.empty() ? (r->argv.empty() ? std::string() : r->argv[0]) : r->cc_local);
        if (res.compiler_id.empty() || res.sysinc_fp.empty() || res.compiler_id != cid || res.sysinc_fp != sysinc_content(r->cc_local)) {
          toolchain_skips_++; note_skip(*r, "store-skipped", "worker toolchain differs from the client's (or not reported)");
        } else store_client_cache(r, res);
      }
      std::lock_guard<std::mutex> g(mu_);
      // measured work: the header-push worker's CPU includes its preprocessing, and so does w_j (from source)
      if (res.status == "ok" && res.cache != "hit") observe_speed_locked(r, r->result_node, res.cpu_s, true);
      double w = std::max(0.0, res.cpu_s * topo_.node(r->result_node).speed);
      complete_locked(r, r->result_node, w);
      return;
    }
    send(s, {{"op", "decision"}, {"node", node}, {"name", node > 0 ? topo_.node(node).name : std::string("pending")}});
    json pp; if (!recv(s, &pp)) throw std::runtime_error("client vanished");
    if (pp.value("op", "") != "preprocessed") {
      // preprocessing failed: the client compiles locally to surface the error
      send(s, {{"op", "fallback"}, {"reason", "preprocess failed"}});
      json done; recv(s, &done);
      r->rc = done.value("rc", -1); r->outcome = "fallback"; r->fallback = true;
      std::lock_guard<std::mutex> g(mu_);
      complete_locked(r, std::max(node, 0), done.value("cpu_s", 0.0));
      return;
    }
    r->pre_bytes = pp.value("pre_bytes", 0.0); r->pp_cpu = pp.value("pp_cpu_s", 0.0);
    {
      std::lock_guard<std::mutex> g(mu_);
      Job& j = wl_.jobs[static_cast<size_t>(r->id)];
      j.pre_size_bytes = r->pre_bytes; j.preprocess_cpu_s = r->pp_cpu;
    }
    { std::lock_guard<std::mutex> g(r->m); r->ifile = pp.value("ifile", ""); r->preprocessed = true; }
    // classic key needs the .i (read + SHA-256 of ~3 MB: ~7-15 ms of client CPU per kernel TU): only when some worker
    // has reported cached objects (the index is non-empty), i.e. never on cache-less pools
    if (node == kUnbound && !r->ifile.empty() && cache_idx_.size() > 0) {
      try {
        r->cache_key = cacheidx::client_classic_key(r->argv, r->scan.source_index, util::sha256_hex(util::read_file(r->ifile)));
        auto h = cache_idx_.holders(r->cache_key);
        if (!h.empty()) { std::lock_guard<std::mutex> g(mu_); sched_->set_affinity(r->id, h, cacheidx::kDefaultHitCost); }
      } catch (...) {}
    }
    if (node == kUnbound) node = wait_binding(r);   // late binding: node chosen now that the .i exists
    if (node == 0) {
      // every remote window stayed full: compile locally after all (the .i is discarded; the decision is forward-looking)
      send(s, {{"op", "fallback"}, {"reason", "late-local"}});
      json done; recv(s, &done);
      r->rc = done.value("rc", -1); r->outcome = "local"; r->obj_bytes = done.value("obj_bytes", 0ULL);
      std::lock_guard<std::mutex> g(mu_);
      complete_locked(r, 0, done.value("cpu_s", 0.0));
      return;
    }
    launch_attempt(r, node, false);
    maybe_verify(r, node);
    proto::CompileResult res; bool ok;
    {
      std::unique_lock<std::mutex> l(r->m);
      r->cv.wait(l, [&] { return r->finished; });
      ok = r->have_result; if (ok) res = r->result;
    }
    if (r->verify) resolve_verify(r, &res, &ok);
    if (ok && node_flagged(r->result_node)) { ok = false; r->last_error = "result from a worker that failed verification"; }
    if (!ok) {
      { std::lock_guard<std::mutex> g(mu_); sched_->rebind_local(r->id, now()); }   // the client compiles it
      send(s, {{"op", "fallback"}, {"reason", r->last_error}});
      json done; recv(s, &done);
      r->rc = done.value("rc", -1); r->outcome = "fallback"; r->fallback = true; r->obj_bytes = done.value("obj_bytes", 0ULL);
      std::lock_guard<std::mutex> g(mu_);
      complete_locked(r, node, done.value("cpu_s", 0.0));
      return;
    }
    if (client_gone(s)) throw std::runtime_error("client gone before delivery (wrapper timeout)");
    if (res.status == "ok") {
      std::string out = r->out;
      if (!out.empty() && out[0] != '/') out = r->cwd + "/" + out;
      util::write_file(out, res.object);
      r->obj_bytes = res.object.size();
    }
    send(s, {{"op", "result"}, {"rc", res.rc}, {"stdout", res.out}, {"stderr", res.err}, {"node", r->result_node}});
    r->rc = res.rc; r->outcome = "remote";
    std::lock_guard<std::mutex> g(mu_);
    if (res.status == "ok" && res.cache != "hit") observe_speed_locked(r, r->result_node, res.cpu_s, false);
    // w_j is from source: the worker compiled the .i, the client preprocessed it (the client is the reference)
    complete_locked(r, r->result_node, res.cpu_s * topo_.node(r->result_node).speed + std::max(0.0, r->pp_cpu));
  } catch (const std::exception& e) {
    log({{"event", "client-error"}, {"job", r->id}, {"error", e.what()}});
    std::lock_guard<std::mutex> g(mu_);
    if (r->t_done == 0) { r->outcome = "aborted"; complete_locked(r, std::max(node, 0), 0.0); }
  }
}


void Coordinator::serve_link(const net::Socket& s, const json& req) {
  std::vector<std::string> argv = req.value("argv", std::vector<std::string>{}); std::string cwd = req.value("cwd", "");
  last_activity_ = now();
  LinkOptions lo; lo.pipeline = o_.link_pipeline; lo.partition = o_.link_partition; lo.gc = o_.link_gc; lo.apply_pcrel = o_.link_pcrel;
  lo.zstd_level = o_.zstd_level; lo.report_path = o_.link_report; lo.seed = static_cast<unsigned>(o_.seed);
  // per-request overrides from the client's environment (settings may change between links of one build)
  if (!req.value("report", "").empty()) lo.report_path = req["report"];
  if (!req.value("pipeline", "").empty()) lo.pipeline = req["pipeline"];
  if (!req.value("partition", "").empty()) lo.partition = req["partition"];
  if (!req.value("gc", "").empty()) lo.gc = req["gc"] == "1";
  if (!req.value("pcrel", "").empty()) lo.apply_pcrel = req["pcrel"] == "1";
  { std::string f = req.value("force", ""); if (f.empty()) f = util::getenv_or("OPTICC_LINK_FORCE", "0"); lo.force = f == "1"; }   // per-request value wins
  { std::string mb = req.value("min_mb", ""); if (mb.empty()) mb = util::getenv_or("OPTICC_LINK_MIN_MB", ""); if (!mb.empty()) lo.min_mb = std::atof(mb.c_str()); }
  { std::string k = o_.link_keep; size_t p = 0; while (p <= k.size() && !k.empty()) { size_t e = k.find(',', p); std::string g = k.substr(p, e == std::string::npos ? std::string::npos : e - p); if (!g.empty()) lo.keep_globs.push_back(g); if (e == std::string::npos) break; p = e + 1; } }
  // what the coordinator knows about each object: the worker that compiled it keeps it under obj_sha (--keep-objects)
  std::map<std::string, LinkObject> known;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& r : jobs_) {
      if (r->out.empty()) continue;
      LinkObject o; o.path = closure::absolutize(r->out, r->cwd); o.weight = r->w_meas;
      if (r->outcome == "remote" && r->result_node > 0 && !r->result.obj_sha.empty()) { o.sha = r->result.obj_sha; o.node = r->result_node; o.size = r->obj_bytes; }
      known[o.path] = o;
    }
  }
  std::vector<net::WorkerConn*> conns; for (auto& c : conns_) conns.push_back(c.get());
  auto lookup = [&](const std::string& abs) { auto it = known.find(abs); return it != known.end() ? it->second : LinkObject{abs, "", 0, 0, 0}; };
  LinkOutcome out = run_link_pipeline(argv, cwd, lo, conns, lookup, [&](const json& j) { log(j); }, [&] { return next_rid_++; });
  { std::lock_guard<std::mutex> g(mu_); link_reports_.push_back(out.report); last_activity_ = now(); }
  send(s, {{"op", "link-plan"}, {"used_domains", out.used_domains}, {"argv", out.argv}, {"error", out.error}, {"report", out.report}});
}

void Coordinator::write_summary() {
  flush_hash_state();
  if (o_.summary_path.empty()) return;
  std::lock_guard<std::mutex> g(mu_);
  json nodes = json::array();
  json cl{{"name", "client"}, {"cores", topo_.client.cores}, {"jobs", 0}, {"window", sched_->windows().at(0)}};
  int local = 0; json per(json::array());
  std::vector<int> per_node(topo_.nodes.size() + 1, 0);
  uint64_t raw = 0, z = 0; int fallbacks = 0, hedges = 0, errors = 0, remote_err_retries = 0, hic_local = 0, client_hits = 0;
  for (auto& r : jobs_) {
    if (r->outcome == "remote") per_node[static_cast<size_t>(r->result_node)]++; else local++;   // quarantined jobs count as local
    raw += r->raw_bytes; z += r->zstd_bytes; fallbacks += r->fallback; hedges += r->hedges; errors += r->rc != 0;
    remote_err_retries += (r->remote_compile_error && r->fallback) ? 1 : 0;
    hic_local += r->has_include_computed ? 1 : 0;
    client_hits += r->outcome == "client-cache" ? 1 : 0;
    per.push_back({{"id", r->id}, {"src", r->src}, {"outcome", r->outcome}, {"node", r->outcome == "remote" ? r->result_node : 0},
                   {"mode", r->mode == Mode::HeaderPush ? "header_push" : "classic"},
                   {"rc", r->rc}, {"w_hat", r->w_hat}, {"w_meas", r->w_meas}, {"pre_bytes", r->pre_bytes}, {"pp_cpu_s", r->pp_cpu},
                   {"scan_s", r->scan_s}, {"closure_files", r->deps.size()}, {"closure_source", r->closure_source}, {"scan_files", r->scan_files},
                   {"shipped_bytes", r->shipped_bytes}, {"miss_bytes", r->miss_bytes}, {"piggy_bytes", r->piggy_bytes}, {"deps_prefetched", r->deps_prefetched}, {"rounds", r->rounds}, {"cache", r->cache_status},
                   {"zstd_bytes", r->zstd_bytes}, {"obj_bytes", r->obj_bytes}, {"t_request", r->t_request}, {"t_decided", r->t_decided},
                   {"t_done", r->t_done}, {"hedges", r->hedges}, {"attempts", r->attempts_total},
                   {"verify", r->verify}, {"verify_mismatch", r->verify_mismatch}});
  }
  cl["jobs"] = local; nodes.push_back(cl);
  for (auto& n : topo_.nodes) {
    nodes.push_back({{"name", n.name}, {"cores", n.cores}, {"speed", n.speed}, {"speed_samples", speed_est_ ? speed_est_->samples(n.id) : 0}, {"rtt_ms", n.rtt_s * 1e3}, {"bw_up_mbps", n.bw_up_bps / 1e6},
                     {"channel", node_channel_.size() > static_cast<size_t>(n.id) ? node_channel_[static_cast<size_t>(n.id)] : ""},
                     {"fingerprint", node_fingerprint_.size() > static_cast<size_t>(n.id) ? node_fingerprint_[static_cast<size_t>(n.id)] : ""},
                     {"verify_failed", node_verify_failed_.size() > static_cast<size_t>(n.id) ? node_verify_failed_[static_cast<size_t>(n.id)] : 0},
                     {"bw_down_mbps", n.bw_down_bps / 1e6}, {"jobs", per_node[static_cast<size_t>(n.id)]}, {"window", sched_->windows().at(static_cast<size_t>(n.id))},
                     {"disabled", sched_->node_disabled(n.id)}, {"bytes_sent", conns_[static_cast<size_t>(n.id - 1)]->bytes_sent()},
                     {"bytes_recv", conns_[static_cast<size_t>(n.id - 1)]->bytes_recv()},
                     {"prefetch_exact", pf_exact_.size() > static_cast<size_t>(n.id) && pf_exact_[static_cast<size_t>(n.id)]},
                     {"prefetch_bg_bytes", pf_bg_bytes_.size() > static_cast<size_t>(n.id) ? pf_bg_bytes_[static_cast<size_t>(n.id)] : 0},
                     {"prefetch_piggy_bytes", pf_piggy_bytes_.size() > static_cast<size_t>(n.id) ? pf_piggy_bytes_[static_cast<size_t>(n.id)] : 0},
                     {"prefetch_held", pf_.held_count(n.id)}});
  }
  int hp = 0; for (auto& r : jobs_) hp += r->outcome == "remote" && r->mode == Mode::HeaderPush;
  json s{{"policy", sched_->name()}, {"predictor", pred_->name()}, {"wall_s", now()}, {"jobs_total", jobs_.size()},
         {"mode_policy", json(topo_.params.mode_policy)}, {"jobs_header_push", hp}, {"nu", sched_->nu()},
         {"jobs_local", local}, {"jobs_remote", jobs_.size() - static_cast<size_t>(local)}, {"fallbacks", fallbacks}, {"remote_error_local_retries", remote_err_retries}, {"has_include_computed_jobs", hic_local}, {"client_cache_hits", client_hits}, {"bloom_affinity_jobs", bloom_affinity_jobs_}, {"missing_refused", missing_refused_}, {"attempt_timeouts", attempt_timeouts_},
         {"affinity", {{"bloom_jobs", bloom_affinity_jobs_}, {"index_jobs", aff_index_jobs_}, {"real_hits", aff_real_hits_}, {"misses", aff_misses_},
                       {"probed", aff_probed_.load()}, {"unready", aff_unready_.load()},
                       {"holder_hits", bloom_holder_hits_}, {"holder_outcomes", bloom_holder_outcomes_},
                       {"bloom_p", sched_->learned_hit_p(bloom_holder_hits_, bloom_holder_outcomes_)},
                       {"trust", [&] { json t = json::array(); for (size_t k = 1; k < trust_.size(); ++k) { int kk = static_cast<int>(k);
                           t.push_back({{"node", k}, {"hits", trust_.hits(kk)}, {"outcomes", trust_.outcomes(kk)}, {"p", trust_.p(kk)},
                                        {"wilson_lb", Scheduler::wilson_lower(trust_.hits(kk), trust_.outcomes(kk))}}); } return t; }()},
                       {"trust_resets", trust_resets_.load()}}},
         {"sysinc", [&] {   // fingerprint readiness (seconds since coordinator start; -1 = never ready)
            std::lock_guard<std::mutex> g(sysinc_warm_mu_);
            json per = json::object(); double ready = -1, scan = 0;
            for (auto& [c, tm] : sysinc_timing_) { per[c] = {{"ready_at_s", tm.ready_at_s}, {"scan_s", tm.scan_s}}; ready = std::max(ready, tm.ready_at_s); scan = std::max(scan, tm.scan_s); }
            return json{{"ready_at_s", ready}, {"scan_s", scan}, {"requests_before_ready", sysinc_before_ready_.load()},
                        {"wait_timeouts", sysinc_wait_timeouts_.load()}, {"wait_s", static_cast<double>(sysinc_wait_ns_.load()) / 1e9},
                        {"skipped_wait", sysinc_skipped_wait_.load()}, {"compilers", per}};
          }()},
         {"redispatches", redispatches_.load()},   // lost-worker jobs re-bound to another worker
         {"v4", {{"record_calls", rec_calls_.load()}, {"recorded", rec_ok_.load()}, {"record_dropped", rec_dropped_.load()},
                 {"local_only_jobs", local_only_jobs_.load()}, {"unexplained_dep_entries", unexplained_entries_.load()},
                 {"directive_skips", directive_skips_.load()}, {"racy_store_skips", racy_store_skips_.load()}, {"toolchain_skips", toolchain_skips_.load()}, {"closure_builds", closure_builds_.load()}, {"watcher_lag_max_s", static_cast<double>(watcher_lag_ns_.load()) / 1e9}, {"time_macros", [&] { std::lock_guard<std::mutex> g(tmf_mu_); return json{{"refusals", tm_refusals_.load()}, {"files", std::vector<std::string>(tm_files_seen_.begin(), tm_files_seen_.end())}}; }()}, {"pp", json{{"hits", pp_hits_.load()}, {"stores", pp_stores_.load()}, {"runs", pp_runs_.load()}, {"s", static_cast<double>(pp_ns_.load()) / 1e9}}}, {"racy_margin_s", direct::racy_margin_s()},
                 {"rdirect", rd_summary()}, {"exit_s", exit_s_},
                 {"uploads", {{"queued", up_queued_.load()}, {"sent", up_sent_.load()}, {"bytes", up_bytes_.load()}, {"skipped_held", up_skipped_held_.load()},
                              {"skipped_racy", up_skipped_racy_.load()}, {"dropped", up_dropped_.load()}, {"failed", up_failed_.load()},
                              {"left", [&] { std::lock_guard<std::mutex> g(up_mu_); return up_q_.size(); }()}}},
                 {"record_phase_s", [&] { json a = json::array(); for (auto& x : rec_phase_ns_) a.push_back(static_cast<double>(x.load()) / 1e9); return a; }()},
                 {"record_queue_left", [&] { std::lock_guard<std::mutex> g(rec_qmu_); return rec_q_.size(); }()}, {"record_s", static_cast<double>(rec_ns_.load()) / 1e9}, {"bg_deferred_s", static_cast<double>(bg_defer_ns_.load()) / 1e9},
                 {"verify_calls", scanner_.verify_calls.load()}, {"verify_s", static_cast<double>(scanner_.verify_ns.load()) / 1e9},
                 {"shadow_invalidations", scanner_.stats().shadow_invalidations},
                 {"skips", [&] { json k = json::object(); std::lock_guard<std::mutex> g(rec_skip_mu_); for (auto& [w, e] : rec_skip_) k[w] = {{"n", e.first}, {"example", e.second}}; return k; }()}}}, {"client_fast_hits", static_cast<long long>(util::file_size(state_dir_ + "/fasthits")) - static_cast<long long>(fasthits_at_start_)},
     {"client_probe_hits", static_cast<long long>(util::file_size(state_dir_ + "/probehits")) - static_cast<long long>(probehits_at_start_)}, {"hedges", hedges},
         {"compile_errors", errors}, {"bytes_raw", raw}, {"bytes_zstd", z}, {"rho", raw ? static_cast<double>(z) / static_cast<double>(raw) : 0.0},
         {"nodes", nodes}, {"probes", probes_}, {"late_admissions", late_admissions_}, {"prior_refreshes", prior_refreshes_}, {"jobs", per}, {"local_bench_s", local_bench_},
         {"security", topo_.params.security}, {"enc_bytes_per_s", enc_bps_}, {"verify_frac", o_.verify_frac},
         {"binding", json(topo_.params.binding)}, {"pred_gamma", o_.pred_gamma}, {"seed", o_.seed},
         {"npp", sched_->preprocess_admission()}, {"window_scale", topo_.params.window_scale}, {"windows", sched_->windows()},
         {"nu_updates", sched_->nu_updates()}, {"nu_direction_changes", sched_->nu_direction_changes()}, {"nu_peak", sched_->nu_peak()},
         {"i2_core_moves", [&] { auto* e = dynamic_cast<ECTScheduler*>(sched_.get()); return e ? e->i2_core_moves() : 0; }()},
         {"verify_jobs", verify_jobs_}, {"verify_mismatches", verify_mismatches_}, {"verify_recompiled", verify_recompiled_},
         {"prefetch", o_.prefetch}, {"prefetch_frac", o_.prefetch_frac}, {"prefetch_budget_mb", o_.prefetch_budget_mb}, {"prefetch_min_remote", o_.prefetch_min_remote}, {"prefetch_warm_mb", o_.prefetch_warm_mb}};
  {
    uint64_t miss = 0, piggy = 0, bg = 0; uint64_t deps = 0, deps_pf = 0;
    for (auto& r : jobs_) if (r->outcome == "remote" && r->mode == Mode::HeaderPush) { miss += r->miss_bytes; piggy += r->piggy_bytes; deps += r->deps.size(); deps_pf += static_cast<uint64_t>(r->deps_prefetched); }
    for (auto b : pf_bg_bytes_) bg += b;
    s["prefetch_totals"] = {{"critical_need_bytes", miss}, {"critical_piggy_bytes", piggy}, {"background_bytes", bg},
                            {"deps", deps}, {"deps_already_held", deps_pf}, {"held_hit_rate", deps ? static_cast<double>(deps_pf) / static_cast<double>(deps) : 0.0},
                            {"remote_launches", remote_launches_.load()}, {"gate_open_s", pf_gate_open_s_.load()}};
  }
  if (auto* ap = dynamic_cast<AffineSizePredictor*>(base_pred_.get())) s["predictor_fit"] = {{"a", ap->a()}, {"b", ap->b()}};
  if (hist_) s["tu_history"] = {{"loaded", hist_->loaded()}, {"entries", hist_->size()}, {"predictions", hist_->history_predictions()}};
  s["links"] = link_reports_; s["link_domains"] = o_.link_domains;
  try { save_json(o_.summary_path + ".tmp", s); std::rename((o_.summary_path + ".tmp").c_str(), o_.summary_path.c_str()); } catch (const std::exception& e) { fputs((std::string("opticc: summary: ") + e.what() + "\n").c_str(), stderr); }
}

// ---- header prefetch ----------------------------------------------------------------------
void Coordinator::prefetch_setup() {
  size_t n = topo_.nodes.size();
  pf_.resize(static_cast<int>(n));
  pf_bucket_.assign(n + 1, prefetch::TokenBucket());
  pf_piggy_bytes_.assign(n + 1, 0); pf_bg_bytes_.assign(n + 1, 0);
  for (auto& nd : topo_.nodes) {
    double rate = o_.prefetch_frac * std::max(nd.bw_up_bps, 1e6) / 8.0;     // bytes/s
    pf_bucket_[static_cast<size_t>(nd.id)].set(rate, std::max(1e6, 0.25 * rate));
  }
}

// warm start: the previous build's ranking (most referenced first) seeds the popularity table; a file edited since then
// hashes to a new sha and is simply pushed fresh. Done by the prefetch thread once the demand gate opens, never on the
// coordinator's startup path, and not at all for builds that never dispatch remotely.
// Format ("#opticc-popular 2"): one line per file, most referenced first:
//   sha <TAB> size <TAB> dev <TAB> ino <TAB> mtime_ns <TAB> ctime_ns <TAB> path
// An entry is used only when the file's stat still matches exactly, so loading needs stats, no hashing. The sha is only
// a hint for ranking and held-set bookkeeping: every background push re-hashes the bytes it sends and skips a file
// whose content no longer has that sha (prefetch_send), so a stale entry can never push wrong content. A file in the
// older format (one path per line) is hashed by the prefetch thread, never on the startup path.
void Coordinator::prefetch_warm_load() {
  std::ifstream in(o_.prefetch_warm);
  std::vector<std::string> lines; std::string line;
  while (std::getline(in, line)) if (!line.empty()) lines.push_back(line);
  if (lines.empty()) return;
  const bool v2 = lines[0] == "#opticc-popular 2";
  size_t used = 0;
  const size_t n = lines.size();
  for (size_t i = v2 ? 1 : 0; i < n; ++i) {
    const uint64_t weight = static_cast<uint64_t>(n - i);
    if (!v2) {
      uint64_t sz = 0; std::string sha;
      try { sha = hash_file(lines[i], &sz); } catch (...) { continue; }
      pf_.note(sha, lines[i], sz, weight); ++used; continue;
    }
    std::vector<std::string> f; size_t a = 0;
    for (int k = 0; k < 6; ++k) { size_t t = lines[i].find('\t', a); if (t == std::string::npos) break; f.push_back(lines[i].substr(a, t - a)); a = t + 1; }
    if (f.size() != 6) continue;
    const std::string path = lines[i].substr(a);
    try {
      direct::FStat rec; rec.exists = true; rec.size = std::stoull(f[1]); rec.dev = std::stoull(f[2]); rec.ino = std::stoull(f[3]);
      rec.mtime_ns = std::stoll(f[4]); rec.ctime_ns = std::stoll(f[5]);
      if (!direct::same(direct::stat_path(path), rec)) continue;   // changed since it was ranked: jobs will note it again
      pf_.note(f[0], path, rec.size, weight); ++used;
    } catch (...) {}
  }
  fputs(("opticc: prefetch warm list " + std::to_string(used) + " files\n").c_str(), stderr);
}

void Coordinator::prefetch_send(int node, const std::vector<proto::FileRef>& files, bool background, uint64_t* raw_out) {
  auto& conn = conns_.at(static_cast<size_t>(node - 1));
  proto::BlobsMsg b; uint64_t batch = 0, raw = 0;
  auto flush = [&] {
    if (b.blobs.empty()) return;
    uint64_t sent = 0; conn->send_prefetch(b, o_.zstd_level, &sent);
    for (auto& [sha, data] : b.blobs) pf_.mark(node, sha, data.size());
    b.blobs.clear(); batch = 0;
  };
  for (auto& f : files) {
    if (pf_.held(node, f.sha)) continue;
    std::string data;
    try { data = util::read_file(f.path); } catch (...) { continue; }
    if (background && util::sha256_hex(data) != f.sha) continue;   // edited since ranked: never push mismatched content
    raw += data.size(); batch += data.size();
    b.blobs.emplace_back(f.sha, std::move(data));
    if (batch >= (1u << 20)) flush();                               // <= ~1 MiB frames: the worker stores them inline
  }
  flush();
  {
    std::lock_guard<std::mutex> g(mu_);
    if (background) pf_bg_bytes_[static_cast<size_t>(node)] += raw; else pf_piggy_bytes_[static_cast<size_t>(node)] += raw;
  }
  if (raw_out) *raw_out = raw;
}

void Coordinator::prefetch_loop() {
  const uint64_t budget = static_cast<uint64_t>(o_.prefetch_budget_mb * 1e6);
  // two phases, so that background pushes (read + sha256 + zstd + encryption, up to the budget per node) do not run in
  // full at every coordinator start, also for builds answered from the caches with no remote work:
  //   warm phase (ungated): at most prefetch_warm_mb per node, the top of the warm ranking — the headers the first jobs
  //     will need, pushed before they are dispatched (the warm-start benefit; gating it would delay the first jobs'
  //     headers);
  //   bulk phase: the rest of the budget, only after prefetch_min_remote remote dispatches (a clean build reaches that
  //     within its first second; a cache-answered build never does).
  // Critical-path header delivery (NEED, piggyback) is unaffected by either.
  prefetch_warm_load();   // stats only; off the startup path
  const uint64_t warm_budget = std::min(budget, static_cast<uint64_t>(std::max(0.0, o_.prefetch_warm_mb) * 1e6));
  while (!stopping_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool gate = remote_launches_.load() >= static_cast<uint64_t>(o_.prefetch_min_remote);
    if (gate && pf_gate_open_s_.load() < 0) pf_gate_open_s_ = now();
    const uint64_t allow = gate ? budget : warm_budget;
    for (auto& nd : topo_.nodes) {
      size_t k = static_cast<size_t>(nd.id);
      if (k >= pf_exact_.size() || !pf_exact_[k]) continue;                    // warm-from-before store: NEED stays exact
      bool off; { std::lock_guard<std::mutex> g(mu_); off = sched_->node_disabled(nd.id); }
      if (off || !conns_[k - 1]->connected()) continue;
      uint64_t done; { std::lock_guard<std::mutex> g(mu_); done = pf_bg_bytes_[k]; }
      if (done >= allow) continue;
      double avail = pf_bucket_[k].available(util::now_s());
      if (avail < 64e3) continue;
      uint64_t take = std::min<uint64_t>(static_cast<uint64_t>(avail), allow - done);
      auto picks = pf_.pick(nd.id, take, 4096);
      if (picks.empty()) continue;
      std::vector<proto::FileRef> files; uint64_t bytes = 0;
      for (auto& [sha, f] : picks) { files.push_back(proto::FileRef{f.path, sha, f.size}); bytes += f.size; }
      pf_bucket_[k].take(static_cast<double>(bytes), util::now_s());
      try { prefetch_send(nd.id, files, true, nullptr); } catch (...) {}
    }
  }
}

void Coordinator::prefetch_save_ranking() {
  try {
    auto top = pf_.top(20000);
    if (top.empty()) return;
    std::string dir = util::dirname(o_.prefetch_warm); util::mkdir_p(dir);
    std::string tmp = o_.prefetch_warm + ".tmp", out = "#opticc-popular 2\n";
    for (auto& [sha, f] : top) {   // stat-validated entries (prefetch_warm_load)
      const direct::FStat st = direct::stat_path(f.path);
      if (!st.exists || st.is_dir) continue;
      out += sha + "\t" + std::to_string(st.size) + "\t" + std::to_string(st.dev) + "\t" + std::to_string(st.ino) + "\t" +
             std::to_string(st.mtime_ns) + "\t" + std::to_string(st.ctime_ns) + "\t" + f.path + "\n";
    }
    util::write_file(tmp, out); std::rename(tmp.c_str(), o_.prefetch_warm.c_str());
  } catch (...) {}
}

#include "rdirect_coord.inc"

int Coordinator::run() {
  // exactly one coordinator per socket. Concurrent first compiles each spawn one (the wrapper's connect loop); without
  // the lock a second coordinator would unlink the first one's socket, serve part of the build, and unlink it at exit.
  // The lock is held (fd kept open) for the coordinator's lifetime; a loser exits quietly.
  {
    int lfd = ::open((o_.sock_path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0 && ::flock(lfd, LOCK_EX | LOCK_NB) != 0) { ::close(lfd); return 0; }
  }
  t0_ = util::now_s();
  setup_topology();
  probe_workers();
  pred_ = make_predictor(json{{"predictor", o_.predictor}}, o_.seed); base_pred_ = pred_;
  if (o_.pred_gamma > 1.0) pred_ = std::make_shared<GammaNoisePredictor>(pred_, o_.pred_gamma, o_.seed);
  // per-TU times from earlier builds (client cache state; loaded below once its directory is known)
  if (util::getenv_or("OPTICC_LOCAL_CACHE", "on") != "off" && util::getenv_or("OPTICC_TU_HISTORY", "on") != "off") {
    hist_ = std::make_shared<HistoryPredictor>(pred_); pred_ = hist_;
  }
  sched_ = make_scheduler(o_.policy, topo_, wl_, pred_);
  for (int k : unreachable_) sched_->disable_node(k);   // retried by admission_loop()
  std::thread(&Coordinator::admission_loop, this).detach();
  ::unlink((o_.sock_path + ".cache").c_str());   // rewritten below when the client cache is enabled
  unlink_sysinc_files(o_.sock_path);              // a previous coordinator's signatures are never trusted
  if (util::getenv_or("OPTICC_LOCAL_CACHE", "on") != "off") {
    try {
      std::string dir = util::getenv_or("OPTICC_LOCAL_CACHE_DIR", util::getenv_or("HOME", "/tmp") + "/.cache/opticc/objects");
      double gb = std::atof(util::getenv_or("OPTICC_LOCAL_CACHE_GB", "5").c_str());
      local_cache_.init(dir, static_cast<uint64_t>(std::max(0.1, gb) * 1e9));
      state_dir_ = dir + "/state"; util::mkdir_p(state_dir_);
      if (hist_) { std::lock_guard<std::mutex> g(mu_); hist_->load(state_dir_ + "/tu_times.tsv"); }
      if (util::file_exists(state_dir_ + "/fasthits")) fasthits_at_start_ = static_cast<uint64_t>(util::file_size(state_dir_ + "/fasthits"));
      probehits_at_start_ = static_cast<uint64_t>(util::file_size(state_dir_ + "/probehits"));
      // tell compiler wrappers where THIS coordinator's cache is (their environment may differ from ours)
      util::write_file(o_.sock_path + ".cache.tmp", util::abspath(dir)); std::rename((o_.sock_path + ".cache.tmp").c_str(), (o_.sock_path + ".cache").c_str());
      // the content memo belongs to the MACHINE (its system headers), not to this cache: a fresh client cache reuses it
      sysinc_ = std::make_unique<sysinc::Tracker>(util::getenv_or("OPTICC_SYSINC_MEMO_DIR", util::getenv_or("HOME", "/tmp") + "/.cache/opticc/sysinc"),
                                                  std::atof(util::getenv_or("OPTICC_SYSINC_REFRESH_S", "10").c_str()));
      sysinc_->on_change([this](const std::string& c, const sysinc::State& st) { publish_sysinc(c, st.stat_sig); });
      // pre-warm only the drivers named in OPTICC_SYSINC_PREWARM (default "clang,clang++"; one canonical binary on
      // Ubuntu), so that a small client does not scan several toolchains just as the first compiles start. Any other
      // compiler is warmed on its first request.
      std::vector<std::string> prewarm;
      { std::string lst = util::getenv_or("OPTICC_SYSINC_PREWARM", "clang,clang++"), cur;
        for (char ch : lst + ",") { if (ch == ',') { if (!cur.empty()) prewarm.push_back(cur); cur.clear(); } else if (ch != ' ') cur += ch; } }
      for (const std::string& n : prewarm) {
        std::error_code ec; const std::string rc = cachekey::resolve_compiler(n);
        if (rc.find('/') == std::string::npos) continue;
        const std::string c = fs::canonical(rc, ec).string();
        if (!ec && !c.empty()) (void)sysinc_warm(c);   // stat signature and content fingerprint, normal priority
      }
      // the compiler identity (driver, helpers, shared libraries: ~1.2 s for clang) is
      // computed off the first wave's critical path; jobs that ask before it is done wait for this one computation
      std::thread([prewarm] { for (const std::string& n : prewarm) (void)cachekey::compiler_id(n); }).detach();
      load_hash_state();   // key hashes survive coordinator restarts (every entry is still stat-validated on use)
      for (int i = 0; i < 2; ++i) std::thread(&Coordinator::record_worker, this, true).detach();   // background recorders
      std::thread(&Coordinator::upload_worker, this).detach();   // write-back to the worker caches
      std::thread(&Coordinator::rd_publish_worker, this).detach();   // shared direct manifests
      try { scanner_.import_state(util::read_file(state_dir_ + "/scan.bin")); } catch (...) {}   // parse memo
    } catch (const std::exception& e) { fputs((std::string("opticc: client cache disabled: ") + e.what() + "\n").c_str(), stderr); }
  }
  if (o_.prefetch) { prefetch_setup(); std::thread(&Coordinator::prefetch_loop, this).detach(); }
  net::Socket ls;
  try { ls = net::Socket::listen_unix(o_.sock_path); } catch (const std::exception& e) { fputs((std::string("opticc coordinator: ") + e.what() + "\n").c_str(), stderr); return 1; }
  fputs(("opticc coordinator: policy " + sched_->name() + ", " + std::to_string(topo_.nodes.size()) + " worker(s), socket " + o_.sock_path + "\n").c_str(), stderr);
  signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGUSR1, on_signal); signal(SIGPIPE, SIG_IGN);
  // watcher: idle timeout, signals
  std::thread([this, &ls] {
    auto last = std::chrono::steady_clock::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      {   // the longest gap between two passes of this loop (timers, idle timeout, signals ride on it)
        const auto nw = std::chrono::steady_clock::now();
        const uint64_t gap = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nw - last).count()); last = nw;
        uint64_t m = watcher_lag_ns_.load(); while (gap > m && !watcher_lag_ns_.compare_exchange_weak(m, gap)) {}
      }
      int sig = g_signal.exchange(0);
      // released jobs first finish their store decision and job record (bounded, as at
      // exit): the client is released BEFORE its store runs, so a summary asked for right after the build returned could
      // predate the decision
      auto wait_released = [this] {
        const auto t0 = std::chrono::steady_clock::now();
        while (post_release_.load() > 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      };
      // a summary reflects every recorded entry and every local store of a fetched entry. It is written on a helper
      // thread: the wait (up to 30 s mid-build) must not hold up hedging timers, the idle timeout or SIGTERM
      // in this loop. Helpers are serialised (one summary at a time; write_summary itself runs under mu_).
      if (sig == SIGUSR1) {
        std::thread([this, wait_released] { std::lock_guard<std::mutex> g(usr1_mu_); wait_released(); drain_records(); rd_drain_local(); write_summary(); }).detach();
        continue;
      }
      // hedging timers: the scheduler asks to be called back at next_wakeup()
      { std::lock_guard<std::mutex> g(mu_); if (sched_ && sched_->next_wakeup(now()) <= now()) run_decide_locked(); }
      bool idle; { std::lock_guard<std::mutex> g(mu_); idle = inflight_ == 0 && now() - last_activity_ > o_.idle_timeout_s; }
      if (sig == SIGTERM || sig == SIGINT || idle) {
        stopping_ = true; cv_.notify_all();
        // the exit is timed per phase (summary "exit_s")
        auto tp = std::chrono::steady_clock::now();
        auto lap = [&](const char* k) { auto n = std::chrono::steady_clock::now(); std::lock_guard<std::mutex> g(mu_); exit_s_[k] = std::chrono::duration<double>(n - tp).count(); tp = n; };
        {   // exit_s_ is read by write_summary under mu_ (a SIGUSR1 summary may be running)
          size_t lq, pq; { std::lock_guard<std::mutex> g(rd_mu_); lq = rd_local_q_.size(); pq = rd_q_.size(); }
          std::lock_guard<std::mutex> g(mu_); exit_s_["rd_local_queued_at_exit"] = static_cast<double>(lq); exit_s_["rd_publish_queued_at_exit"] = static_cast<double>(pq);
        }
        wait_released();   // released jobs finish their store and record first (bounded: 30 s)
        lap("wait_released");
        drain_records();   // queued direct entries are written before the summary (the next build needs them)
        lap("drain_records");
        drain_uploads();   // client-local results to the worker caches (bounded; the rest is abandoned and counted)
        lap("drain_uploads");
        rd_drain();        // published direct manifests (bounded likewise)
        lap("rd_drain");
        write_summary();
        if (hist_ && !state_dir_.empty()) { std::lock_guard<std::mutex> g(mu_); hist_->save(state_dir_ + "/tu_times.tsv"); }
        if (o_.prefetch) prefetch_save_ranking();
        unlink_sysinc_files(o_.sock_path);
        ::unlink(o_.sock_path.c_str());
        ls.shutdown_both();
        _exit(0);
      }
    }
  }).detach();
  for (;;) {
    net::Socket c;
    try { c = ls.accept(); } catch (const std::exception&) { if (stopping_) break; continue; }
    std::thread(&Coordinator::serve_client, this, std::move(c)).detach();
  }
  return 0;
}

}  // namespace

int run(const Options& o) { Coordinator c(o); return c.run(); }

}  // namespace opticc::coord
