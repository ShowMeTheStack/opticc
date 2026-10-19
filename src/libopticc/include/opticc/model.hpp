// opticc: model types.
// Units: sizes in bytes, bandwidth in bits/s, time in seconds, work in
// reference-core CPU seconds. Node id 0 is always the client ("local").
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace opticc {

using json = nlohmann::json;

// Preprocessing mode of one remote job:
//   Classic    client preprocesses (p_j on a client core), ships rho*s_j
//   HeaderPush client ships the raw source plus the headers the worker's
//              content-addressed store lacks (rho*(s_src + miss)), pays only the
//              include-scan/hash cost q_j; the worker preprocesses (w_j/f_k; w_j is from source, classic workers do w_j-p_j)
//   Cached     HeaderPush with a warm store (miss = 0); same code path
enum class Mode { Classic, HeaderPush, Cached };
NLOHMANN_JSON_SERIALIZE_ENUM(Mode, {{Mode::Classic, "classic"},
                                    {Mode::HeaderPush, "header_push"},
                                    {Mode::Cached, "cached"}})
// Which modes a policy may choose (OPTICC_MODE / --mode): Auto = per job, per node.
enum class ModePolicy { Classic, HeaderPush, Auto };
// When the node of a remote job is chosen.
//   Early: at dispatch, before the client's preprocessing/hashing (as distcc does).
//   Late:  dispatch decides local vs remote only; the node is chosen by ECT when the preprocessed
//          bytes are ready and the per-node window counts from that moment.
enum class Binding { Early, Late };
NLOHMANN_JSON_SERIALIZE_ENUM(Binding, {{Binding::Early, "early"}, {Binding::Late, "late"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ModePolicy, {{ModePolicy::Classic, "classic"},
                                          {ModePolicy::HeaderPush, "header-push"},
                                          {ModePolicy::Auto, "auto"}})

// Remote worker k >= 1.
struct Node {
  int id = 0;                    // must equal position+1 in Topology::nodes (0 = unset)
  std::string name;
  int cores = 1;                 // c_k
  double mem_bytes = 0;          // M_k (0 = unlimited)
  double speed = 1.0;            // f_k, relative to the reference core
  double rtt_s = 0.0;            // L_k
  double bw_up_bps = 0.0;        // B_k^u  client -> node (0 = unlimited)
  double bw_down_bps = 0.0;      // B_k^d  node -> client (0 = unlimited)
  double price_per_hour = 0.0;   // pi_k
};

// The client (node 0).
struct Client {
  int cores = 1;                 // c_0
  double speed = 1.0;            // f_0
  double mem_bytes = 0;          // 0 = unlimited
  double egress_bps = 0.0;       // B_0^u shared by all uploads (0 = unlimited)
  double ingress_bps = 0.0;      // B_0^d shared by all downloads (0 = unlimited)
  double price_per_hour = 0.0;
};

// Translation unit (or barrier job such as header generation / final link).
struct Job {
  int id = 0;
  std::string name;
  double work_ref_cpu_s = 0.0;   // w_j
  double preprocess_cpu_s = 0.0; // p_j (client CPU, classic mode; worker CPU in header-push)
  double scan_cpu_s = 0.0;       // q_j client cost in header-push (closure lookup + hashing; 0 when a .d is reused)
  double pre_size_bytes = 0.0;   // s_j   preprocessed source
  double src_size_bytes = 0.0;   // s_j^src source file bytes (shipped in header-push)
  double closure_bytes = 0.0;    // total bytes of the non-system headers j includes (header-push miss volume on a cold store)
  std::vector<int> headers;      // header ids (index into Workload::header_sizes) for store simulation
  double obj_size_bytes = 0.0;   // o_j
  double mem_bytes = 0.0;        // m_j peak RSS
  std::vector<int> deps;         // predecessor job ids
  bool is_link_barrier = false;  // must run locally; consumes everything before it
};

// Model-wide parameters and policy knobs.
struct Params {
  double alpha = 1.0;            // connection round trips per job (1 pipelined, 2 distcc)
  double rho = 0.2;              // compression ratio applied to shipped bytes
  int zstd_level = 3;            // informational
  double header_miss_bytes = -1; // header-push: prior expected miss bytes per job (<0 = use the job's closure until observed)
  ModePolicy mode_policy = ModePolicy::Classic;
  // Resource-priced earliest-completion time (ECT): cost = C_hat + nu * (client CPU seconds the option consumes).
  bool priced = true;
  double nu = -1;                // >= 0: static shadow price; < 0: adaptive (multiplicative update, see sched.cpp)
  // policy knobs
  double hedge_kappa = 2.0;      // kappa rule: duplicate after kappa * queue-aware predicted completion
  int hedge_max = -1;            // max concurrent hedges (-1 = number of remote nodes)
  bool work_conserving = true;   // localhost-last gate + no idle remote slot while jobs wait
  double hedge_lambda_min = 0.1; // hedging rule: floor on the duplicate-work weight lambda
  double hedge_prior_sigma = 0.3;// hedging rule: log-normal prior on actual/predicted until 30 samples
  // distcc emulation ships uncompressed source unless the host spec says ",lzo".
  // The default is ",lzo", with a placeholder compression ratio of 0.35.
  std::string distcc_compression = "lzo";
  double rho_lzo = 0.35;
  // Connection security: "noise" charges encryption CPU for every byte
  // shipped and received (client side in p_j/q_j, worker side in its compile time); "plain" charges nothing.
  // Model default is "plain" so analytic tests stay exact; the coordinator sets it from the channels
  // actually in use (noise whenever any worker connection is encrypted) and opticc-sim takes --security.
  std::string security = "plain";
  double enc_bytes_per_s = 1.5e9;  // measured by the coordinator's probe (ChaCha20-Poly1305 encrypt+decrypt)
  Binding binding = Binding::Late;  // late binding (opticc policies); distcc emulation is always early
  double window_scale = 1.0;     // multiply the window N_k* for sweeps; 0 = plain slot count (OPTICC_WINDOW=slots)
  int local_slots = -1;          // -1 = client cores (opticc policies); non-distributable jobs bypass it
  std::vector<int> host_slots;   // distcc "/N" per remote node, empty = cores
  // distcc emulation: compile slots of the "localhost/N" entry in DISTCC_HOSTS (4 for
  // "localhost/4"); 0 = no localhost entry (real distcc then compiles nothing locally).
  int distcc_localhost_slots = 4;
  // distcc-pump include server: per-job serialised client cost; pump ships the whole
  // closure with every job (no worker-side cross-job store). 5 ms is a placeholder.
  double q_pump_s = 0.005;
  // Client CPU per job not attributable to the compiler: make/fixdep/objtool/wrapper and
  // zstd. Charged on the client pool for every job under every policy. < 0 = take the workload's value
  // (trace_to_sim derives it from the traced build: (CPU% x wall - sum w - sum p) / n), 0 if absent.
  double client_overhead_cpu_s_per_job = -1;
  // Floor on the mean client CPU per shipped job used in nu_max: hashing/scan cost is never below this.
  double min_client_cpu_s = 1e-3;
};

struct Workload {
  std::vector<Job> jobs;         // ids must equal index
  std::vector<double> header_sizes;  // bytes per header id (header-push store simulation); may be empty
  double client_overhead_cpu_s_per_job = 0.0;  // measured per-job client overhead of the traced build
  int release_window = 0;        // make -j of the traced build (0 = unknown); the sim's default release model
  double mean_pre_size() const;
  double mean_obj_size() const;
  double mean_work() const;
  double mean_mem() const;
  double total_work() const;
  std::vector<std::vector<int>> successors() const;
};

struct Topology {
  Client client;
  std::vector<Node> nodes;       // nodes[i].id == i+1
  Params params;
  // effective concurrency ~c_k = min(c_k, floor(M_k / mean_m))
  int capacity(int node_id, double mean_mem) const;
  const Node& node(int id) const { return nodes.at(static_cast<size_t>(id - 1)); }
  double speed(int id) const { return id == 0 ? client.speed : node(id).speed; }
  int num_nodes() const { return static_cast<int>(nodes.size()) + 1; }  // incl. client
  // Set nodes[i].id = i+1 (hand-built topologies); from_json does this already.
  void assign_ids() { for (size_t i = 0; i < nodes.size(); ++i) nodes[i].id = static_cast<int>(i) + 1; }
  // Throws unless nodes[i].id == i+1 for all i.
  void check_ids() const;
};

// Shipped bytes b_jk for a given mode, with the compression ratio of the
// transport in use and, for header-push, the expected miss volume on that node.
double shipped_bytes(const Job& j, Mode mode, const Params& p);
double shipped_bytes(const Job& j, Mode mode, const Params& p, double rho);
double shipped_bytes(const Job& j, Mode mode, const Params& p, double rho, double miss_bytes);
// Default miss volume when nothing has been observed on a node yet.
double default_miss_bytes(const Job& j, const Params& p);
// Client CPU seconds (reference) an option consumes: w_j locally, p_j classic, q_j header-push.
double client_cpu_of(const Job& j, int node, Mode mode);
// Same plus the encryption cost of the bytes the option ships and receives (Params::security == "noise").
double client_cpu_of(const Job& j, int node, Mode mode, const Params& p, double shipped);
// Reference-CPU seconds to encrypt/decrypt `bytes` under the channel policy in p (0 for plain).
double enc_cpu_s(double bytes, const Params& p);

// Uncontended remote completion time R_jk. Pure transfer+compute,
// no queueing. Uses the given work estimate (true or predicted).
double remote_time(const Job& j, double work_est, const Node& k, const Client& c,
                   const Params& p, Mode mode = Mode::Classic, double miss_bytes = -1);

// w_j is the work of compiling j FROM SOURCE -- what a local compile and the trace recorder measure (preprocessing
// included). A worker compiling the preprocessed .i (classic) does w_j - p_j; a header-push worker preprocesses itself
// and does w_j. Floored at 5% of w (p_j <= w_j by definition; estimates may not be).
inline double remote_compile_work(double w, double p, Mode m) { return m == Mode::Classic ? std::max(w - p, 0.05 * w) : w; }

// Transfer helper: bytes over bps (0 bps = unlimited -> 0 s).
inline double xfer_s(double bytes, double bps) { return bps <= 0 ? 0.0 : bytes * 8.0 / bps; }

void to_json(json& j, const Node& n);
void from_json(const json& j, Node& n);
void to_json(json& j, const Client& c);
void from_json(const json& j, Client& c);
void to_json(json& j, const Job& x);
void from_json(const json& j, Job& x);
void to_json(json& j, const Params& p);
void from_json(const json& j, Params& p);
void to_json(json& j, const Workload& w);
void from_json(const json& j, Workload& w);
void to_json(json& j, const Topology& t);
void from_json(const json& j, Topology& t);

Topology load_topology(const std::string& path);
Workload load_workload(const std::string& path);
json load_json(const std::string& path);
void save_json(const std::string& path, const json& j);

}  // namespace opticc
