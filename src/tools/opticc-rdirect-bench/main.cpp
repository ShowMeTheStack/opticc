// SPDX-License-Identifier: GPL-2.0-only
// opticc-rdirect-bench: replays the fresh-client VALIDATION of shared direct manifests (rdirect.hpp)
// over a real source tree, without a worker or a compile.
//
//   record    for every TU of a compile_commands.json (C/C++ only): the include closure, the system and other inputs
//             and the absent include candidates, exactly as the coordinator's record_direct() derives them from the
//             scanner (incscan::Scanner closure + direct_inputs + negative_groups), hashed as it does (comment-insensitive
//             key hash unless the argv keeps comments) with the publisher's exact-bytes hash (fsha), encoded and decoded
//             as on the wire (rdirect::encode / decode). Deviations: no dependency file (TUs with a computed include,
//             which need one, are skipped), no precompiled-header candidates.
//   validate  a FRESH rdirect::Validator (empty memos, as a fresh client's coordinator) validates every entry with
//             N threads (the build's concurrency), each thread taking the next TU — per-TU wall time, the Validator's
//             thread-CPU and wall split per phase, digests per hash kind, distinct files.
//
// Usage: opticc-rdirect-bench --cc compile_commands.json [--j 8,56] [--limit N] [--repeat R] [--drop-caches] [--no-fsha]
//                             [--label NAME] [--out result.json]
// --drop-caches (Linux, root): echo 3 > /proc/sys/vm/drop_caches before every validation run (cold page cache).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "opticc/argscan.hpp"
#include "opticc/cachekey.hpp"
#include "opticc/closure.hpp"
#include "opticc/direct.hpp"
#include "opticc/incscan.hpp"
#include "opticc/rdirect.hpp"
#include "opticc/util.hpp"

using namespace opticc;
using json = nlohmann::json;

namespace {

// POSIX-shell word splitting of a compile_commands "command" (quotes and backslashes; no expansion)
std::vector<std::string> shell_split(const std::string& s) {
  std::vector<std::string> out; std::string cur; bool have = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n') { if (have) { out.push_back(cur); cur.clear(); have = false; } continue; }
    have = true;
    if (c == '\\' && i + 1 < s.size()) { cur += s[++i]; continue; }
    if (c == '\'') { while (++i < s.size() && s[i] != '\'') cur += s[i]; continue; }
    if (c == '"') {
      while (++i < s.size() && s[i] != '"') { if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\')) ++i; cur += s[i]; }
      continue;
    }
    cur += c;
  }
  if (have) out.push_back(cur);
  return out;
}

std::string lang_of(const std::string& src) {
  auto ext = src.substr(src.rfind('.') == std::string::npos ? src.size() : src.rfind('.'));
  if (ext == ".c") return "c";
  if (ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".C") return "c++";
  return "";
}

struct Tu { std::string src; rdirect::PEntry e; };

double secs(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

}  // namespace

int main(int argc, char** argv) {
  std::string cc_json, label = "bench", out_f; std::vector<int> js{8, 56}; size_t limit = 0; int repeat = 1; bool drop = false, no_fsha = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--cc") cc_json = next();
    else if (a == "--j") { js.clear(); std::string v = next(); size_t p = 0; while (p < v.size()) { size_t q = v.find(',', p); if (q == std::string::npos) q = v.size(); js.push_back(std::atoi(v.substr(p, q - p).c_str())); p = q + 1; } }
    else if (a == "--limit") limit = static_cast<size_t>(std::atol(next().c_str()));
    else if (a == "--drop-caches") drop = true;
    else if (a == "--repeat") repeat = std::max(1, std::atoi(next().c_str()));
    else if (a == "--no-fsha") no_fsha = true;
    else if (a == "--label") label = next();
    else if (a == "--out") out_f = next();
    else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
  }
  if (cc_json.empty()) { std::fprintf(stderr, "usage: opticc-rdirect-bench --cc compile_commands.json [--j 8,56] [--limit N] [--drop-caches] [--no-fsha] [--label L] [--out F]\n"); return 2; }
  json cmds = json::parse(util::read_file(cc_json));

  // ---- record (single thread, like one recording client) ----
  const auto r0 = std::chrono::steady_clock::now();
  incscan::Scanner scanner;
  rdirect::Validator recorder;
  std::map<std::string, std::vector<std::string>> sysd_memo;
  std::vector<Tu> tus;
  struct Hic { std::vector<std::string> argv; std::string cwd, src; };
  std::vector<Hic> hic_tus;   // computed __has_include: keyed by preprocessed text
  std::map<std::string, size_t> skipped;
  for (auto& c : cmds) {
    if (limit && tus.size() >= limit) break;
    std::vector<std::string> av;
    if (c.contains("arguments")) for (auto& x : c["arguments"]) av.push_back(x.get<std::string>());
    else av = shell_split(c.value("command", std::string()));
    const std::string cwd = c.value("directory", std::string());
    std::string src = c.value("file", std::string());
    if (av.empty() || cwd.empty() || src.empty()) { skipped["malformed"]++; continue; }
    const std::string lang = lang_of(src);
    if (lang.empty()) { skipped["not C/C++"]++; continue; }
    const std::string src_abs = closure::absolutize(src, cwd);
    std::string key = av[0] + "\x01" + lang;
    auto it = sysd_memo.find(key);
    if (it == sysd_memo.end()) it = sysd_memo.emplace(key, closure::system_include_dirs(av[0], lang, cwd)).first;
    const auto& sysd = it->second;
    const auto dirs = args::include_dirs(av);
    bool computed = false, hic = false; int sf = 0;
    std::vector<std::string> clo;
    try { clo = scanner.closure(src_abs, dirs, sysd, cwd, &computed, &sf, &hic); } catch (...) { skipped["scan failed"]++; continue; }
    if (hic) { skipped["computed __has_include"]++; hic_tus.push_back({av, cwd, src_abs}); continue; }
    std::vector<std::string> extra, comp; std::vector<std::pair<int, int>> negp; bool hic2 = false;
    scanner.direct_inputs(src_abs, clo, dirs, sysd, cwd, &extra, &negp, &comp, &hic2);
    if (hic2) { skipped["computed __has_include"]++; hic_tus.push_back({av, cwd, src_abs}); continue; }
    if (!comp.empty()) { skipped["computed include (needs a dependency file)"]++; continue; }
    std::vector<std::string> paths{src_abs};
    paths.insert(paths.end(), clo.begin(), clo.end());
    paths.insert(paths.end(), extra.begin(), extra.end());
    std::sort(paths.begin(), paths.end()); paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    direct::Entry e; e.obj = util::sha256_hex(src_abs); e.recorded_wall = direct::wall_now();
    e.keysha = !cachekey::keeps_comments(av);
    const std::vector<direct::FStat> sts = direct::stat_many(paths);
    bool ok = true;
    for (size_t i = 0; i < paths.size(); ++i) {
      if (!sts[i].exists || sts[i].is_dir) { ok = false; break; }
      direct::Pos p; p.path = paths[i]; p.st = sts[i];
      p.sha = recorder.hash_of(paths[i], sts[i], e.keysha ? rdirect::Validator::Key : rdirect::Validator::Raw);
      if (p.sha.empty()) { ok = false; break; }
      e.pos.push_back(std::move(p));
    }
    if (!ok) { skipped["input missing or unreadable"]++; continue; }
    if (!scanner.negative_groups(negp, direct::wall_now(), &e.neg)) { skipped["absent candidate exists"]++; continue; }
    rdirect::PEntry pe = rdirect::from_direct(e, util::sha256_hex(e.obj));
    if (!no_fsha) for (size_t i = 0; i < e.pos.size(); ++i) pe.pos[i].fsha = recorder.hash_of(e.pos[i].path, e.pos[i].st, rdirect::Validator::Fast);
    std::vector<rdirect::PEntry> dec;
    if (!rdirect::decode(rdirect::encode({pe}), &dec) || dec.size() != 1) { skipped["encode/decode"]++; continue; }
    tus.push_back(Tu{src_abs, std::move(dec[0])});
  }
  const double rec_s = secs(r0, std::chrono::steady_clock::now());
  size_t npos = 0, nneg = 0; std::set<std::string> files;
  for (auto& t : tus) { npos += t.e.pos.size(); for (auto& g : t.e.neg) nneg += g.names.size(); for (auto& p : t.e.pos) files.insert(p.path); }
  uint64_t file_bytes = 0; for (auto& f : files) file_bytes += direct::stat_path(f).size;
  json out{{"label", label}, {"tus", tus.size()}, {"record_s", rec_s}, {"positives", npos}, {"absent_names", nneg},
           {"distinct_files", files.size()}, {"distinct_bytes", file_bytes}, {"skipped", skipped}, {"no_fsha", no_fsha},
           {"keysha_entries", std::count_if(tus.begin(), tus.end(), [](const Tu& t) { return t.e.keysha; })}, {"runs", json::array()}};
  std::printf("[%s] recorded %zu TUs in %.1f s: %zu positives (%zu distinct files, %.1f MB), %zu absent names; skipped %s\n",
              label.c_str(), tus.size(), rec_s, npos, files.size(), static_cast<double>(file_bytes) / 1e6, nneg, json(skipped).dump().c_str());

  // ---- what a MISS costs on a preprocessed-text key ----
  // gate check (always paid by such a TU): read the main source + sha256 (the index key's source part; the key itself is
  // one more sha256 and a Bloom lookup); preprocessing (paid before the compile only when the index key is in some
  // worker's filter): `-E -o pp.i -MD|-MMD -MF pp.d` without -c/-o/-M*, as the coordinator runs it
  {
    json pc = json::array();
    for (auto& h : hic_tus) {
      const auto g0 = std::chrono::steady_clock::now();
      std::string sha; try { sha = util::sha256_hex(util::read_file(h.src)); } catch (...) {}
      const double gate_s = secs(g0, std::chrono::steady_clock::now());
      std::vector<std::string> a;
      bool md = false;
      for (size_t i = 0; i < h.argv.size(); ++i) {
        const std::string& x = h.argv[i];
        if (x == "-c" || x == "-MD" || x == "-MMD" || x == "-MP" || x == "-M" || x == "-MM") { if (x == "-MD") md = true; continue; }
        if (x == "-o" || x == "-MF" || x == "-MT" || x == "-MQ") { ++i; continue; }
        if (x.rfind("-MF", 0) == 0 || x.rfind("-MT", 0) == 0 || x.rfind("-MQ", 0) == 0) continue;
        if (x.rfind("-Wp,", 0) == 0 && (x.find("-MD") != std::string::npos || x.find("-MMD") != std::string::npos)) { if (x.find("-MD") != std::string::npos && x.find("-MMD") == std::string::npos) md = true; continue; }
        a.push_back(x);
      }
      std::string dir = util::make_temp_dir("opticc-rdb-pp-");
      a.push_back("-E"); a.push_back("-o"); a.push_back(dir + "/pp.i"); a.push_back(md ? "-MD" : "-MMD"); a.push_back("-MF"); a.push_back(dir + "/pp.d");
      const auto p0 = std::chrono::steady_clock::now();
      util::RunResult rr = util::run_process(a, "", dir + "/err", h.cwd);
      const double pp_s = secs(p0, std::chrono::steady_clock::now());
      uint64_t bytes = 0; try { bytes = util::read_file(dir + "/pp.i").size(); } catch (...) {}
      util::remove_tree(dir);
      pc.push_back({{"src", h.src}, {"gate_s", gate_s}, {"pp_s", pp_s}, {"rc", rr.rc}, {"pp_bytes", bytes}});
      std::printf("[%s] computed __has_include TU %s: gate (read + sha256) %.4f s, preprocessing %.3f s (rc %d, %.1f MB)\n", label.c_str(),
                  h.src.c_str(), gate_s, pp_s, rr.rc, static_cast<double>(bytes) / 1e6);
    }
    out["pp_miss_cost"] = pc;
  }

  // ---- validate (fresh Validator per run) ----
  std::vector<int> runs_j;
  for (int rep = 0; rep < repeat; ++rep) runs_j.insert(runs_j.end(), js.begin(), js.end());
  for (int j : runs_j) {
    if (j <= 0) continue;
    if (drop) {
      ::sync();
      std::ofstream dc("/proc/sys/vm/drop_caches"); dc << "3\n";
      if (!dc) std::fprintf(stderr, "warning: could not drop the page cache (not root / not Linux)\n");
    }
    rdirect::Validator v;
    std::atomic<size_t> next{0}; std::atomic<size_t> bad{0};
    std::vector<double> per(tus.size(), 0.0);
    std::mutex wm; std::map<std::string, size_t> whys;
    const auto w0 = std::chrono::steady_clock::now();
    std::vector<std::thread> th;
    for (int t = 0; t < j; ++t)
      th.emplace_back([&] {
        for (;;) {
          size_t i = next++;
          if (i >= tus.size()) return;
          const auto a = std::chrono::steady_clock::now();
          std::string why; direct::Entry local;
          const bool ok = v.valid(tus[i].e, &why, &local);
          per[i] = secs(a, std::chrono::steady_clock::now());
          if (!ok) { bad++; std::lock_guard<std::mutex> g(wm); whys[why.substr(0, why.find(':'))]++; }
        }
      });
    for (auto& t : th) t.join();
    const double wall = secs(w0, std::chrono::steady_clock::now());
    std::vector<double> s = per; std::sort(s.begin(), s.end());
    double sum = 0; for (double x : s) sum += x;
    auto q = [&](double f) { return s.empty() ? 0.0 : s[std::min(s.size() - 1, static_cast<size_t>(f * static_cast<double>(s.size())))]; };
    auto S = [](const std::atomic<uint64_t>& ns) { return static_cast<double>(ns.load()) / 1e9; };
    json kinds = json::object(); const char* names[3] = {"raw", "key", "fast"};
    for (int k = 0; k < 3; ++k)
      kinds[names[k]] = {{"n", v.n_digest[k].load()}, {"bytes", v.bytes_digest[k].load()}, {"cpu_s", S(v.cpu_digest[k])}, {"wall_s", S(v.ns_digest[k])},
                         {"ms_per_digest_wall", v.n_digest[k] ? 1e3 * S(v.ns_digest[k]) / static_cast<double>(v.n_digest[k].load()) : 0.0},
                         {"ms_per_digest_cpu", v.n_digest[k] ? 1e3 * S(v.cpu_digest[k]) / static_cast<double>(v.n_digest[k].load()) : 0.0}};
    json run{{"j", j}, {"wall_s", wall}, {"tus_per_s", wall > 0 ? static_cast<double>(tus.size()) / wall : 0.0},
             {"per_tu_ms", {{"mean", tus.empty() ? 0.0 : 1e3 * sum / static_cast<double>(tus.size())}, {"p50", 1e3 * q(0.5)}, {"p90", 1e3 * q(0.9)}, {"max", 1e3 * q(1.0)}}},
             {"invalid", bad.load()}, {"invalid_why", whys},
             {"phase_wall_s", {{"stat", S(v.ns_stat)}, {"hash", S(v.ns_hash)}, {"neg", S(v.ns_neg)}}},
             {"phase_cpu_s", {{"stat", S(v.cpu_stat)}, {"hash", S(v.cpu_hash)}, {"neg", S(v.cpu_neg)}}},
             {"digest", kinds}, {"memo_hits", v.memo_hits.load()}, {"hashed_files", v.hashed_files()}, {"distinct_files", v.distinct_files()},
             {"no_fsha", v.no_fsha.load()}, {"fsha_mismatch", v.fsha_mismatch.load()}, {"listed_dirs", v.listed_dirs()}, {"drop_caches", drop},
             {"neg_access", v.neg_access.load()}, {"neg_listed", v.neg_listed.load()},
             {"neg_access_why", {{"dir_missing", v.neg_access_why[0].load()}, {"not_pinned", v.neg_access_why[1].load()},
                                 {"dot_component", v.neg_access_why[2].load()}, {"last_listed", v.neg_access_why[3].load()}}}};
    out["runs"].push_back(run);
    std::printf("[%s] -j %d: %.2f s wall, %.0f TU/s, per TU mean %.2f ms p50 %.2f p90 %.2f max %.1f; invalid %zu; "
                "phase wall/cpu s: stat %.2f/%.2f hash %.2f/%.2f neg %.2f/%.2f; digests raw %llu key %llu fast %llu "
                "(fast %.3f ms wall %.3f ms cpu each, key %.3f/%.3f); distinct %llu hashed %llu memo hits %llu; no_fsha %llu mismatch %llu; "
                "neg access %llu listed %llu (access: dir missing %llu, not pinned %llu, dot %llu, last listed %llu)\n",
                label.c_str(), j, wall, run["tus_per_s"].get<double>(), run["per_tu_ms"]["mean"].get<double>(), 1e3 * q(0.5), 1e3 * q(0.9), 1e3 * q(1.0), bad.load(),
                S(v.ns_stat), S(v.cpu_stat), S(v.ns_hash), S(v.cpu_hash), S(v.ns_neg), S(v.cpu_neg),
                (unsigned long long)v.n_digest[0].load(), (unsigned long long)v.n_digest[1].load(), (unsigned long long)v.n_digest[2].load(),
                kinds["fast"]["ms_per_digest_wall"].get<double>(), kinds["fast"]["ms_per_digest_cpu"].get<double>(),
                kinds["key"]["ms_per_digest_wall"].get<double>(), kinds["key"]["ms_per_digest_cpu"].get<double>(),
                (unsigned long long)v.distinct_files(), (unsigned long long)v.hashed_files(), (unsigned long long)v.memo_hits.load(),
                (unsigned long long)v.no_fsha.load(), (unsigned long long)v.fsha_mismatch.load(),
                (unsigned long long)v.neg_access.load(), (unsigned long long)v.neg_listed.load(),
                (unsigned long long)v.neg_access_why[0].load(), (unsigned long long)v.neg_access_why[1].load(),
                (unsigned long long)v.neg_access_why[2].load(), (unsigned long long)v.neg_access_why[3].load());
  }
  if (!out_f.empty()) { std::ofstream f(out_f); f << out.dump(1) << "\n"; }
  return 0;
}
