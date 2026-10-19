// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/model.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <fstream>
#include <stdexcept>

namespace opticc {

double Workload::mean_pre_size() const {
  double s = 0; int n = 0;
  for (auto& j : jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) { s += j.pre_size_bytes; ++n; }
  return n ? s / n : 0.0;
}
double Workload::mean_obj_size() const {
  double s = 0; int n = 0;
  for (auto& j : jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) { s += j.obj_size_bytes; ++n; }
  return n ? s / n : 0.0;
}
double Workload::mean_work() const {
  double s = 0; int n = 0;
  for (auto& j : jobs) if (!j.is_link_barrier && j.pre_size_bytes > 0) { s += j.work_ref_cpu_s; ++n; }
  return n ? s / n : 0.0;
}
double Workload::mean_mem() const {
  double s = 0; int n = 0;
  for (auto& j : jobs) if (j.mem_bytes > 0) { s += j.mem_bytes; ++n; }
  return n ? s / n : 0.0;
}
double Workload::total_work() const {
  double s = 0;
  for (auto& j : jobs) s += j.work_ref_cpu_s;
  return s;
}
std::vector<std::vector<int>> Workload::successors() const {
  std::vector<std::vector<int>> succ(jobs.size());
  for (auto& j : jobs)
    for (int d : j.deps) succ.at(static_cast<size_t>(d)).push_back(j.id);
  return succ;
}

int Topology::capacity(int node_id, double mean_mem) const {
  int cores = node_id == 0 ? client.cores : node(node_id).cores;
  double mem = node_id == 0 ? client.mem_bytes : node(node_id).mem_bytes;
  if (mem <= 0 || mean_mem <= 0) return cores;
  int by_mem = static_cast<int>(std::floor(mem / mean_mem));
  return std::max(1, std::min(cores, by_mem));
}

void Topology::check_ids() const {
  for (size_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].id != static_cast<int>(i) + 1)
      throw std::runtime_error("topology: nodes[" + std::to_string(i) + "].id must be " + std::to_string(i + 1) +
                               " (call Topology::assign_ids() on hand-built topologies)");
}

double default_miss_bytes(const Job& j, const Params& p) {
  return p.header_miss_bytes >= 0 ? p.header_miss_bytes : j.closure_bytes;
}
double shipped_bytes(const Job& j, Mode mode, const Params& p, double rho, double miss_bytes) {
  switch (mode) {
    case Mode::Classic: return rho * j.pre_size_bytes;
    case Mode::HeaderPush: return rho * (j.src_size_bytes + std::max(0.0, miss_bytes < 0 ? default_miss_bytes(j, p) : miss_bytes));
    case Mode::Cached: return rho * j.src_size_bytes;
  }
  return rho * j.pre_size_bytes;
}
double shipped_bytes(const Job& j, Mode mode, const Params& p, double rho) { return shipped_bytes(j, mode, p, rho, -1); }
double shipped_bytes(const Job& j, Mode mode, const Params& p) { return shipped_bytes(j, mode, p, p.rho, -1); }

double client_cpu_of(const Job& j, int node, Mode mode) {
  if (node == 0) return j.work_ref_cpu_s;
  return mode == Mode::Classic ? j.preprocess_cpu_s : j.scan_cpu_s;
}
double enc_cpu_s(double bytes, const Params& p) {
  return (p.security == "noise" && p.enc_bytes_per_s > 0) ? bytes / p.enc_bytes_per_s : 0.0;
}
double client_cpu_of(const Job& j, int node, Mode mode, const Params& p, double shipped) {
  if (node == 0) return j.work_ref_cpu_s;
  return client_cpu_of(j, node, mode) + enc_cpu_s(shipped + j.obj_size_bytes, p);
}

// Classic:     p/f0 + alpha L + rho s/B_up + (w-p)/f_k + o/B_down
// HeaderPush:  q/f0 + alpha L + rho (s_src+miss)/B_up + w/f_k + o/B_down   (the worker preprocesses; w is from source)
double remote_time(const Job& j, double work_est, const Node& k, const Client& c,
                   const Params& p, Mode mode, double miss_bytes) {
  double bytes = shipped_bytes(j, mode, p, p.rho, miss_bytes);
  double enc = enc_cpu_s(bytes + j.obj_size_bytes, p);   // both ends encrypt/decrypt what they send/receive
  double client = ((mode == Mode::Classic ? j.preprocess_cpu_s : j.scan_cpu_s) + enc) / c.speed;
  return client + p.alpha * k.rtt_s + xfer_s(bytes, k.bw_up_bps) +
         (remote_compile_work(work_est, j.preprocess_cpu_s, mode) + enc) / k.speed + xfer_s(j.obj_size_bytes, k.bw_down_bps);
}

// ---- JSON -------------------------------------------------------------
void to_json(json& j, const Node& n) {
  j = json{{"id", n.id}, {"name", n.name}, {"cores", n.cores}, {"mem_bytes", n.mem_bytes},
           {"speed", n.speed}, {"rtt_s", n.rtt_s}, {"bw_up_bps", n.bw_up_bps},
           {"bw_down_bps", n.bw_down_bps}, {"price_per_hour", n.price_per_hour}};
}
void from_json(const json& j, Node& n) {
  n.id = j.value("id", n.id); n.name = j.value("name", n.name);
  n.cores = j.value("cores", n.cores); n.mem_bytes = j.value("mem_bytes", n.mem_bytes);
  n.speed = j.value("speed", n.speed); n.rtt_s = j.value("rtt_s", n.rtt_s);
  n.bw_up_bps = j.value("bw_up_bps", n.bw_up_bps); n.bw_down_bps = j.value("bw_down_bps", n.bw_down_bps);
  n.price_per_hour = j.value("price_per_hour", n.price_per_hour);
}
void to_json(json& j, const Client& c) {
  j = json{{"cores", c.cores}, {"speed", c.speed}, {"mem_bytes", c.mem_bytes},
           {"egress_bps", c.egress_bps}, {"ingress_bps", c.ingress_bps},
           {"price_per_hour", c.price_per_hour}};
}
void from_json(const json& j, Client& c) {
  c.cores = j.value("cores", c.cores); c.speed = j.value("speed", c.speed);
  c.mem_bytes = j.value("mem_bytes", c.mem_bytes);
  c.egress_bps = j.value("egress_bps", c.egress_bps); c.ingress_bps = j.value("ingress_bps", c.ingress_bps);
  c.price_per_hour = j.value("price_per_hour", c.price_per_hour);
}
void to_json(json& j, const Job& x) {
  j = json{{"id", x.id}, {"name", x.name}, {"w", x.work_ref_cpu_s}, {"p", x.preprocess_cpu_s}, {"q", x.scan_cpu_s},
           {"s", x.pre_size_bytes}, {"s_src", x.src_size_bytes}, {"closure", x.closure_bytes}, {"o", x.obj_size_bytes},
           {"m", x.mem_bytes}, {"deps", x.deps}, {"is_link_barrier", x.is_link_barrier}};
  if (!x.headers.empty()) j["headers"] = x.headers;
}
void from_json(const json& j, Job& x) {
  x.id = j.value("id", x.id); x.name = j.value("name", x.name);
  x.work_ref_cpu_s = j.value("w", x.work_ref_cpu_s); x.preprocess_cpu_s = j.value("p", x.preprocess_cpu_s);
  x.pre_size_bytes = j.value("s", x.pre_size_bytes); x.src_size_bytes = j.value("s_src", x.src_size_bytes);
  x.obj_size_bytes = j.value("o", x.obj_size_bytes); x.mem_bytes = j.value("m", x.mem_bytes);
  x.deps = j.value("deps", x.deps); x.is_link_barrier = j.value("is_link_barrier", x.is_link_barrier);
  x.scan_cpu_s = j.value("q", x.scan_cpu_s); x.closure_bytes = j.value("closure", x.closure_bytes);
  x.headers = j.value("headers", x.headers);
}
void to_json(json& j, const Params& p) {
  j = json{{"alpha", p.alpha}, {"rho", p.rho}, {"zstd_level", p.zstd_level},
           {"header_miss_bytes", p.header_miss_bytes}, {"hedge_kappa", p.hedge_kappa},
           {"hedge_max", p.hedge_max}, {"work_conserving", p.work_conserving}, {"hedge_lambda_min", p.hedge_lambda_min}, {"hedge_prior_sigma", p.hedge_prior_sigma},
           {"distcc_compression", p.distcc_compression}, {"rho_lzo", p.rho_lzo}, {"window_scale", p.window_scale},
           {"local_slots", p.local_slots}, {"host_slots", p.host_slots},
           {"mode_policy", p.mode_policy}, {"priced", p.priced}, {"nu", p.nu},
           {"security", p.security}, {"enc_bytes_per_s", p.enc_bytes_per_s}, {"binding", p.binding},
           {"distcc_localhost_slots", p.distcc_localhost_slots}, {"q_pump_s", p.q_pump_s},
           {"client_overhead_cpu_s_per_job", p.client_overhead_cpu_s_per_job}, {"min_client_cpu_s", p.min_client_cpu_s}};
}
void from_json(const json& j, Params& p) {
  p.alpha = j.value("alpha", p.alpha); p.rho = j.value("rho", p.rho);
  p.zstd_level = j.value("zstd_level", p.zstd_level);
  p.header_miss_bytes = j.value("header_miss_bytes", p.header_miss_bytes);
  p.hedge_kappa = j.value("hedge_kappa", p.hedge_kappa); p.hedge_max = j.value("hedge_max", p.hedge_max); p.work_conserving = j.value("work_conserving", p.work_conserving);
  p.hedge_lambda_min = j.value("hedge_lambda_min", p.hedge_lambda_min); p.hedge_prior_sigma = j.value("hedge_prior_sigma", p.hedge_prior_sigma);
  p.distcc_compression = j.value("distcc_compression", p.distcc_compression); p.rho_lzo = j.value("rho_lzo", p.rho_lzo);
  p.window_scale = j.value("window_scale", p.window_scale);
  p.local_slots = j.value("local_slots", p.local_slots); p.host_slots = j.value("host_slots", p.host_slots);
  p.mode_policy = j.value("mode_policy", p.mode_policy); p.priced = j.value("priced", p.priced); p.nu = j.value("nu", p.nu);
  p.security = j.value("security", p.security); p.enc_bytes_per_s = j.value("enc_bytes_per_s", p.enc_bytes_per_s);
  p.binding = j.value("binding", p.binding);
  p.distcc_localhost_slots = j.value("distcc_localhost_slots", p.distcc_localhost_slots);
  p.q_pump_s = j.value("q_pump_s", p.q_pump_s);
  p.client_overhead_cpu_s_per_job = j.value("client_overhead_cpu_s_per_job", p.client_overhead_cpu_s_per_job);
  p.min_client_cpu_s = j.value("min_client_cpu_s", p.min_client_cpu_s);
}
void to_json(json& j, const Workload& w) {
  j = json{{"jobs", w.jobs}};
  if (!w.header_sizes.empty()) j["header_sizes"] = w.header_sizes;
  if (w.client_overhead_cpu_s_per_job > 0) j["client_overhead_cpu_s_per_job"] = w.client_overhead_cpu_s_per_job;
  if (w.release_window > 0) j["release_window"] = w.release_window;
}
void from_json(const json& j, Workload& w) {
  w.jobs = j.at("jobs").get<std::vector<Job>>();
  w.header_sizes = j.value("header_sizes", std::vector<double>{});
  w.client_overhead_cpu_s_per_job = j.value("client_overhead_cpu_s_per_job", 0.0);
  w.release_window = j.value("release_window", 0);
  for (size_t i = 0; i < w.jobs.size(); ++i)
    if (w.jobs[i].id != static_cast<int>(i)) throw std::runtime_error("workload: job ids must equal index");
}
void to_json(json& j, const Topology& t) { j = json{{"client", t.client}, {"nodes", t.nodes}, {"params", t.params}}; }
void from_json(const json& j, Topology& t) {
  t.client = j.value("client", Client{});
  t.nodes = j.value("nodes", std::vector<Node>{});
  if (j.contains("params")) t.params = j.at("params").get<Params>();
  for (size_t i = 0; i < t.nodes.size(); ++i) t.nodes[i].id = static_cast<int>(i) + 1;
}

json load_json(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  json j; f >> j; return j;
}
void save_json(const std::string& path, const json& j) {
  std::ofstream f(path);
  if (!f) throw std::runtime_error("cannot write " + path);
  f << j.dump(2) << "\n";
}
Topology load_topology(const std::string& path) { return load_json(path).get<Topology>(); }
Workload load_workload(const std::string& path) { return load_json(path).get<Workload>(); }

}  // namespace opticc
