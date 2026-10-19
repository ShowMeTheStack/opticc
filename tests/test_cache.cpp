// Compile cache: keys, worker object cache, protocol, index, affinity.
// SPDX-License-Identifier: GPL-2.0-only
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache_index.hpp"
#include "objcache.hpp"
#include "opticc/cachekey.hpp"
#include "opticc/protocol.hpp"
#include "opticc/sched.hpp"
#include "opticc/sim.hpp"
#include "opticc/util.hpp"
#include "test_harness.hpp"

using namespace opticc;

namespace {
std::string tmpdir(const char* tag) { return util::make_temp_dir(tag); }
Topology two_nodes(double f1, double f2) {
  Topology t; t.client = Client{1, 0.05, 0, 10e9, 10e9, 0};   // a slow client: local is never best
  t.params.alpha = 1.0; t.params.rho = 0.2;
  for (double f : {f1, f2}) { Node n; n.name = "w"; n.cores = 4; n.speed = f; n.rtt_s = 0.001; n.bw_up_bps = 1e9; n.bw_down_bps = 1e9; t.nodes.push_back(n); }
  t.assign_ids();
  return t;
}
Workload jobs(int n, double w) {
  Workload wl;
  for (int i = 0; i < n; ++i) { Job j; j.id = i; j.name = "tu" + std::to_string(i); j.pre_size_bytes = 1e6; j.src_size_bytes = 2e4;
    j.work_ref_cpu_s = w; j.preprocess_cpu_s = 0.0; j.obj_size_bytes = 3e4; j.mem_bytes = 3e8; wl.jobs.push_back(j); }
  return wl;
}
}  // namespace

// Key stability and sensitivity: same inputs -> same key regardless of output names / dep flags / tree location when
// every path is spelled relative; any flag, source or header content change -> different key.
OPTICC_TEST(cachekey_stability_and_sensitivity) {
  std::vector<std::string> a1 = {"/usr/bin/cc", "-O2", "-Iinc", "-c", "f.c", "-o", "/home/u/proj/f.o", "-MMD", "-MF", "/home/u/proj/.f.o.d"};
  std::vector<std::string> a2 = {"/usr/bin/cc", "-O2", "-Iinc", "-c", "f.c", "-o", "/tmp/zz.o", "-Wp,-MMD,.f.d"};
  auto n1 = cachekey::normalise_argv(a1, 4, "/home/u/proj"), n2 = cachekey::normalise_argv(a2, 4, "/srv/x/proj");
  CHECK(n1 == n2);   // relocated tree, different -o / dep flags
  {   // __FILE__ expands to the path AS SPELLED, debug info records cwd
    std::vector<std::string> abs1 = {"/usr/bin/cc", "-O2", "-I/home/u/proj/inc", "-c", "/home/u/proj/f.c", "-o", "f.o"};
    std::vector<std::string> abs2 = {"/usr/bin/cc", "-O2", "-I/srv/x/proj/inc", "-c", "/srv/x/proj/f.c", "-o", "f.o"};
    CHECK(cachekey::normalise_argv(abs1, 4, "/home/u/proj") != cachekey::normalise_argv(abs2, 4, "/srv/x/proj"));   // absolute: the tree differs
    std::vector<std::string> rel1 = {"/usr/bin/cc", "-O2", "-Iinc", "-c", "f.c", "-o", "f.o"};
    CHECK(cachekey::normalise_argv(abs1, 4, "/home/u/proj") != cachekey::normalise_argv(rel1, 4, "/home/u/proj"));   // absolute vs relative spelling
    auto absI = rel1; absI[2] = "-I/home/u/proj/inc";
    CHECK(cachekey::normalise_argv(absI, 4, "/home/u/proj") != cachekey::normalise_argv(rel1, 4, "/home/u/proj"));   // -I spelling alone
    auto g1 = rel1; g1.insert(g1.begin() + 1, "-g");
    CHECK(cachekey::normalise_argv(g1, 5, "/home/u/proj") != cachekey::normalise_argv(g1, 5, "/srv/x/proj"));       // -g: cwd is keyed
    auto g0 = g1; g0.insert(g0.begin() + 2, "-g0");
    CHECK(cachekey::normalise_argv(g0, 6, "/home/u/proj") == cachekey::normalise_argv(g0, 6, "/srv/x/proj"));       // -g0 after -g: not
    CHECK(cachekey::normalise_argv(rel1, 4, "/home/u/proj") == cachekey::normalise_argv(rel1, 4, "/srv/x/proj"));   // no -g: shared
  }
  std::string cid = "cid";
  std::string m1 = cachekey::manifest_key(cid, n1, "srcsha"), m2 = cachekey::manifest_key(cid, n2, "srcsha");
  CHECK(m1 == m2);
  auto a3 = a1; a3[1] = "-O3";
  CHECK(cachekey::manifest_key(cid, cachekey::normalise_argv(a3, 4, "/home/u/proj"), "srcsha") != m1);   // flag change
  CHECK(cachekey::manifest_key(cid, n1, "srcsha2") != m1);                                               // source change
  CHECK(cachekey::manifest_key("cid2", n1, "srcsha") != m1);                                             // compiler change
  cachekey::Closure c1 = {{"/home/u/proj/inc/a.h", "h1"}, {"/home/u/proj/inc/b.h", "h2"}};
  cachekey::Closure c2 = {{"/srv/x/proj/inc/b.h", "h2"}, {"/srv/x/proj/inc/a.h", "h1"}};   // moved tree, other order
  CHECK(cachekey::object_key(m1, c1, "/home/u/proj") == cachekey::object_key(m1, c2, "/srv/x/proj"));
  cachekey::Closure c3 = c1; c3[1].second = "h2x";                                                       // header change
  CHECK(cachekey::object_key(m1, c3, "/home/u/proj") != cachekey::object_key(m1, c1, "/home/u/proj"));
  CHECK(cachekey::classic_key(cid, n1, "pre") != cachekey::classic_key(cid, n1, "pre2"));
  CHECK(!cachekey::compiler_id("cc").empty() && cachekey::compiler_id("cc") == cachekey::compiler_id("cc"));
}

// Worker object cache: store/lookup, used-closure matching, header-change invalidation, shadowing header (shape) miss.
OPTICC_TEST(objcache_manifest_matching) {
  std::string d = tmpdir("opticc-objc-");
  objcache::ObjectCache oc; oc.init(d, 1 << 20);
  std::unordered_map<std::string, std::string> view = {{"f.c", "s"}, {"inc/a.h", "A"}, {"inc/b.h", "B"}};   // b.h shipped, unused
  cachekey::Closure used = {{"inc/a.h", "A"}, {"f.c", "s"}};
  std::string O = cachekey::object_key("M", used, "");
  oc.store(O, "OBJ", " f.c inc/a.h\n", "", "", 0); oc.add_manifest_entry("M", used, O, objcache::ObjectCache::shape_hash(view));
  objcache::Hit h;
  CHECK(oc.lookup_manifest("M", view, &h) && h.object == "OBJ" && h.depfile_body == " f.c inc/a.h\n");
  auto v2 = view; v2["inc/b.h"] = "B2";        // unused header edited: still a hit
  CHECK(oc.lookup_manifest("M", v2, &h));
  auto v3 = view; v3["inc/a.h"] = "A2";        // used header edited: miss
  CHECK(!oc.lookup_manifest("M", v3, &h));
  auto v4 = view; v4["inc2/a.h"] = "X";        // a header now shadows a.h from an earlier -I dir: shape differs -> miss
  CHECK(!oc.lookup_manifest("M", v4, &h));
  CHECK(!oc.lookup_manifest("M-other", view, &h));
  // persistence: a fresh instance on the same directory sees the entry
  objcache::ObjectCache oc2; oc2.init(d, 1 << 20);
  CHECK(oc2.lookup_manifest("M", view, &h) && h.object == "OBJ");
  util::remove_tree(d);
}

// LRU eviction by last use, bounded by the byte cap.
OPTICC_TEST(objcache_lru_eviction) {
  std::string d = tmpdir("opticc-objl-");
  objcache::ObjectCache oc; oc.init(d, 2500);
  std::string kb(1000, 'x');
  oc.store("o1", kb, "", "", "", 0); oc.store("o2", kb, "", "", "", 0);
  objcache::Hit h; CHECK(oc.lookup_object("o1", &h));   // o1 now more recent than o2
  oc.store("o3", kb, "", "", "", 0);                    // 3000 > 2500: evict the least recently used (o2)
  CHECK(oc.has_object("o1") && !oc.has_object("o2") && oc.has_object("o3"));
  CHECK(oc.total_bytes() <= 2500);
  CHECK(!std::filesystem::exists(d + "/o/o2"));
  util::remove_tree(d);
}

// Every torn store state is a miss, never a hit with rc 0, an empty .d and no output. The states a crash, a power
// loss or a full disk can leave: meta without the object, object without its meta (an older store order), a short
// object (data not on disk), an unparsable meta, an older meta without the size; and a reopened cache
// (init indexes the object file) must apply the same checks. The intact entry and a store after repair still hit.
OPTICC_TEST(objcache_torn_store_is_a_miss) {
  namespace fs = std::filesystem;
  std::string d = tmpdir("opticc-objt-");
  const std::string obj(4096, 'o');
  {
    objcache::ObjectCache oc; oc.init(d, 1 << 30);
    for (const char* k : {"ok", "nometa", "noobj", "short", "badmeta", "oldmeta", "flipped"}) CHECK(oc.store(k, obj, " a.h", "", "", 0));
    objcache::Hit h; CHECK(oc.lookup_object("ok", &h) && h.object == obj && h.depfile_body == " a.h");
  }
  fs::remove(d + "/o/nometa.meta");
  fs::remove(d + "/o/noobj");
  util::write_file(d + "/o/short", obj.substr(0, 1000));
  { std::string f = obj; f[2000] = 'X'; util::write_file(d + "/o/flipped", f); }   // same size, other bytes
  util::write_file(d + "/o/badmeta.meta", "{\"depfile_body\": \" a");
  util::write_file(d + "/o/oldmeta.meta", "{\"depfile_body\":\" a.h\",\"stdout\":\"\",\"stderr\":\"\",\"rc\":0}");
  util::write_file(d + "/o/stray.tmp.1.1", obj);   // a leftover temporary file is never indexed
  objcache::ObjectCache oc; oc.init(d, 1 << 30);
  objcache::Hit h;
  CHECK(oc.lookup_object("ok", &h) && h.object == obj);
  CHECK(util::xxh64_hex("") == "ef46db3751d8e999" && util::xxh64_hex("abc") == "44bc2cf5ad770999");   // reference vectors
  for (const char* k : {"nometa", "noobj", "short", "badmeta", "oldmeta", "flipped", "stray.tmp.1.1"}) {
    objcache::Hit m; CHECK(!oc.lookup_object(k, &m)); CHECK(!oc.has_object(k));
  }
  // the dropped entries' bytes left the total ("ok" alone remains of the 4096-byte entries indexed at
  // init: nometa/short/badmeta/oldmeta/flipped were indexed, noobj and the stray file were not)
  CHECK(oc.total_bytes() == obj.size());
  CHECK(oc.store("short", obj, " a.h", "", "", 0));   // a fresh store repairs the entry
  CHECK(oc.total_bytes() == 2 * obj.size());
  objcache::Hit r; CHECK(oc.lookup_object("short", &r) && r.object == obj);
  // the store leaves no temporary files behind
  for (auto& e : fs::directory_iterator(d + "/o")) CHECK(e.path().filename().string().find(".tmp.") == std::string::npos || e.path().filename() == "stray.tmp.1.1");
  util::remove_tree(d);
}

// Two PROCESSES storing one key into one cache directory (two coordinators sharing a cache, or a worker's CACHE_PUT
// racing its own compile) must never produce a torn entry. A temporary file named after the argument's address would
// be the same in two forked processes, so their writes would interleave in one file. A reader running during the race
// sees either a miss or an object consisting of one writer's bytes only.
OPTICC_TEST(objcache_two_process_writers_never_tear) {
  std::string d = tmpdir("opticc-objw-");
  { objcache::ObjectCache oc; oc.init(d, 1ull << 30); oc.store("K", std::string(10, 'z'), "", "", "", 0); }
  pid_t kids[2];
  for (int k = 0; k < 2; ++k) {
    kids[k] = fork();
    if (kids[k] == 0) {
      objcache::ObjectCache oc; oc.init(d, 1ull << 30);
      int failed = 0;   // with a shared temporary name one writer's rename takes the other's file: its store fails
      for (int i = 0; i < 3000; ++i) { std::string obj(k ? 4000 + i % 5 : 1000 + i % 5, k ? 'b' : 'a'); if (!oc.store("K", obj, "", "", "", 0)) ++failed; }
      _exit(failed ? 1 : 0);
    }
  }
  objcache::ObjectCache rd; rd.init(d, 1ull << 30);
  int torn = 0, hits = 0, alive = 2, failed_writers = 0;
  while (alive > 0) {
    objcache::Hit h;
    if (rd.lookup_object("K", &h)) {
      ++hits;
      if (h.object.empty() || h.object.find_first_not_of(h.object[0]) != std::string::npos) ++torn;
    } else rd.init(d, 1ull << 30);   // a miss drops the key from the index: re-index
    for (int k = 0; k < 2; ++k) { int st = 0; if (kids[k] > 0 && waitpid(kids[k], &st, WNOHANG) == kids[k]) { kids[k] = -1; --alive; if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) ++failed_writers; } }
  }
  objcache::ObjectCache fin; fin.init(d, 1ull << 30);
  objcache::Hit h;
  if (fin.lookup_object("K", &h)) { ++hits; if (h.object.find_first_not_of(h.object[0]) != std::string::npos) ++torn; }
  CHECK(torn == 0);
  CHECK(hits > 0);
  CHECK(failed_writers == 0);   // every store of both writers succeeded (no shared temporary file)
  for (auto& e : std::filesystem::directory_iterator(d + "/o")) CHECK(e.path().filename().string().find(".tmp.") == std::string::npos);
  util::remove_tree(d);
}

// A manifest is replaced atomically (a new file renamed over the old one), never truncated and
// rewritten in place, where a crash or a concurrent reader saw a partial JSON document.
OPTICC_TEST(objcache_manifest_replaced_atomically) {
  std::string d = tmpdir("opticc-objm-");
  objcache::ObjectCache oc; oc.init(d, 1ull << 30);
  std::unordered_map<std::string, std::string> view{{"f.c", "s1"}};
  oc.store("O1", "x", "", "", "", 0); oc.add_manifest_entry("M", {{"f.c", "s1"}}, "O1", objcache::ObjectCache::shape_hash(view));
  struct stat a{}; CHECK(::stat((d + "/m/M.json").c_str(), &a) == 0);
  oc.store("O2", "y", "", "", "", 0); oc.add_manifest_entry("M", {{"f.c", "s2"}}, "O2", objcache::ObjectCache::shape_hash(view));
  struct stat b{}; CHECK(::stat((d + "/m/M.json").c_str(), &b) == 0);
  CHECK(a.st_ino != b.st_ino);   // a new inode: renamed into place, not rewritten
  objcache::ObjectCache re; re.init(d, 1ull << 30); CHECK(re.has_manifest("M"));
  for (auto& e : std::filesystem::directory_iterator(d + "/m")) CHECK(e.path().filename().string().find(".tmp.") == std::string::npos);
  util::remove_tree(d);
}

// The comment-insensitive key keeps the presence of the final newline (-Wnewline-eof /
// -pedantic-errors change the exit status), and the time macros are recognised as identifiers only.
OPTICC_TEST(content_key_final_newline_and_time_macros) {
  const std::string a = "int f(void) { return 1; }\n", b = "int f(void) { return 1; }";
  CHECK(cachekey::content_key_sha(a) != cachekey::content_key_sha(b));
  CHECK(cachekey::content_key_sha(a) == cachekey::content_key_sha(a + "/* appended comment */\n"));   // the touch edit still shares
  CHECK(cachekey::content_key_sha(b + "\n") == cachekey::content_key_sha(a));
  // trailing whitespace on any physical line is kept as a marker (GCC 15 -Wtrailing-whitespace -Werror)
  CHECK(cachekey::content_key_sha("int x;  \nint y;\n") != cachekey::content_key_sha("int x;\nint y;\n"));
  CHECK(cachekey::content_key_sha("int x;\t\r\nint y;\r\n") != cachekey::content_key_sha("int x;\r\nint y;\r\n"));
  CHECK(cachekey::content_key_sha("int x; /* a */\n") == cachekey::content_key_sha("int x; /* abc */\n"));   // comment edit shares
  CHECK(cachekey::uses_time_macros("const char *d = __DATE__;\n"));
  CHECK(cachekey::uses_time_macros("#define STAMP __TIME__\n"));
  CHECK(cachekey::uses_time_macros("const char *t = __TIMESTAMP__;\n"));
  CHECK(cachekey::uses_time_macros("int x = 1'000; const char *d = __DATE__;\n"));          // digit separator is no literal
  CHECK(cachekey::uses_time_macros("/* unterminated __DATE__"));                             // errs towards yes
  CHECK(!cachekey::uses_time_macros("int f(void);\n"));
  CHECK(!cachekey::uses_time_macros("// built on __DATE__\n/* __TIME__ */\nint f(void);\n"));  // comments
  CHECK(!cachekey::uses_time_macros("const char *s = \"__DATE__\"; char c = '_';\n"));        // literals
  CHECK(!cachekey::uses_time_macros("int __DATE__x; int my__TIME__;\n"));                    // other identifiers
  // pasting from fragments of >= 4 characters
  CHECK(cachekey::uses_time_macros("#define CAT(a, b) a ## b\nconst char *s = CAT(__DA, TE__);\n"));
  CHECK(cachekey::uses_time_macros("const char *s = CAT(__TIME, __);\n"));
  CHECK(cachekey::uses_time_macros("const char *s = CAT(__TIMES, TAMP__);\n"));
  CHECK(cachekey::uses_time_macros("#define HALF __TIME\nint x = 1;\n"));   // a lone fragment (its other part elsewhere)
  CHECK(!cachekey::uses_time_macros("int __DAY; int TE_X; int __TIX__;\n"));                 // not fragments
  // 3- and 4-part pastes, and -D values
  CHECK(cachekey::uses_time_macros("#define S __ ## DATE ## __\n"));
  CHECK(cachekey::uses_time_macros("const char *s = CAT3(__, TIME, __);\n"));
  CHECK(cachekey::uses_time_macros("const char *s = CAT4(_, _TIME, STAMP_, _);\n"));
  CHECK(!cachekey::uses_time_macros("int a, b, c; unsigned long DATE; struct TIME t;\n"));
  CHECK(cachekey::argv_uses_time_macros({"cc", "-DSTAMP=__TIME__", "-c", "f.c"}));
  CHECK(cachekey::argv_uses_time_macros({"cc", "-D", "WHEN=__DATE__ \" \" __TIME__", "-c", "f.c"}));
  CHECK(!cachekey::argv_uses_time_macros({"cc", "-DNDEBUG", "-DV=3", "-D__DATE_IS_FINE", "-c", "f.c"}));
}

OPTICC_TEST(cache_query_roundtrip) {
  proto::CacheQuery q; q.keys = {"k1", "k2", "k3"};
  auto q2 = proto::decode_cache_query(proto::encode_cache_query(q));
  CHECK(q2.keys == q.keys);
  proto::CacheHave hv; hv.have = {true, false, true};
  auto f = proto::encode_cache_have(hv);
  CHECK(f.type == proto::Type::CacheHave);
  CHECK(proto::decode_cache_have(f).have == hv.have);
  proto::CompileResult r; r.id = 7; r.status = "ok"; r.cache = "hit"; r.cache_key = "M1"; r.object = "obj";
  auto r2 = proto::decode_result(proto::encode_result(r, 3));
  CHECK(r2.cache == "hit" && r2.cache_key == "M1" && r2.object == "obj");
}

OPTICC_TEST(cache_index_holders) {
  cacheidx::CacheIndex ix;
  ix.record_result("K", 2, "store"); ix.record_result("K", 1, "miss"); ix.record_result("K", 3, "hit");
  CHECK((ix.holders("K") == std::vector<int>{2, 3}));
  ix.record_have(1, {"K", "J"}, {true, false});
  CHECK((ix.holders("K") == std::vector<int>{1, 2, 3}) && ix.holders("J").empty());
  ix.record_have(2, {"K"}, {false});
  ix.forget_node(3);
  CHECK((ix.holders("K") == std::vector<int>{1}));
  ix.invalidate("K"); CHECK(ix.holders("K").empty() && ix.size() == 0);
  CHECK(ix.hits_seen() == 1);
}

// Affinity routing: a job whose object sits on the slower node goes there (hit, no compile), others go to
// the fast node; the simulator serves the hit without taking a core.
OPTICC_TEST(affinity_routes_to_holding_slower_node) {
  Topology t = two_nodes(0.25, 2.0);   // node 1 slow (4 s per job), node 2 fast (0.5 s)
  Workload wl = jobs(2, 1.0);
  {
    ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
    sim::Result r = sim::Simulator(t, wl, s).run();
    CHECK(r.job_node[0] == 2 && r.job_node[1] == 2);   // without a cache both go to the fast node
  }
  sim::Options o; o.cached_on = {1, -1};               // job 0's object is cached on the slow node
  ECTScheduler s(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r = sim::Simulator(t, wl, s, o).run();
  CHECK(r.job_node[0] == 1 && r.job_node[1] == 2);
  CHECK(r.cache_hits == 1);
  CHECK(r.job_done_time[0] < 0.1);                     // RTT + 4 KB up + 2 ms + 30 KB down, not 4 s of compile
  CHECK(r.nodes[1].busy_core_s == 0.0);
}

// Repeat build in the simulator: every job hits, makespan collapses to transfers, no worker compute.
OPTICC_TEST(sim_cache_repeat_all_hits) {
  Topology t = two_nodes(1.0, 1.0);
  Workload wl = jobs(40, 1.0);
  ECTScheduler s0(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r0 = sim::Simulator(t, wl, s0).run();
  sim::Options o; o.cached_on = r0.job_node;
  ECTScheduler s1(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r1 = sim::Simulator(t, wl, s1, o).run();
  int remote0 = 0; for (int k : r0.job_node) remote0 += k > 0;
  CHECK(r1.cache_hits == remote0 && remote0 > 30);
  CHECK(r1.makespan < 0.1 * r0.makespan);
}

// Comment-insensitive key hash. Comment-only edits that do not move any token hash equal; anything that
// moves a token (line or column) or changes code hashes differently; unmodelled constructs fall back to the raw hash.
OPTICC_TEST(content_key_comment_insensitive) {
  using cachekey::content_key_sha;
  const std::string base = "#ifndef X_H\n#define X_H\n/* licence\n   text */\nint a; // trailing\nint b = 2; /* inline */ int c;\n#endif\n";
  bool n = false;
  std::string h0 = content_key_sha(base, &n); CHECK(n); CHECK(h0.rfind("n:", 0) == 0);
  // appended comment line(s) at EOF (a touch edit of a header) -> same
  CHECK(content_key_sha(base + "/* opticc-touch 123456 */\n") == h0);
  CHECK(content_key_sha(base + "// x\n\n\n") == h0);
  // block comment text changed, same line structure -> same
  CHECK(content_key_sha("#ifndef X_H\n#define X_H\n/* LICENCE\n   other words here */\nint a; // trailing\nint b = 2; /* inline */ int c;\n#endif\n") == h0);
  // end-of-line comment shortened / lengthened -> same (only trailing blanks differ)
  CHECK(content_key_sha("#ifndef X_H\n#define X_H\n/* licence\n   text */\nint a; // t\nint b = 2; /* inline */ int c;\n#endif\n") == h0);
  // inline comment of a different length moves the column of `int c` -> different
  CHECK(content_key_sha("#ifndef X_H\n#define X_H\n/* licence\n   text */\nint a; // trailing\nint b = 2; /* in */ int c;\n#endif\n") != h0);
  // a comment line inserted in the middle moves later lines -> different
  CHECK(content_key_sha("#ifndef X_H\n#define X_H\n// new\n/* licence\n   text */\nint a; // trailing\nint b = 2; /* inline */ int c;\n#endif\n") != h0);
  // a code change -> different
  CHECK(content_key_sha("#ifndef X_H\n#define X_H\n/* licence\n   text */\nint a; // trailing\nint b = 3; /* inline */ int c;\n#endif\n") != h0);
  // strings and header names are not comments
  CHECK(content_key_sha("const char* s = \"a // b\";\n") != content_key_sha("const char* s = \"a // c\";\n"));
  CHECK(content_key_sha("#include <a//b.h>\n") != content_key_sha("#include <a//c.h>\n"));
  CHECK(content_key_sha("char q = '\\'';\n// x\n") == content_key_sha("char q = '\\'';\n// yy\n"));
  // unmodelled -> raw hash (never "n:")
  content_key_sha("auto s = R\"(x // y)\";\n", &n); CHECK(!n);
  content_key_sha("int a ??= 1;\n", &n); CHECK(!n);
  content_key_sha("// comment continued \\\nint hidden;\n", &n); CHECK(!n);
  content_key_sha("/* unterminated\n", &n); CHECK(!n);
  content_key_sha("#define M \\   \nint x;\n", &n); CHECK(!n);   // backslash then blanks: splice semantics -> raw
  CHECK(cachekey::keeps_comments({"cc", "-E", "-C", "x.c"})); CHECK(!cachekey::keeps_comments({"cc", "-c", "x.c"}));
}

// The property the cache relies on: whenever two variants hash equal (and the flags allow the normalised hash, i.e.
// no debug info), the real compiler produces byte-identical objects from the same path. With -g the raw hash is used
// (DWARF 5 embeds the source MD5), which this test also checks.
OPTICC_TEST(content_key_equal_implies_identical_object) {
  std::string d = tmpdir("opticc-ckobj-");
  const std::vector<std::string> variants = {
    "/* hdr */\nint add(int a, int b) { return a + b; } // add\nint g = 7; /* end */\n",
    "/* HEADER COMMENT */\nint add(int a, int b) { return a + b; } // addition\nint g = 7; /* END */\n",
    "/* hdr */\nint add(int a, int b) { return a + b; } // add\nint g = 7; /* end */\n/* opticc-touch 1 */\n\n",
    "/* hdr */\nint add(int a, int b) { return a + b; }\nint g = 7;\n",
  };
  std::string ref_sha, ref_obj; int compared = 0;
  for (auto& v : variants) {
    util::write_file(d + "/t.c", v);
    auto rr = util::run_process({"cc", "-O1", "-c", d + "/t.c", "-o", d + "/t.o"}, "", "", d);
    if (rr.rc != 0) { std::printf("  skipped: cc not usable\n"); return; }
    std::string ks = cachekey::content_key_sha(v), obj = util::read_file(d + "/t.o");
    if (ref_sha.empty()) { ref_sha = ks; ref_obj = obj; continue; }
    CHECK(ks == ref_sha);   // every variant above is comment-only and token-position preserving
    CHECK(obj == ref_obj); ++compared;
  }
  CHECK(compared == 3);
  CHECK(cachekey::keeps_comments({"cc", "-g", "-c", "t.c"})); CHECK(cachekey::keeps_comments({"cc", "-gdwarf-5", "-c", "t.c"}));
  CHECK(!cachekey::keeps_comments({"cc", "-g0", "-c", "t.c"})); CHECK(!cachekey::keeps_comments({"cc", "-O2", "-c", "t.c"}));
}

// lookup_manifest_fn: entries are matched by shape and per-file key hashes supplied by the caller (files outside the
// shape's view are verified on demand), newest entry first.
OPTICC_TEST(objcache_manifest_fn_matching) {
  std::string d = tmpdir("opticc-mfn-");
  objcache::ObjectCache c; c.init(d, 1u << 30);
  std::unordered_map<std::string, std::string> view{{"a.c", "s1"}, {"inc/x.h", "s2"}};
  std::string shape = objcache::ObjectCache::shape_hash(view);
  c.store("O1", "obj-bytes", " a.c inc/x.h gen.h", "", "warn", 0);
  c.add_manifest_entry("M", {{"a.c", "s1"}, {"inc/x.h", "s2"}, {"gen.h", "g1"}}, "O1", shape);
  std::unordered_map<std::string, std::string> fs{{"a.c", "s1"}, {"inc/x.h", "s2"}, {"gen.h", "g1"}};
  auto sha_of = [&](const std::string& r) { auto it = fs.find(r); return it == fs.end() ? std::string() : it->second; };
  objcache::Hit h;
  CHECK(c.lookup_manifest_fn("M", shape, sha_of, &h)); CHECK(h.object == "obj-bytes"); CHECK(h.err == "warn");
  fs["gen.h"] = "g2";                                    // a file outside the shape's view changed -> miss
  CHECK(!c.lookup_manifest_fn("M", shape, sha_of, &h));
  fs["gen.h"] = "g1";
  CHECK(!c.lookup_manifest_fn("M", "other-shape", sha_of, &h));   // a different scanned closure -> miss
  objcache::ObjectCache c2; c2.init(d, 1u << 30);        // persisted and reloaded
  CHECK(c2.lookup_manifest_fn("M", shape, sha_of, &h));
}

// Soundness of the comment-insensitive key.
OPTICC_TEST(content_key_soundness) {
  using cachekey::content_key_sha;
  bool n = false;
  // C++14 digit separators: a stale-object example. 1'000 must not open a character literal that swallows
  // "x'y" and makes the following "/*" ... "*/" look like a comment: changing b must change the key.
  const std::string a = "int a = 1'000; const char* s = \"x'y\"; const char* t = \"/*\"; int b = 1; const char* u = \"*/\";\n";
  const std::string b = "int a = 1'000; const char* s = \"x'y\"; const char* t = \"/*\"; int b = 2; const char* u = \"*/\";\n";
  CHECK(content_key_sha(a) != content_key_sha(b));
  CHECK(content_key_sha("int x = 0x1'ff'ff; // c\n") == content_key_sha("int x = 0x1'ff'ff; // d\n"));   // still normalised
  content_key_sha("double d = 1.5e+10; int q = u8'x';\n", &n); CHECK(n);
  // raw strings: rejected (raw hash), never treated as comments
  content_key_sha("auto s = R\"(/* x */)\";\n", &n); CHECK(!n);
  content_key_sha("auto s = u8R\"d(a)d\";\n", &n); CHECK(!n);
  content_key_sha("int Rx = 1; // c\n", &n); CHECK(n);   // an identifier ending in R is not a raw prefix
  // comments whose CONTENT can change a diagnostic -> raw hash
  content_key_sha("/* outer /* nested-looking */ int x;\n", &n); CHECK(!n);                              // -Wcomment
  content_key_sha("switch (x) { case 1: f(); /* fall through */ case 2: g(); }\n", &n); CHECK(!n);        // GCC fall-through
  content_key_sha("// FALLTHRU\nint x;\n", &n); CHECK(!n);
  content_key_sha("// falls-through\nint x;\n", &n); CHECK(!n);
  content_key_sha("// caf\xc3\xa9\nint x;\n", &n); CHECK(!n);                                               // non-ASCII
  content_key_sha("// a fallback path\nint x;\n", &n); CHECK(n);                                            // "fall" alone
  // comment-reading warning flags -> raw hash for the whole compile
  CHECK(cachekey::keeps_comments({"clang", "-Wdocumentation", "-c", "x.cpp"}));
  CHECK(cachekey::keeps_comments({"clang", "-Weverything", "-c", "x.cpp"}));
  CHECK(cachekey::keeps_comments({"clang", "-Werror=documentation", "-c", "x.cpp"}));
  CHECK(!cachekey::keeps_comments({"clang", "-Wall", "-c", "x.cpp"}));
}

// Equal key => identical object, on adversarial inputs.
OPTICC_TEST(content_key_equal_implies_identical_object_full) {
  std::string d = tmpdir("opticc-ck51-");
  const std::vector<std::string> v = {
    "int a = 1'000; const char* s = \"x'y\"; const char* t = \"/*\"; int b = 1; const char* u = \"*/\";\n",
    "int a = 1'000; const char* s = \"x'y\"; const char* t = \"/*\"; int b = 2; const char* u = \"*/\";\n",
    "int a = 1'000; /* c */ int b = 1;\n",
    "int a = 1'000; /* d */ int b = 1;\n",
  };
  std::vector<std::string> keys, objs;
  for (auto& s : v) {
    util::write_file(d + "/t.cpp", s);
    auto rr = util::run_process({"c++", "-std=c++17", "-O1", "-c", d + "/t.cpp", "-o", d + "/t.o"}, "", "", d);
    if (rr.rc != 0) { std::printf("  skipped: c++ not usable\n"); return; }
    keys.push_back(cachekey::content_key_sha(s)); objs.push_back(util::read_file(d + "/t.o"));
  }
  for (size_t i = 0; i < v.size(); ++i)
    for (size_t j = i + 1; j < v.size(); ++j)
      if (keys[i] == keys[j]) CHECK(objs[i] == objs[j]);
  CHECK(keys[0] != keys[1]);   // the stale-object pair is distinguished
  CHECK(keys[2] == keys[3]);   // a plain comment edit still shares a key
}

// After a header edit every job's old object is gone, but a stale affinity hint still names its old holder. Treated
// as a CERTAIN hit (p = 1, as with a manifest-key Bloom filter) it piles every job on that holder without reserving
// cores; treated as an uncertain hit (p = 0.95, a Bloom hit on the full key) the scheduler uses the expected completion and keeps the load balanced.
OPTICC_TEST(affinity_stale_hint_does_not_skew_placement) {
  Topology t = two_nodes(1.0, 1.0);
  Workload wl = jobs(60, 1.0);
  ECTScheduler s0(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r0 = sim::Simulator(t, wl, s0).run();                     // no affinity at all
  auto share1 = [](const sim::Result& r) { int n = 0, m = 0; for (int k : r.job_node) { if (k > 0) ++m; if (k == 1) ++n; } return m ? double(n) / m : 0.0; };
  sim::Options stale; stale.believed_on.assign(wl.jobs.size(), 1);     // every job "held" by node 1, none really
  stale.believed_p = 1.0;                                               // a certain hit
  ECTScheduler s1(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r1 = sim::Simulator(t, wl, s1, stale).run();
  CHECK(r1.cache_hits == 0);
  CHECK(share1(r1) > 0.9);                                              // skewed: the failure being guarded against
  CHECK(r1.makespan > 1.5 * r0.makespan);
  stale.believed_p = 0.95;                                              // a Bloom hit is uncertain
  ECTScheduler s2(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r2 = sim::Simulator(t, wl, s2, stale).run();
  CHECK(r2.cache_hits == 0);
  CHECK(share1(r2) < 0.7);                                              // placement stays balanced
  CHECK(r2.makespan <= 1.10 * r0.makespan);                             // within 10 % of no affinity
  // a REAL cached object still attracts its job (the useful case is kept)
  sim::Options real; real.cached_on.assign(wl.jobs.size(), -1); real.cached_on[0] = 1;
  ECTScheduler s3(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r3 = sim::Simulator(t, wl, s3, real).run();
  CHECK(r3.job_node[0] == 1 && r3.cache_hits == 1);
}

// Every hint is RIGHT (a fresh client after a clean build). At the prior p = 0.95 each Bloom hit reserves a core and
// a window slot, and work conservation can send hinted jobs to the other node, where they miss. With the LEARNED precision the hints
// earn trust (p >= kCertainHit after ~30 real hits on holders) and every job is served by its holder; stale hints
// (misses on holders) never reach it, so the stale-hint guarantee above is kept.
OPTICC_TEST(affinity_learned_bloom_precision) {
  using opticc::Scheduler;
  CHECK(Scheduler::learned_hit_p(0, 0) == 0.95);                        // the prior: uncertain
  CHECK(Scheduler::learned_hit_p(0, 0) < Scheduler::kCertainHit);
  CHECK(Scheduler::learned_hit_p(30, 30) >= Scheduler::kCertainHit);    // 30 real hits on holders, no miss: trusted
  CHECK(Scheduler::learned_hit_p(29, 29) < Scheduler::kCertainHit);
  CHECK(Scheduler::learned_hit_p(0, 50) < 0.3);                         // stale hints: misses on the holder
  CHECK(Scheduler::learned_hit_p(1000, 1100) < Scheduler::kCertainHit); // 9 % misses on holders: never trusted
  CHECK(Scheduler::learned_hit_p(1000, 1008) >= Scheduler::kCertainHit); // a ~0.8 % false-positive rate stays trusted
  Topology t = two_nodes(1.0, 1.0);
  Workload wl = jobs(60, 1.0);
  sim::Options right; right.cached_on.assign(wl.jobs.size(), 1); right.believed_on.assign(wl.jobs.size(), 1);
  right.believed_p = Scheduler::learned_hit_p(0, 0);                    // before any evidence
  ECTScheduler s1(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r1 = sim::Simulator(t, wl, s1, right).run();
  right.believed_p = Scheduler::learned_hit_p(30, 30);                  // after 30 real hits on holders
  ECTScheduler s2(t, wl, std::make_shared<OraclePredictor>(), ECTOptions{true, false, HedgeRule::None});
  sim::Result r2 = sim::Simulator(t, wl, s2, right).run();
  std::printf("  right hints: p=0.95 hits %d makespan %.3f; learned p hits %d makespan %.3f\n", r1.cache_hits, r1.makespan, r2.cache_hits, r2.makespan);
  CHECK(r2.cache_hits == static_cast<int>(wl.jobs.size()));             // every job served by its holder
  CHECK(r2.cache_hits >= r1.cache_hits);
  CHECK(r2.makespan <= 1.01 * r1.makespan + 1e-9);
}
