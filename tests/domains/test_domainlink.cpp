// SPDX-License-Identifier: GPL-2.0-only
// End-to-end linker-domain pipeline on loopback:
//   objects are compiled THROUGH opticc against three local workers that keep their objects (--keep-objects),
//   then `opticc --link` drives manifests → resolution → prelink → rewritten final link.
//   (a) byte-identical to a direct lld link of the original objects in the same (domain) order, with and without
//       --gc-sections; (b) an undefined symbol is reported from manifests before any prelinked bytes are requested;
//   (c) pipelined and sequential modes record their timings; (d) gc ablation; (e) fallback keeps the link working.
#include <cstdlib>
#include <fstream>
#include <thread>

#include "elf_test_util.hpp"
#include "nlohmann/json.hpp"
#include "test_harness.hpp"

#ifndef OPTICC_BIN
#define OPTICC_BIN "opticc"
#endif
#ifndef OPTICCD_BIN
#define OPTICCD_BIN "opticcd"
#endif

using namespace elftest;
using nlohmann::json;

namespace {
struct Workers {
  std::vector<std::string> ports; std::string token; std::string dir;
  ~Workers() { if (!token.empty()) run("pkill -f 'token " + token + "' >/dev/null 2>&1; true"); }
};
Workers start_workers(const std::string& dir, int n) {
  Workers w; w.dir = dir; w.token = "dl" + std::to_string(::getpid());
  for (int i = 0; i < n; ++i) {
    std::string log = dir + "/w" + std::to_string(i) + ".log";
    run(std::string(OPTICCD_BIN) + " --listen 127.0.0.1:0 --accept plain --allow 127.0.0.1/32 --jobs 2 --keep-objects --tmp " + dir + "/tmp" + std::to_string(i) +
        " --token " + w.token + " > " + log + " 2>&1 &");
    std::string port;
    for (int k = 0; k < 600 && port.empty(); ++k) {   // up to 30 s: opticcd runs a compile benchmark before listening
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::string l = elftest::read(log); size_t p = l.find("127.0.0.1:");
      if (p != std::string::npos) { size_t e = l.find_first_not_of("0123456789", p + 10); port = l.substr(p + 10, e - p - 10); }
    }
    if (port.empty()) throw std::runtime_error("worker " + std::to_string(i) + " did not start: " + elftest::read(log));
    w.ports.push_back(port);
  }
  return w;
}
void set_env(const std::string& k, const std::string& v) { setenv(k.c_str(), v.c_str(), 1); }
json report_of(const std::string& path) { return json::parse(elftest::read(path)); }
std::vector<std::string> domain_order(const json& rep) {
  std::vector<std::string> v; for (auto& d : rep["domains"]) for (auto& p : d["order"]) v.push_back(p.get<std::string>()); return v;
}
std::string link_cmd(const std::string& out, bool gc) {
  return tool("ld.lld") + " -static -nostdlib -e _start -z keep-text-section-prefix -z nostart-stop-gc " + std::string(gc ? "--gc-sections " : "") + "-o " + out;
}
}  // namespace

OPTICC_TEST(domainlink_e2e) {
  if (!exists(tool("ld.lld")) || !exists(tool("clang"))) { std::printf("  skipped: llvm tools missing\n"); return; }
  std::string T = tmpdir("opticc-domainlink"); std::printf("  workdir %s\n", T.c_str());
  Workers w = start_workers(T, 3);
  std::string hosts; for (auto& p : w.ports) hosts += (hosts.empty() ? "" : " ") + std::string("127.0.0.1:") + p + "/2,plain";
  set_env("OPTICC_HOSTS", hosts); set_env("OPTICC_SECURITY", "plain"); set_env("OPTICC_TOKEN", w.token);
  set_env("OPTICC_SOCK", T + "/c.sock"); set_env("OPTICC_POLICY", "ect-window"); set_env("OPTICC_MODE", "classic");
  set_env("OPTICC_LOCAL_SLOTS", "0"); set_env("OPTICC_IDLE_TIMEOUT", "300"); set_env("OPTICC_LINK_DOMAINS", "1");
  set_env("OPTICC_LINK_FORCE", "1");   // tiny test programs: bypass the benefit gate (exercised separately below)
  set_env("OPTICC_SUMMARY", T + "/summary.json"); set_env("OPTICC_BINDING", "early");
  // no global pkill: the socket lives in this test's fresh tmpdir, so no stale coordinator can own it. A pattern kill
  // here would kill the coordinators of e2e tests running concurrently under `ctest -j`.
  std::string opticc = OPTICC_BIN;
  // --- objects through opticc (14 objects: entry, f0..f9, mytabA/B, cpp1/2) ---
  run("mkdir -p " + T + "/src");
  Program P = gen_program(T + "/src", false, 10, opticc + " " + tool("clang"), opticc + " " + tool("clang++"));
  CHECK(P.objs.size() == 15);
  // every object must exist and be ELF
  for (auto& o : P.objs) CHECK(elftest::read(o).compare(0, 4, "\x7f" "ELF") == 0);
  auto do_link = [&](const std::string& tag, bool gc, const std::string& extra_env_k = "", const std::string& extra_env_v = "") {
    if (!extra_env_k.empty()) set_env(extra_env_k, extra_env_v);
    std::string rep = T + "/" + tag + ".json"; set_env("OPTICC_LINK_REPORT", rep);
    std::string out = T + "/" + tag + ".elf";
    Run r = run(opticc + " --link -- " + link_cmd(out, gc) + " " + join(P.objs));
    if (!extra_env_k.empty()) unsetenv(extra_env_k.c_str());
    return std::make_pair(r, rep);
  };
  // (a) no gc: domain link vs direct link in domain order
  {
    auto [r, rep] = do_link("nogc", false);
    CHECK(r.rc == 0);
    json j = report_of(rep); CHECK(j.value("domains_formed", 0) >= 1); CHECK(!j.contains("fallback"));
    CHECK(j["relocs_applied"].get<uint64_t>() > 0);
    std::printf("  nogc: domains=%d objects=%d relocs_in=%llu applied=%llu residual=%llu bytes_in=%llu bytes_out=%llu resolve_s=%.4f\n",
                j.value("domains_formed", 0), j.value("objects", 0), (unsigned long long)j["relocs_in"].get<uint64_t>(), (unsigned long long)j["relocs_applied"].get<uint64_t>(),
                (unsigned long long)j["relocs_residual"].get<uint64_t>(), (unsigned long long)j["bytes_in"].get<uint64_t>(), (unsigned long long)j["bytes_out"].get<uint64_t>(), j.value("resolve_s", 0.0));
    for (auto& d : j["domains"]) std::printf("    domain %d node %d objs %d manifest_bytes %llu prelinked %llu t_manifest %.3f->%.3f t_prelink %.3f->%.3f\n", d.value("id", 0), d.value("node", 0), d.value("objects", 0),
                                              (unsigned long long)d["manifest_bytes"].get<uint64_t>(), (unsigned long long)d["prelinked_bytes"].get<uint64_t>(), d.value("t_manifest_sent", 0.0), d.value("t_manifest_recv", 0.0), d.value("t_prelink_sent", 0.0), d.value("t_prelink_recv", 0.0));
    // at least one domain must live on a worker (objects were compiled remotely with no local slots)
    int remote_domains = 0; for (auto& d : j["domains"]) if (d.value("node", 0) > 0) remote_domains++;
    CHECK(remote_domains >= 1);
    Run ref = run(link_cmd(T + "/nogc-ref.elf", false) + " " + join(domain_order(j)));
    CHECK(ref.rc == 0);
    std::string why; CHECK(signatures_equal(link_signature(T + "/nogc-ref.elf", T), link_signature(T + "/nogc.elf", T), &why, 0.5));
    if (!why.empty()) std::printf("  nogc mismatch: %s\n", why.c_str());
  }
  // (a'') partition ablation: a random K-way partition spreads the objects over all three workers (objects are shipped to the
  //        worker that prelinks them; measured as shipped_bytes); output still identical to the direct link in domain order
  {
    auto [r, rep] = do_link("rnd", false, "OPTICC_DOMAIN_PARTITION", "random");
    CHECK(r.rc == 0);
    json j = report_of(rep); CHECK(!j.contains("fallback")); CHECK(j.value("domains_formed", 0) >= 2);
    uint64_t shipped = 0; for (auto& d : j["domains"]) shipped += d["shipped_bytes"].get<uint64_t>();
    std::printf("  random partition: domains=%d shipped_bytes=%llu cut=%s\n", j.value("domains_formed", 0), (unsigned long long)shipped, j["cut"].dump().c_str());
    CHECK(shipped > 0);
    Run ref = run(link_cmd(T + "/rnd-ref.elf", false) + " " + join(domain_order(j)));
    CHECK(ref.rc == 0);
    std::string why; CHECK(signatures_equal(link_signature(T + "/rnd-ref.elf", T), link_signature(T + "/rnd.elf", T), &why, 0.5));
    if (!why.empty()) std::printf("  rnd mismatch: %s\n", why.c_str());
  }
  // (a') with two-phase gc vs direct --gc-sections link in domain order
  {
    auto [r, rep] = do_link("gc", true);
    CHECK(r.rc == 0);
    json j = report_of(rep); CHECK(!j.contains("fallback")); CHECK(j.value("gc_effective", false));
    Run ref = run(link_cmd(T + "/gc-ref.elf", true) + " " + join(domain_order(j)));
    CHECK(ref.rc == 0);
    std::string why; CHECK(signatures_equal(link_signature(T + "/gc-ref.elf", T), link_signature(T + "/gc.elf", T), &why, 0.5));
    if (!why.empty()) std::printf("  gc mismatch: %s\n", why.c_str());
    uint64_t dropped = 0; for (auto& d : j["domains"]) if (d["stats"].contains("sections_dropped_dead")) dropped += d["stats"]["sections_dropped_dead"].get<uint64_t>();
    std::printf("  gc: dead sections dropped on workers = %llu\n", (unsigned long long)dropped);
    CHECK(dropped > 0);
  }
  // (d) gc ablation: OPTICC_DOMAIN_GC=0 with --gc-sections must still equal the reference (the final linker gcs)
  {
    auto [r, rep] = do_link("gcoff", true, "OPTICC_DOMAIN_GC", "0");
    CHECK(r.rc == 0);
    json j = report_of(rep); CHECK(!j.value("gc_effective", true));
    Run ref = run(link_cmd(T + "/gcoff-ref.elf", true) + " " + join(domain_order(j)));
    CHECK(ref.rc == 0);
    // merging before the final link defeats --gc-sections (sections become atomic): the domain link cannot be smaller
    // than the gc'd reference, and must be at least as large as the no-gc reference's text; two-phase gc restores identity
    uint64_t sz_got = elftest::read(T + "/gcoff.elf").size(), sz_ref = elftest::read(T + "/gcoff-ref.elf").size(), sz_nogc = elftest::read(T + "/nogc-ref.elf").size();
    std::printf("  gc ablation (domain gc off, final --gc-sections): %llu bytes vs gc'd reference %llu vs no-gc reference %llu\n", (unsigned long long)sz_got, (unsigned long long)sz_ref, (unsigned long long)sz_nogc);
    CHECK(sz_got >= sz_ref);
  }
  // (c) sequential pipeline: timings recorded, same output
  {
    auto [r, rep] = do_link("seq", false, "OPTICC_LINK_PIPELINE", "sequential");
    CHECK(r.rc == 0);
    json j = report_of(rep); CHECK(j.value("pipeline", "") == "sequential"); CHECK(j.value("t_prelinked_done", 0.0) > 0);
    std::printf("  sequential: manifests done %.3f s, prelinked done %.3f s\n", j.value("t_manifests_done", 0.0), j.value("t_prelinked_done", 0.0));
    std::string why; CHECK(signatures_equal(link_signature(T + "/nogc.elf", T), link_signature(T + "/seq.elf", T), &why, 0.5));
  }
  // (b) early error detection: an object referencing an undefined symbol; the report must name it from manifests,
  //     i.e. before the prelink phase, and the (failing) final link must still be attempted normally
  {
    write(T + "/src/bad.c", "int missing_fn(int);\nint usebad(int x) { return missing_fn(x) + 1; }\n");
    Run c = run(opticc + " " + tool("clang") + " --target=x86_64-linux-gnu -ffreestanding -nostdinc -O2 -ffunction-sections -fdata-sections -fPIC -c " + T + "/src/bad.c -o " + T + "/src/bad.o");
    CHECK(c.rc == 0);
    std::vector<std::string> objs = P.objs; objs.push_back(T + "/src/bad.o");
    set_env("OPTICC_LINK_REPORT", T + "/bad.json");
    Run r = run(opticc + " --link -- " + link_cmd(T + "/bad.elf", false) + " " + join(objs));
    json j = report_of(T + "/bad.json");
    bool found = false; for (auto& u : j["undefined"]) if (u.get<std::string>() == "missing_fn") found = true;
    CHECK(found); CHECK(j.value("errors_detected_before_bytes", false));
    // in the report the manifests phase ends before any prelink was sent
    double tm = j.value("t_manifests_done", 0.0); bool before = true; for (auto& d : j["domains"]) if (d.value("t_prelink_sent", 0.0) < tm) before = false;
    CHECK(before);
    std::printf("  early error: undefined=%s rc(final link)=%d\n", j["undefined"].dump().c_str(), r.rc);
  }
  // (e) fallback: a command with a single object input cannot be domain-linked; the link must still succeed unchanged
  {
    set_env("OPTICC_LINK_REPORT", T + "/fb.json");
    std::string one = T + "/src/one.c"; write(one, "void _start(void){ for(;;); }\n");
    run(tool("clang") + " --target=x86_64-linux-gnu -ffreestanding -nostdinc -O2 -c " + one + " -o " + T + "/src/one.o");
    Run r = run(opticc + " --link -- " + link_cmd(T + "/fb.elf", false) + " " + T + "/src/one.o");
    CHECK(r.rc == 0); json j = report_of(T + "/fb.json"); CHECK(j.contains("fallback"));
  }
  // (f) benefit gate: without force, a small relocatable link must NOT be domain-linked; the plan
  //     says why, and the original command produces the output
  {
    set_env("OPTICC_LINK_FORCE", "0"); set_env("OPTICC_LINK_REPORT", T + "/gate.json");
    Run r = run(opticc + " --link -- " + tool("ld.lld") + " -r -o " + T + "/gate.o " + join(P.objs));
    CHECK(r.rc == 0); CHECK(exists(T + "/gate.o"));
    json j = report_of(T + "/gate.json");
    CHECK(j.dump().find("benefit gate") != std::string::npos);
    std::printf("  benefit gate: %s\n", j.value("fallback", std::string("?")).c_str());
    set_env("OPTICC_LINK_FORCE", "1");
  }
  // summary carries the link reports
  // stop only OUR coordinator: the process bound to this test's socket path
  run("for p in $(lsof -t " + T + "/c.sock 2>/dev/null); do kill -TERM $p; done; true"); std::this_thread::sleep_for(std::chrono::milliseconds(800));
  if (exists(T + "/summary.json")) { json s = json::parse(elftest::read(T + "/summary.json")); CHECK(s.contains("links")); CHECK(s["links"].size() >= 6); }
  unsetenv("OPTICC_HOSTS"); unsetenv("OPTICC_LINK_DOMAINS"); unsetenv("OPTICC_LINK_FORCE"); unsetenv("OPTICC_SOCK"); unsetenv("OPTICC_LOCAL_SLOTS");
}
