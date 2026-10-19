// SPDX-License-Identifier: GPL-2.0-only
#include "link_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <thread>
#include <unistd.h>

#include "opticc/closure.hpp"
#include "opticc/domains/partition.hpp"
#include "opticc/elf/liveness.hpp"
#include "opticc/elf/manifest.hpp"
#include "opticc/elf/prelink.hpp"
#include "opticc/elf/wire.hpp"
#include "opticc/util.hpp"

namespace opticc::coord {
using json = nlohmann::json;

static bool ends_with(const std::string& s, const char* suf) { size_t n = std::strlen(suf); return s.size() >= n && s.compare(s.size() - n, n, suf) == 0; }

bool looks_like_link(const std::vector<std::string>& argv) {
  bool has_o = false; int objs = 0;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-c" || a == "-E" || a == "-S" || a == "-M" || a == "-MM" || a == "--version" || a == "-fsyntax-only") return false;
    if (a == "-o") has_o = true;
    if (a[0] != '-' && (ends_with(a, ".o") || ends_with(a, ".a") || a[0] == '@')) objs++;
  }
  return has_o && objs >= 2;
}

// Thin archive: "!<thin>\n" then 60-byte member headers; member names are "/N" offsets into the "//" long-name table,
// paths are relative to the archive's directory. Regular archives (members embedded) return false.
bool expand_thin_archive(const std::string& path, std::vector<std::string>* members) {
  std::string d; try { d = util::read_file(path); } catch (...) { return false; }
  if (d.compare(0, 8, "!<thin>\n") != 0) return false;
  size_t off = 8; std::string longnames; std::string dir = util::dirname(path);
  while (off + 60 <= d.size()) {
    std::string name = d.substr(off, 16); std::string sizes = d.substr(off + 48, 10);
    uint64_t sz = std::strtoull(sizes.c_str(), nullptr, 10);
    off += 60;
    auto trim = [](std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '/')) s.pop_back(); return s; };
    if (name.compare(0, 2, "//") == 0) { longnames = d.substr(off, sz); off += sz + (sz & 1); continue; }   // long-name table is stored inline
    if (name[0] == '/' && name[1] == ' ') { off += sz + (sz & 1); continue; }                              // symbol index (stored inline)
    std::string n;
    if (name[0] == '/' && std::isdigit(static_cast<unsigned char>(name[1]))) {
      size_t lo = std::strtoull(name.c_str() + 1, nullptr, 10);
      size_t e = longnames.find('\n', lo); n = longnames.substr(lo, e == std::string::npos ? std::string::npos : e - lo);
      if (!n.empty() && n.back() == '/') n.pop_back();
    } else n = trim(name);
    if (!n.empty()) members->push_back(n[0] == '/' ? n : dir + "/" + n);
    // thin archive: member data is NOT stored; no size skip
  }
  return true;
}

namespace {
struct Domain {
  int id = 0; int node = 0;                      // node that prelinks it (0 = coordinator itself)
  std::vector<int> objs;                         // indices into `all`, link order
  elf::DomainManifest manifest; bool have_manifest = false;
  std::string prelinked; json pre_manifest, stats;
  double t_manifest_sent = 0, t_manifest_recv = 0, t_prelink_sent = 0, t_prelink_recv = 0;
  uint64_t shipped_bytes = 0; int shipped_objects = 0; uint64_t manifest_bytes = 0;
  std::string error;
};
}  // namespace

static elf::Roots roots_for(const std::vector<std::string>& argv, const LinkOptions& opt) {
  elf::Roots r; std::string entry = "_start";
  for (size_t i = 1; i < argv.size(); ++i) {
    if ((argv[i] == "-e" || argv[i] == "--entry") && i + 1 < argv.size()) entry = argv[i + 1];
    else if (argv[i].rfind("--entry=", 0) == 0) entry = argv[i].substr(8);
    else if (argv[i].rfind("-Wl,-e,", 0) == 0) entry = argv[i].substr(7);
  }
  r.entry_symbols = {entry}; r.keep_globs = opt.keep_globs; return r;
}

LinkOutcome run_link_pipeline(const std::vector<std::string>& argv, const std::string& cwd, const LinkOptions& opt,
                              const std::vector<net::WorkerConn*>& conns, const Lookup& lookup, const Log& log,
                              const std::function<int64_t()>& next_id) {
  LinkOutcome out; out.argv = argv;
  double T0 = util::now_s();
  auto T = [&] { return util::now_s() - T0; };
  json rep; rep["pipeline"] = opt.pipeline; rep["partition"] = opt.partition; rep["gc"] = opt.gc; rep["apply_pcrel"] = opt.apply_pcrel;
  try {
    // ---- 1. object arguments ---------------------------------------------------------------------------------
    bool relocatable = false, static_link = false, export_dyn = false, shared = false;
    for (auto& a : argv) { if (a == "-r" || a == "--relocatable") relocatable = true; if (a == "-static" || a == "--static" || a == "-Bstatic") static_link = true;
                           if (a == "--export-dynamic" || a == "-rdynamic" || a == "-E") export_dyn = true; if (a == "-shared") shared = true; }
    struct Arg { size_t idx; std::vector<std::string> objs; };   // argument index -> objects it contributes
    std::vector<Arg> oargs; std::vector<std::string> all_paths;
    for (size_t i = 1; i < argv.size(); ++i) {
      const std::string& a = argv[i];
      if (a.empty() || a[0] == '-') { if ((a == "-o" || a == "-e" || a == "-T" || a == "--entry" || a == "-L" || a == "-l") && i + 1 < argv.size()) ++i; continue; }
      std::string abs = closure::absolutize(a, cwd);
      if (ends_with(a, ".o") && util::file_exists(abs)) { oargs.push_back({i, {abs}}); all_paths.push_back(abs); continue; }
      if (ends_with(a, ".a")) { std::vector<std::string> m; if (expand_thin_archive(abs, &m) && !m.empty()) { for (auto& x : m) all_paths.push_back(x); oargs.push_back({i, m}); } continue; }
      if (a[0] == '@' && util::file_exists(abs.substr(0, 0) + closure::absolutize(a.substr(1), cwd))) {
        std::string rsp = util::read_file(closure::absolutize(a.substr(1), cwd)); std::vector<std::string> m;
        size_t p = 0; while (p < rsp.size()) { size_t e = rsp.find_first_of(" \n\t", p); std::string tok = rsp.substr(p, e == std::string::npos ? std::string::npos : e - p); if (!tok.empty() && ends_with(tok, ".o")) m.push_back(closure::absolutize(tok, cwd)); if (e == std::string::npos) break; p = e + 1; }
        if (!m.empty()) { for (auto& x : m) all_paths.push_back(x); oargs.push_back({i, m}); }
      }
    }
    if (all_paths.size() < 2) throw std::runtime_error("fewer than two object inputs; nothing to domain-link");
    bool final_gc = false; for (auto& a : argv) if (a == "--gc-sections" || a == "-Wl,--gc-sections" || a.find(",--gc-sections") != std::string::npos) final_gc = true;
    // domain-side liveness stripping is only legitimate when the final link itself garbage-collects; a plain link
    // keeps unreferenced sections, and so must we. Relocatable output (-r) keeps everything regardless.
    bool gc = opt.gc && !relocatable && final_gc;
    bool nonpreemptible = relocatable || static_link || (!shared && !export_dyn);
    // ---- 2. object records -----------------------------------------------------------------------------------
    std::vector<LinkObject> all; all.reserve(all_paths.size());
    for (auto& p : all_paths) {
      LinkObject o = lookup(p); o.path = p;
      if (o.sha.empty()) { std::string b = util::read_file(p); o.sha = util::sha256_hex(b); o.size = b.size(); }
      if (o.node > 0 && (static_cast<size_t>(o.node) > conns.size() || !conns[static_cast<size_t>(o.node - 1)] || !conns[static_cast<size_t>(o.node - 1)]->connected())) o.node = 0;
      all.push_back(o);
    }
    int W = 0; for (auto* c : conns) if (c && c->connected()) W++;
    // ---- 2b. benefit gate ---------------------------------------------------------------------------------------
    // Domains pay only when the final link has real work to shed AND the objects already sit on the workers.
    //  * a relocatable link without --gc-sections keeps every section; ld.lld merges a Linux kernel's vmlinux.o
    //    (about 2,800 objects) in about 0.1 s, so there is nothing to win and a plan costs more than the link;
    //  * the coordinator's own (client) domain must stay small: condensing a domain of thousands of local objects on
    //    the client takes minutes of the client's CPU;
    //  * tiny links (total input below OPTICC_LINK_MIN_MB, default 128) finish faster than a manifest round trip.
    // OPTICC_LINK_FORCE=1 bypasses the gate (tests only).
    if (!opt.force) {
      uint64_t bytes = 0; size_t remote = 0;
      for (auto& o : all) { bytes += o.size; if (o.node > 0) remote++; }
      double rfrac = all.empty() ? 0.0 : static_cast<double>(remote) / static_cast<double>(all.size());
      double min_mb = opt.min_mb > 0 ? opt.min_mb : 128.0;
      rep["gate"] = {{"bytes", bytes}, {"remote_fraction", rfrac}, {"relocatable", relocatable}, {"final_gc", final_gc}, {"min_mb", min_mb}};
      std::string why;
      if (relocatable && !final_gc) why = "relocatable link without --gc-sections: no residual work to remove";
      else if (rfrac < 0.9) why = "only " + std::to_string(static_cast<int>(rfrac * 100)) + "% of the objects are held by workers (need >= 90%)";
      else if (static_cast<double>(bytes) < min_mb * 1e6) why = "input " + std::to_string(bytes / 1000000) + " MB below the " + std::to_string(static_cast<int>(min_mb)) + " MB threshold";
      if (!why.empty()) throw std::runtime_error("benefit gate: " + why);
    }
    // ---- 3. domain formation ---------------------------------------------------------------------------------
    std::vector<int> dom_of(all.size(), 0); json cut;
    if (opt.partition == "placement" || W == 0) {
      for (size_t i = 0; i < all.size(); ++i) dom_of[i] = all[i].node;
    } else {
      // object reference graph from the local copies (the coordinator has every object)
      elf::ObjectSet set = elf::ObjectSet::load(all_paths);
      elf::SectionGraph g = elf::build_section_graph(set);
      domains::Graph G; G.n = static_cast<int>(all.size()); G.names = all_paths;
      for (auto& o : all) G.vw.push_back(o.weight > 0 ? o.weight : static_cast<double>(std::max<uint64_t>(o.size, 1)));
      std::map<std::pair<int, int>, std::pair<double, double>> agg;
      for (auto& [arc, cc] : g.arcs) { int u = arc.first.obj, v = arc.second.obj; if (u == v || u < 0 || v < 0) continue; auto k = std::minmax(u, v); auto& e = agg[{k.first, k.second}]; e.first += static_cast<double>(cc.pcrel); e.second += static_cast<double>(cc.abs + cc.got); }
      for (auto& [k, e] : agg) G.edges.push_back({k.first, k.second, e.first, e.second});
      G.finalize();
      std::vector<int> part;
      if (opt.partition == "mincut") part = domains::mincut_partition(G, W, 1.05, opt.seed);
      else if (opt.partition == "random") part = domains::random_partition(G, W, opt.seed);
      else part = domains::directory_partition(G, W, 1);
      std::vector<int> live_nodes; for (size_t i = 0; i < conns.size(); ++i) if (conns[i] && conns[i]->connected()) live_nodes.push_back(static_cast<int>(i) + 1);
      for (size_t i = 0; i < all.size(); ++i) dom_of[i] = live_nodes[static_cast<size_t>(part[i]) % live_nodes.size()];
      cut = domains::evaluate(G, part, W).json();
    }
    // domains numbered by first appearance in link order; the node a domain lives on = dom_of (node id)
    std::vector<Domain> doms; std::map<int, int> node_to_dom;
    for (size_t i = 0; i < all.size(); ++i) {
      int node = dom_of[i]; auto it = node_to_dom.find(node);
      if (it == node_to_dom.end()) { Domain d; d.id = static_cast<int>(doms.size()); d.node = node; doms.push_back(d); it = node_to_dom.emplace(node, d.id).first; }
      doms[static_cast<size_t>(it->second)].objs.push_back(static_cast<int>(i));
    }
    rep["domains_formed"] = doms.size(); rep["objects"] = all.size(); rep["cut"] = cut; rep["relocatable"] = relocatable; rep["gc_effective"] = gc;
    elf::Roots roots = roots_for(argv, opt); json roots_j = elf::roots_json(roots);
    json options{{"gc", gc}, {"apply_pcrel", opt.apply_pcrel}, {"static_link", nonpreemptible}};
    // ---- helpers: one remote request with NEED/BLOBS service ----------------------------------------------------
    auto remote = [&](Domain& d, const std::string& phase, const json& live, proto::PrelinkReply* reply) {
      auto* conn = conns[static_cast<size_t>(d.node - 1)];
      proto::PrelinkRequest req; req.id = next_id(); req.domain = d.id; req.phase = phase; req.roots = roots_j; req.options = options; req.live = live;
      for (int i : d.objs) req.objects.push_back(proto::FileRef{all[static_cast<size_t>(i)].path, all[static_cast<size_t>(i)].sha, all[static_cast<size_t>(i)].size});
      uint64_t sent = 0; auto s = conn->request_session(proto::encode_prelink(req), req.id, &sent);
      proto::Frame ev;
      while (s->wait_event(&ev)) {
        if (ev.type != proto::Type::Need) continue;
        proto::NeedMsg need = proto::decode_need(ev); proto::BlobsMsg b; b.id = req.id;
        for (auto& sha : need.shas) for (auto& o : req.objects) if (o.sha == sha) { b.blobs.emplace_back(sha, util::read_file(o.path)); d.shipped_bytes += o.size; d.shipped_objects++; break; }
        conn->send_blobs(b, opt.zstd_level, &sent);
      }
      if (!s->error.empty()) throw std::runtime_error("domain " + std::to_string(d.id) + ": " + s->error);
      *reply = proto::decode_prelink_reply(s->raw);
      if (reply->status != "ok") throw std::runtime_error("domain " + std::to_string(d.id) + " " + phase + ": " + reply->error);
    };
    auto local_set = [&](const Domain& d) { std::vector<std::string> ps; for (int i : d.objs) ps.push_back(all[static_cast<size_t>(i)].path); return elf::ObjectSet::load(ps); };
    // ---- 4. phase 1: manifests ---------------------------------------------------------------------------------
    std::vector<std::thread> th; std::atomic<bool> failed{false}; std::mutex emu; std::string first_error;
    auto manifest_of = [&](Domain& d) {
      try {
        d.t_manifest_sent = T();
        if (d.node == 0) { d.manifest = elf::condense_domain(local_set(d), roots, d.id); }
        else { proto::PrelinkReply r; remote(d, "manifest", json::array(), &r); d.manifest = elf::domain_manifest_from_wire(r.manifest); d.manifest_bytes = r.manifest.dump().size(); }
        d.manifest.domain = d.id; d.have_manifest = true; d.t_manifest_recv = T();
      } catch (const std::exception& e) { std::lock_guard<std::mutex> g(emu); failed = true; if (first_error.empty()) first_error = e.what(); d.error = e.what(); }
    };
    if (opt.pipeline == "sequential") { for (auto& d : doms) manifest_of(d); }
    else { for (auto& d : doms) th.emplace_back(manifest_of, std::ref(d)); for (auto& t : th) t.join(); th.clear(); }
    if (failed) throw std::runtime_error(first_error);
    // ---- 5. resolution from manifests only + liveness fixpoint ------------------------------------------------
    double t_res0 = T();
    std::map<std::string, int> export_count; std::set<std::string> imported;
    for (auto& d : doms) { for (auto& [s, r] : d.manifest.exports) export_count[s]++; for (auto& [x, syms] : d.manifest.imports) for (auto& s : syms) imported.insert(s); }
    json undefined = json::array(), duplicates = json::array();
    auto linker_defined = [](const std::string& s) { return s.rfind("__start_", 0) == 0 || s.rfind("__stop_", 0) == 0 || s == "_GLOBAL_OFFSET_TABLE_" || s == "_DYNAMIC" || s == "__ehdr_start" || s == "__executable_start" || s == "_end" || s == "end" || s == "_etext" || s == "etext" || s == "_edata" || s == "edata" || s == "__bss_start" || s == "__init_array_start" || s == "__init_array_end" || s == "__fini_array_start" || s == "__fini_array_end" || s == "__dso_handle"; };
    for (auto& s : imported) if (!export_count.count(s) && !linker_defined(s)) undefined.push_back(s);
    for (auto& [s, n] : export_count) if (n > 1) duplicates.push_back(s);
    std::vector<elf::DomainManifest> ms; for (auto& d : doms) ms.push_back(d.manifest);
    std::map<int, std::set<elf::SecRef>> live_entries;
    if (gc) live_entries = elf::resolve_condensed(ms);
    double t_res1 = T();
    rep["resolve_s"] = t_res1 - t_res0; rep["undefined"] = undefined; rep["duplicates"] = duplicates;
    rep["errors_detected_before_bytes"] = (!undefined.empty() || !duplicates.empty());
    rep["t_manifests_done"] = t_res0;
    // ---- 6. phase 2: prelink -----------------------------------------------------------------------------------
    auto prelink_of = [&](Domain& d) {
      try {
        d.t_prelink_sent = T();
        json live = gc ? elf::secrefs_json(live_entries[d.id]) : json::array();
        if (d.node == 0) {
          elf::ObjectSet set = local_set(d); elf::PrelinkOptions po; po.gc = gc; po.apply_pcrel = opt.apply_pcrel; po.static_link = nonpreemptible;
          for (auto& e : roots.entry_symbols) po.entry_symbols.insert(e); po.origin_tag = "domain-" + std::to_string(d.id);
          std::set<elf::SecRef> lv; if (gc) lv = elf::expand_domain(set, roots, live_entries[d.id]).live;
          elf::PrelinkResult r = elf::prelink(set, gc ? &lv : nullptr, po);
          auto bytes = elf::write_object(r.out); d.prelinked.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
          d.pre_manifest = r.manifest; d.stats = r.stats.json();
        } else { proto::PrelinkReply r; remote(d, "prelink", live, &r); d.prelinked = r.object; d.pre_manifest = r.manifest; d.stats = r.stats; }
        d.t_prelink_recv = T();
      } catch (const std::exception& e) { std::lock_guard<std::mutex> g(emu); failed = true; if (first_error.empty()) first_error = e.what(); d.error = e.what(); }
    };
    if (opt.pipeline == "sequential") { for (auto& d : doms) prelink_of(d); }
    else { for (auto& d : doms) th.emplace_back(prelink_of, std::ref(d)); for (auto& t : th) t.join(); th.clear(); }
    if (failed) throw std::runtime_error(first_error);
    // ---- 7. write domain objects and rewrite the command ------------------------------------------------------
    std::string dir = opt.tmp_dir.empty() ? util::make_temp_dir("opticc-link-") : opt.tmp_dir; util::mkdir_p(dir);   // under $TMPDIR
    std::vector<std::string> dom_paths; uint64_t bytes_in = 0, bytes_out = 0;
    for (auto& d : doms) { std::string p = dir + "/domain" + std::to_string(d.id) + ".o"; util::write_file(p, d.prelinked); dom_paths.push_back(p); bytes_out += d.prelinked.size(); for (int i : d.objs) bytes_in += all[static_cast<size_t>(i)].size; }
    std::vector<std::string> nargv; std::set<size_t> drop; for (auto& oa : oargs) drop.insert(oa.idx);
    size_t first = oargs.front().idx;
    for (size_t i = 0; i < argv.size(); ++i) {
      if (i == first) { for (auto& p : dom_paths) nargv.push_back(p); continue; }
      if (drop.count(i)) continue;
      nargv.push_back(argv[i]);
    }
    // ---- 8. report ---------------------------------------------------------------------------------------------
    json dj = json::array(); uint64_t residual = 0, applied = 0, input_relocs = 0;
    for (auto& d : doms) {
      json o{{"id", d.id}, {"node", d.node}, {"objects", d.objs.size()}, {"manifest_bytes", d.manifest_bytes}, {"prelinked_bytes", d.prelinked.size()},
             {"shipped_objects", d.shipped_objects}, {"shipped_bytes", d.shipped_bytes}, {"t_manifest_sent", d.t_manifest_sent}, {"t_manifest_recv", d.t_manifest_recv},
             {"t_prelink_sent", d.t_prelink_sent}, {"t_prelink_recv", d.t_prelink_recv}, {"stats", d.stats},
             {"order", json::array()}};
      for (int i : d.objs) o["order"].push_back(all[static_cast<size_t>(i)].path);
      if (d.stats.contains("residual") && d.stats["residual"].contains("total")) residual += d.stats["residual"]["total"].get<uint64_t>();
      if (d.stats.contains("applied_pcrel")) applied += d.stats["applied_pcrel"].get<uint64_t>();
      if (d.stats.contains("input") && d.stats["input"].contains("total")) input_relocs += d.stats["input"]["total"].get<uint64_t>();
      dj.push_back(o);
    }
    rep["domains"] = dj; rep["bytes_in"] = bytes_in; rep["bytes_out"] = bytes_out; rep["relocs_in"] = input_relocs; rep["relocs_applied"] = applied; rep["relocs_residual"] = residual;
    rep["t_prelinked_done"] = T(); rep["domain_dir"] = dir; rep["argv"] = nargv;
    out.used_domains = true; out.argv = nargv; out.report = rep;
  } catch (const std::exception& e) {
    out.used_domains = false; out.argv = argv; out.error = e.what(); rep["fallback"] = e.what(); out.report = rep;
    log({{"event", "link-fallback"}, {"error", e.what()}});
  }
  if (!opt.report_path.empty()) { try { util::write_file(opt.report_path, out.report.dump(1)); } catch (...) {} }
  return out;
}

}  // namespace opticc::coord
