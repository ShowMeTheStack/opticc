// SPDX-License-Identifier: GPL-2.0-only
// The client-side DIRECT cache (opticc/direct.hpp) and the scanner's direct_inputs (incscan.hpp).
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "opticc/argscan.hpp"
#include "opticc/bloom.hpp"
#include "opticc/cachekey.hpp"
#include "opticc/direct.hpp"
#include "opticc/incscan.hpp"
#include "opticc/probe.hpp"
#include "opticc/sysinc.hpp"
#include "opticc/util.hpp"
#include "test_harness.hpp"

namespace dr = opticc::direct;
namespace ut = opticc::util;

static dr::Pos pos_of(const std::string& p) { return dr::Pos{p, dr::stat_path(p), ut::sha256_hex(ut::read_file(p))}; }
static void set_mtime(const std::string& p, time_t sec) {
  struct timespec ts[2]; ts[0].tv_sec = ts[1].tv_sec = sec; ts[0].tv_nsec = ts[1].tv_nsec = 0;
  utimensat(AT_FDCWD, p.c_str(), ts, 0);
}
// Linux stamps ctime/mtime from the COARSE kernel clock (one tick, 1 to 4 ms): two writes within one tick can leave a file
// stat-identical (same ino, size, mtime, ctime) although its bytes changed, and these unit tests build entries with pos_of
// directly from just-written files. Production never records such an entry: the coordinator refuses any input whose
// ctime is within the racy margin (2 ticks + 2 ms) of the compile start (direct::changed_since), so any later rewrite gets
// a strictly newer ctime and fails dr::same(). settle() gives the tests the same precondition (macOS stamps at ns
// resolution, which is why they passed there): wait past the margin before a rewrite whose detection the test asserts.
static void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
static bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

OPTICC_TEST(direct_encode_decode_roundtrip) {
  std::string d = ut::make_temp_dir("opticc-dr-");
  ut::write_file(d + "/a.h", "int a;\n");
  dr::Entry e; e.obj = "0123abcd"; e.recorded_wall = 12345.5; e.pos.push_back(pos_of(d + "/a.h"));
  dr::NegGroup g; g.dir = d + "/inc"; g.st = dr::stat_path(g.dir); g.volatile_dir = true; g.names = {"x.h", "sub/y.h"}; e.neg.push_back(g);
  std::vector<dr::Entry> in{e, e}; in[1].obj = "ffff";
  std::string blob = dr::encode(in);
  std::vector<dr::Entry> out; CHECK(dr::decode(blob, &out)); CHECK(out.size() == 2);
  CHECK(out[0].obj == "0123abcd"); CHECK(out[1].obj == "ffff"); CHECK(out[0].recorded_wall == 12345.5);
  CHECK(out[0].pos.size() == 1 && out[0].pos[0].path == d + "/a.h" && dr::same(out[0].pos[0].st, e.pos[0].st) && out[0].pos[0].sha == e.pos[0].sha);
  CHECK(out[0].neg.size() == 1 && out[0].neg[0].names == g.names && out[0].neg[0].volatile_dir && !out[0].neg[0].st.exists);
  // corrupt / truncated / foreign blobs are rejected (a lookup then misses; never a wrong hit)
  CHECK(!dr::decode(blob.substr(0, blob.size() - 1), &out));
  CHECK(!dr::decode(blob + "x", &out));
  CHECK(!dr::decode("OPD3" + blob.substr(4), &out));
  CHECK(!dr::decode("", &out));
}

// a positive input: exact stat match -> valid without reading; stat differs -> content decides (raw sha)
OPTICC_TEST(direct_positive_stat_then_content) {
  std::string d = ut::make_temp_dir("opticc-dr-");
  std::string f = d + "/h.h"; ut::write_file(f, "int v = 1;\n"); set_mtime(f, time(nullptr) - 100);
  dr::Entry e; e.obj = "o"; e.pos.push_back(pos_of(f));
  std::string why; CHECK(dr::valid(e, &why));
  set_mtime(f, time(nullptr) - 50);                 // touch: same bytes -> still valid (content check)
  CHECK(dr::valid(e, &why));
  // same size, different bytes, mtime restored (`touch -r`, rsync -t, cp -p): ctime differs -> content -> invalid
  settle(); ut::write_file(f, "int v = 2;\n"); set_mtime(f, time(nullptr) - 100);
  CHECK(!dr::valid(e, &why)); CHECK(why.find("content") != std::string::npos);
  // replaced by a different file (new inode) with identical bytes -> valid; with other bytes of the same size -> invalid
  ut::write_file(d + "/new", "int v = 1;\n"); std::rename((d + "/new").c_str(), f.c_str());
  CHECK(dr::valid(e, &why));
  ut::write_file(d + "/new", "int v = 3;\n"); std::rename((d + "/new").c_str(), f.c_str());
  CHECK(!dr::valid(e, &why));
  ::unlink(f.c_str()); CHECK(!dr::valid(e, &why)); CHECK(why.find("gone") != std::string::npos);
}

// negative probes: a header created where the compile found nothing (earlier in the search path) invalidates
OPTICC_TEST(direct_negative_shadowing) {
  std::string d = ut::make_temp_dir("opticc-dr-");
  ut::mkdir_p(d + "/a"); ut::mkdir_p(d + "/b");
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));   // the directories are no longer "recent"
  std::vector<dr::NegGroup> neg;
  CHECK(dr::make_negative_groups({d + "/a/x.h", d + "/a/sub/y.h", d + "/nodir/z.h"}, dr::wall_now(), &neg));
  CHECK(neg.size() == 3);
  for (auto& g : neg) CHECK(!g.volatile_dir);
  dr::Entry e; e.obj = "o"; e.neg = neg;
  std::string why; CHECK(dr::valid(e, &why));
  ut::write_file(d + "/a/other.h", "\n");          // unrelated file: directory changed, probed names still absent
  CHECK(dr::valid(e, &why));
  ut::write_file(d + "/a/x.h", "\n");              // shadowing header appears
  CHECK(!dr::valid(e, &why)); CHECK(why.find("shadowing") != std::string::npos);
  ::unlink((d + "/a/x.h").c_str()); CHECK(dr::valid(e, &why));
  ut::mkdir_p(d + "/nodir"); CHECK(dr::valid(e, &why));   // a missing directory created, still without z.h
  ut::write_file(d + "/nodir/z.h", "\n"); CHECK(!dr::valid(e, &why));
  ::unlink((d + "/nodir/z.h").c_str());
  ut::mkdir_p(d + "/a/sub"); ut::write_file(d + "/a/sub/y.h", "\n"); CHECK(!dr::valid(e, &why));
  // a candidate that exists at recording time: nothing may be recorded
  std::vector<dr::NegGroup> n2; CHECK(!dr::make_negative_groups({d + "/a/sub/y.h"}, dr::wall_now(), &n2));
}

// a directory changed within kRacyS of the compile is VOLATILE: its names are probed on every lookup, even when the
// directory's stat is unchanged (a create+delete inside one timestamp tick would otherwise go unnoticed)
OPTICC_TEST(direct_volatile_directory) {
  std::string d = ut::make_temp_dir("opticc-dr-");
  ut::mkdir_p(d + "/gen"); ut::write_file(d + "/gen/tmp", "\n");
  std::vector<dr::NegGroup> neg;
  CHECK(dr::make_negative_groups({d + "/gen/x.h"}, dr::wall_now(), &neg));
  CHECK(neg.size() == 1 && neg[0].volatile_dir);
  dr::Entry e; e.obj = "o"; e.neg = neg; e.neg[0].st = dr::stat_path(d + "/gen");
  ut::write_file(d + "/gen/x.h", "\n"); e.neg[0].st = dr::stat_path(d + "/gen");   // even with a "matching" dir stat
  std::string why; CHECK(!dr::valid(e, &why));
}

OPTICC_TEST(direct_key_sensitivity) {
  std::vector<std::string> av{"/usr/bin/clang", "-O2", "-Iinc", "-Ib", "-DX=1", "-c", "t.c", "-o", "t.o"};
  std::string k = dr::key("cc|1", av, "/w", {}, "s1");
  CHECK(k.size() == 64);
  CHECK(k == dr::key("cc|1", av, "/w", {}, "s1"));
  auto v = av; v[4] = "-DX=2"; CHECK(k != dr::key("cc|1", v, "/w", {}, "s1"));                // -D change
  v = av; std::swap(v[2], v[3]); CHECK(k != dr::key("cc|1", v, "/w", {}, "s1"));                // -I order change
  v = av; v[8] = "u.o"; CHECK(k != dr::key("cc|1", v, "/w", {}, "s1"));                         // output name
  CHECK(k != dr::key("cc|2", av, "/w", {}, "s1"));                                               // compiler identity
  CHECK(k != dr::key("cc|1", av, "/x", {}, "s1"));                                               // cwd
  CHECK(k != dr::key("cc|1", av, "/w", {"CPATH=/i"}, "s1"));                                     // include env
  CHECK(k != dr::key("cc|1", av, "/w", {}, "s2"));                                               // source bytes
  // argument boundaries are unambiguous
  CHECK(dr::key("c", {"-a", "b"}, "/w", {}, "s") != dr::key("c", {"-ab"}, "/w", {}, "s"));
  // the compiler identity follows the executable (stat): a replaced binary changes it
  std::string d = ut::make_temp_dir("opticc-dr-"); ut::write_file(d + "/cc", "v1"); std::string id1 = dr::compiler_identity(d + "/cc");
  ut::write_file(d + "/n", "v2"); std::rename((d + "/n").c_str(), (d + "/cc").c_str()); CHECK(id1 != dr::compiler_identity(d + "/cc"));
}

OPTICC_TEST(direct_record_lookup) {
  std::string d = ut::make_temp_dir("opticc-dr-");
  std::string f = d + "/h.h"; ut::write_file(f, "a\n"); set_mtime(f, time(nullptr) - 100);
  std::string dk(64, 'a');
  std::string why; CHECK(dr::lookup(d, dk, &why).empty()); CHECK(why == "no manifest");
  for (int i = 0; i < 10; ++i) { dr::Entry e; e.obj = "o" + std::to_string(i); e.pos.push_back(pos_of(f)); dr::record(d, dk, e); }
  std::vector<dr::Entry> es; CHECK(dr::decode(ut::read_file(dr::manifest_path(d, dk)), &es));
  CHECK(es.size() == dr::kMaxEntries); CHECK(es.back().obj == "o9"); CHECK(es.front().obj == "o2");
  CHECK(dr::lookup(d, dk) == "o9");   // newest valid entry first
  { dr::Entry e; e.obj = "o5"; e.pos.push_back(pos_of(f)); dr::record(d, dk, e); }   // dedup by object: moved to the end
  CHECK(dr::decode(ut::read_file(dr::manifest_path(d, dk)), &es)); CHECK(es.size() == dr::kMaxEntries); CHECK(es.back().obj == "o5");
  CHECK(std::count_if(es.begin(), es.end(), [](const dr::Entry& x) { return x.obj == "o5"; }) == 1);
  // an entry for other content: after the edit only that one is valid
  settle(); ut::write_file(f, "b\n");
  { dr::Entry e; e.obj = "ob"; e.pos.push_back(pos_of(f)); dr::record(d, dk, e); }
  settle(); ut::write_file(f, "a\n"); CHECK(dr::lookup(d, dk) == "o5");   // back to the old bytes: the older entry is valid again
  settle(); ut::write_file(f, "b\n"); CHECK(dr::lookup(d, dk) == "ob");
  settle(); ut::write_file(f, "c\n"); CHECK(dr::lookup(d, dk, &why).empty());
  ut::write_file(dr::manifest_path(d, dk), "garbage"); CHECK(dr::lookup(d, dk, &why).empty()); CHECK(why == "corrupt manifest");
}

// scanner: absent candidates (shadowing), #include_next probing every directory, system traversal, computed includes
OPTICC_TEST(incscan_direct_inputs) {
  std::string d = ut::make_temp_dir("opticc-dri-");
  for (auto s : {"/i1", "/i2", "/sys/bits"}) ut::mkdir_p(d + s);
  ut::write_file(d + "/i2/a.h", "#pragma once\n#include <b.h>\n#include_next <n.h>\n");
  ut::write_file(d + "/i2/b.h", "#pragma once\n#include <sysh.h>\n");
  ut::write_file(d + "/sys/sysh.h", "#pragma once\n#include <bits/x.h>\n");
  ut::write_file(d + "/sys/bits/x.h", "#pragma once\n");
  ut::write_file(d + "/i2/n.h", "#pragma once\n");
  ut::write_file(d + "/i2/c.h", "#pragma once\n#include MACRO_H\n");
  ut::write_file(d + "/t.c", "#include \"a.h\"\n#include <c.h>\n#include <missing.h>\n");
  auto dirs = opticc::args::include_dirs({"cc", "-I", d + "/i1", "-I", d + "/i2", "-c", d + "/t.c"});
  std::vector<std::string> sysd{d + "/sys"};
  opticc::incscan::Scanner s; bool comp = false;
  auto cl = s.closure(d + "/t.c", dirs, sysd, d, &comp);
  CHECK(has(cl, d + "/i2/a.h")); CHECK(has(cl, d + "/i2/b.h")); CHECK(!has(cl, d + "/sys/sysh.h"));
  std::vector<std::string> extra, computed; std::vector<std::pair<int, int>> negp; bool hic = false;
  s.direct_inputs(d + "/t.c", cl, dirs, sysd, d, &extra, &negp, &computed, &hic);
  CHECK(!hic);
  CHECK(has(extra, d + "/sys/sysh.h")); CHECK(has(extra, d + "/sys/bits/x.h"));      // system headers traversed
  std::vector<dr::NegGroup> groups; CHECK(s.negative_groups(negp, dr::wall_now(), &groups));
  std::vector<std::string> absent;
  for (auto& g : groups) for (auto& n : g.names) absent.push_back(g.dir + "/" + n);
  CHECK(has(absent, d + "/a.h"));            // quoted: includer dir first
  CHECK(has(absent, d + "/i1/a.h"));         // then -I i1 (before i2 where it was found)
  CHECK(!has(absent, d + "/i2/a.h"));
  CHECK(has(absent, d + "/i1/sysh.h")); CHECK(has(absent, d + "/i2/sysh.h"));         // a header in -I would shadow sysh.h
  CHECK(has(absent, d + "/i1/bits/x.h")); CHECK(has(absent, d + "/i2/bits/x.h"));     // ... also for system-internal includes
  CHECK(has(absent, d + "/i1/missing.h")); CHECK(has(absent, d + "/sys/missing.h"));
  CHECK(has(absent, d + "/sys/n.h"));        // include_next: every directory is probed (also past the first hit)
  CHECK(computed.size() == 1 && computed[0] == d + "/i2/c.h");
  // the memoised parse must describe the file as it is now
  CHECK(s.parse_matches(d + "/i2/b.h", dr::stat_path(d + "/i2/b.h")));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ut::write_file(d + "/i2/b.h", "#pragma once\n#include <sysh.h>\n#include <z.h>\n");
  CHECK(!s.parse_matches(d + "/i2/b.h", dr::stat_path(d + "/i2/b.h")));
  // a candidate that exists now: no negative groups (the caller records nothing); and closure() re-resolves
  ut::write_file(d + "/i1/sysh.h", "#pragma once\n");
  std::vector<dr::NegGroup> g2; std::vector<std::pair<int, int>> negp2 = negp; CHECK(!s.negative_groups(negp2, dr::wall_now(), &g2));
  auto cl2 = s.closure(d + "/t.c", dirs, sysd, d, &comp);
  CHECK(has(cl2, d + "/i1/sysh.h"));         // the shadowing header is now part of the closure
}

// The worker's cache-key Bloom filter (HELLO_ACK): no false negatives, ~1% false positives at 10 bits/key,
// JSON round trip, malformed input rejected (empty filter: never "maybe")
OPTICC_TEST(bloom_filter_roundtrip) {
  std::vector<std::string> keys; for (int i = 0; i < 5000; ++i) keys.push_back(ut::sha256_hex("k" + std::to_string(i)));
  auto f = opticc::bloom::Filter::build(keys);
  for (auto& k : keys) CHECK(f.maybe(k));
  int fp = 0; for (int i = 0; i < 20000; ++i) fp += f.maybe(ut::sha256_hex("x" + std::to_string(i))) ? 1 : 0;
  CHECK(fp < 400);   // < 2%
  auto g = opticc::bloom::Filter::from_json(f.to_json());
  CHECK(g.m == f.m && g.k == f.k && g.n == 5000 && g.bits == f.bits);
  for (int i = 0; i < 100; ++i) CHECK(g.maybe(keys[static_cast<size_t>(i)]));
  auto j = f.to_json(); j["hex"] = std::string(j["hex"]).substr(2);
  CHECK(opticc::bloom::Filter::from_json(j).m == 0); CHECK(!opticc::bloom::Filter::from_json(j).maybe(keys[0]));
  CHECK(!opticc::bloom::Filter{}.maybe(keys[0]));
  auto e = opticc::bloom::Filter::build({}); CHECK(e.m == 1024); CHECK(!e.maybe(keys[0]));
}

// keysha entries: a comment appended (size changes) still matches; a token change does not; round trip keeps the flag
OPTICC_TEST(direct_keysha_positive) {
  std::string d = ut::make_temp_dir("opticc-dr-");
  std::string f = d + "/h.h"; ut::write_file(f, "int v = 1;\n");
  dr::Entry e; e.obj = "o"; e.keysha = true;
  e.pos.push_back(dr::Pos{f, dr::stat_path(f), dr::content_sha(ut::read_file(f), true)});
  std::vector<dr::Entry> rt; CHECK(dr::decode(dr::encode({e}), &rt)); CHECK(rt.size() == 1 && rt[0].keysha);
  ut::write_file(f, "int v = 1;\n/* touched */\n"); CHECK(dr::valid(e));
  ut::write_file(f, "int v = 2;\n/* touched */\n"); CHECK(!dr::valid(e));
  e.keysha = false; e.pos[0] = dr::Pos{f, dr::stat_path(f), dr::content_sha(ut::read_file(f), false)};
  ut::write_file(f, "int v = 2;\n/* touched */ \n"); CHECK(!dr::valid(e));   // raw entries: any byte change misses
}

OPTICC_TEST(direct_safe_args) {
  std::string why;
  CHECK(dr::safe_args({"clang", "-O2", "-Iinc", "-DX=1", "-MD", "-MT", "a.o", "-MF", "a.o.d", "-c", "a.c", "-o", "a.o"}, &why));
  CHECK(dr::safe_args({"clang", "-nostdinc", "--target=x86_64-linux-gnu", "-Wp,-MMD,a/.b.o.d", "-include", "k.h", "-c", "b.c", "-o", "b.o"}));
  for (auto bad : {"@args.rsp", "-fprofile-use=x.profdata", "-fprofile-instr-use=p", "-fsanitize-ignorelist=l.txt", "-fmodules",
                   "-fmodule-map-file=m", "-include-pch", "-fplugin=p.so", "-specs=s", "-B/opt/tools", "x.pch", "-fpch-preprocess"}) {
    CHECK(!dr::safe_args({"clang", bad, "-c", "a.c", "-o", "a.o"}, &why)); CHECK(why == bad);
  }
  CHECK(!dr::safe_args({"clang", "-c", "a.c", "-o", "-"})); CHECK(!dr::safe_args({"clang", "-c", "a.c", "-o", "/dev/null"}));
  CHECK(!dr::safe_args({"clang", "-Xclang", "-load", "-Xclang", "p.so", "-c", "a.c", "-o", "a.o"}));
  CHECK(!dr::changes_system_search({"clang", "-O2", "-Iinc", "-c", "a.c"}));
  for (auto f : {"--sysroot=/s", "-isysroot", "--target=aarch64-linux-gnu", "-target", "-stdlib=libc++", "--gcc-toolchain=/g", "-nostdinc++", "-m32"})
    CHECK(dr::changes_system_search({"clang", f, "-c", "a.c"}));
}

// The recording margin (direct.hpp changed_since): a change at or after t0 - margin refuses the entry; an older
// one is accepted; a timestamp without nanoseconds (1-s filesystem) needs the coarse margin.
OPTICC_TEST(direct_changed_since_margin) {
  const double m = dr::racy_margin_s();
  CHECK(m > 0 && m <= dr::kRacyS + 1e-12);
  dr::FStat f; f.exists = true; f.ctime_ns = 1791973600LL * 1000000000LL + 123456789LL;
  const double ct = static_cast<double>(f.ctime_ns) / 1e9;
  CHECK(dr::changed_since(f, ct));                          // changed at t0 itself
  CHECK(dr::changed_since(f, ct + m * 0.5));                // inside the margin
  CHECK(!dr::changed_since(f, ct + m + 0.001));             // older than the margin
  dr::FStat g = f; g.ctime_ns = 1791973600LL * 1000000000LL;   // whole seconds: truncated stamps
  CHECK(dr::changed_since(g, 1791973600.0 + 1.0));
  CHECK(dr::changed_since(g, 1791973600.0 + dr::kRacyCoarseS - 0.01));
  CHECK(!dr::changed_since(g, 1791973600.0 + dr::kRacyCoarseS + 0.01));
}

// The shared content-hash memo: a regenerated file with identical bytes is hashed once, then answered from the
// memo while its stat is unchanged; a memo with another stat is ignored; a file younger than the margin is not memoised.
OPTICC_TEST(direct_hash_memo) {
  setenv("OPTICC_RACY_S", "0.5", 1);   // this process: margin 0.5 s (read once, on first use)
  CHECK(dr::racy_margin_s() == 0.5);
  std::string d = ut::make_temp_dir("opticc-memo-"), c = d + "/cache", f = d + "/gen.inc";
  auto regen = [&](const std::string& body) { ut::write_file(f + ".tmp", body); std::rename((f + ".tmp").c_str(), f.c_str()); };
  regen(std::string(100000, 'A'));
  dr::Entry e; e.obj = "o"; e.pos.push_back(pos_of(f));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  regen(std::string(100000, 'A'));   // regenerated, identical bytes: new inode, new ctime
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  std::string s;
  CHECK(!dr::hash_memo_get(c, f, dr::stat_path(f), false, &s));
  CHECK(dr::valid(e, nullptr, c));                                        // hashed, and memoised (settled)
  CHECK(dr::hash_memo_get(c, f, dr::stat_path(f), false, &s) && s == e.pos[0].sha);
  CHECK(dr::valid(e, nullptr, c));                                        // now from the memo
  CHECK(!dr::hash_memo_get(c, f, dr::stat_path(f), true, &s));            // the other hash kind is separate
  regen(std::string(99999, 'A') + "B");                                   // same size, other bytes
  CHECK(!dr::hash_memo_get(c, f, dr::stat_path(f), false, &s));           // stale memo: stat differs
  CHECK(!dr::valid(e, nullptr, c));
  regen(std::string(100000, 'A'));                                        // identical again, but younger than the margin
  CHECK(dr::valid(e, nullptr, c));
  CHECK(!dr::hash_memo_get(c, f, dr::stat_path(f), false, &s));           // not memoised while racy
  dr::hash_memo_put(c, f, dr::stat_path(f), false, std::string(64, '0'));  // a memo always describes its own stat only
  dr::FStat other = dr::stat_path(f); other.ctime_ns += 1;
  CHECK(!dr::hash_memo_get(c, f, other, false, &s));
}

// Assembler compiles are local-only but cacheable, and always use the raw (byte-exact) content hash
OPTICC_TEST(argscan_asm_local_cacheable) {
  auto s = opticc::args::scan({"clang", "-Wp,-MMD,x/.a.o.d", "-D__ASSEMBLY__", "-Iinc", "-c", "-o", "x/a.o", "a.S"});
  CHECK(!s.distributable); CHECK(s.local_cacheable); CHECK(s.lang == "asm"); CHECK(s.output == "x/a.o"); CHECK(s.has_md);
  auto t = opticc::args::scan({"clang", "-x", "assembler-with-cpp", "-c", "k.tmp", "-o", "k.o"});
  CHECK(t.local_cacheable && t.lang == "asm");
  auto u = opticc::args::scan({"clang", "-S", "a.c", "-o", "a.s"});   // not a -c compile: neither
  CHECK(!u.distributable && !u.local_cacheable);
  auto c = opticc::args::scan({"clang", "-c", "a.c", "-o", "a.o"});
  CHECK(c.distributable && !c.local_cacheable);
  CHECK(opticc::cachekey::keeps_comments({"clang", "-c", "a.S", "-o", "a.o"}));
  CHECK(opticc::cachekey::keeps_comments({"clang", "-c", "b.s", "-o", "b.o"}));
  CHECK(opticc::cachekey::keeps_comments({"clang", "-x", "assembler-with-cpp", "-c", "k.tmp", "-o", "k.o"}));
  CHECK(!opticc::cachekey::keeps_comments({"clang", "-c", "a.c", "-o", "a.o"}));
  CHECK(!opticc::cachekey::keeps_comments({"clang", "-c", "a.c", "-o", "a.s"}));   // an output name is not a source
}

// Compiler-probe classification (opticc/probe.hpp): kbuild's probe shapes are cacheable; anything that reads or
// writes a file the key cannot see, or a real source, is not; the output path is not in the key, its kind is.
OPTICC_TEST(probe_classify_kbuild_shapes) {
  namespace pr = opticc::probe;
  auto ok = [](std::vector<std::string> a) { return pr::classify(a).ok; };
  CHECK(ok({"/usr/bin/clang", "-Werror", "-D__KERNEL__", "-std=gnu11", "-Wno-address-of-packed-member", "-c", "-x", "c", "/dev/null", "-o", ".tmp_1/tmp"}));
  CHECK(ok({"/usr/bin/clang", "-Werror", "--target=x86_64-linux-gnu", "-Wa,--fatal-warnings", "-c", "-x", "assembler-with-cpp", "-o", ".tmp_1/tmp", "-"}));
  CHECK(ok({"/usr/bin/clang", "-Werror", "-mllvm", "-tsan-distinguish-volatile=1", "-c", "-x", "c", "/dev/null", "-o", "t"}));
  CHECK(ok({"/usr/bin/clang", "-dM", "-E", "-x", "c", "/dev/null"}));
  CHECK(ok({"/usr/bin/clang", "--version"}));
  CHECK(pr::classify({"/usr/bin/clang", "-c", "-x", "c", "/dev/null", "-o", "t"}).mode == pr::Mode::Compile);
  CHECK(pr::classify({"/usr/bin/clang", "-S", "-x", "c", "/dev/null", "-o", "t"}).mode == pr::Mode::Assemble);
  // not cacheable
  CHECK(!ok({"/usr/bin/clang", "-include", "h.h", "-c", "-x", "c", "/dev/null", "-o", "t"}));                 // reads h.h
  CHECK(!ok({"/usr/bin/clang", "-c", "-x", "c", "/dev/null", "-o", "t", "-MD", "-MF", "t.d"}));                // writes a dep file
  CHECK(!ok({"/usr/bin/clang", "-Wp,-MMD,t.d", "-c", "-x", "c", "/dev/null", "-o", "t"}));
  CHECK(!ok({"/usr/bin/clang", "-c", "-x", "c", "/dev/null", "-o", "t", "-gsplit-dwarf"}));                      // .dwo named after -o
  CHECK(!ok({"/usr/bin/clang", "@args", "-c", "-x", "c", "/dev/null", "-o", "t"}));
  CHECK(!ok({"/usr/bin/clang", "-fsanitize-ignorelist=x.txt", "-c", "-x", "c", "/dev/null", "-o", "t"}));
  CHECK(!ok({"/usr/bin/clang", "-Wa,-adhln=list", "-c", "-x", "assembler-with-cpp", "-o", "t", "-"}));
  CHECK(!ok({"/usr/bin/clang", "-mllvm", "-sample-profile-file=p", "-c", "-x", "c", "/dev/null", "-o", "t"}));
  CHECK(!ok({"/usr/bin/clang", "-c", "a.c", "-o", "a.o"}));                                                       // a real compile
  CHECK(!pr::classify({"/usr/bin/clang", "-c", "a.c", "-o", "a.o"}).looks_like_probe);
  CHECK(!ok({"/usr/bin/clang", "-c", "/dev/null", "-o", "t"}));                                                   // no -x
  CHECK(!ok({"/usr/bin/clang", "-c", "-x", "c", "/dev/null"}));                                                   // default output name
  CHECK(!ok({"/usr/bin/clang", "-c", "-x", "c", "/dev/null", "-o", "/dev/stdout"}));
  CHECK(!ok({"/usr/bin/clang", "-v"}));                                                                            // reads the filesystem
  CHECK(!ok({"/usr/bin/clang", "-print-file-name=plugin"}));
  // key: output path excluded, its kind included; argv, cwd and input included
  auto key = [](std::vector<std::string> a, std::string cwd = "/w", std::string in = "") {
    auto s = pr::classify(a); return pr::key("cid", "cfg", a, s, cwd, {}, opticc::util::sha256_hex(in));
  };
  const auto k1 = key({"/usr/bin/clang", "-Werror", "-c", "-x", "c", "/dev/null", "-o", ".tmp_1/tmp"});
  CHECK(k1 == key({"/usr/bin/clang", "-Werror", "-c", "-x", "c", "/dev/null", "-o", ".tmp_2/tmp"}));
  CHECK(k1 != key({"/usr/bin/clang", "-Werror", "-c", "-x", "c", "/dev/null", "-o", "/dev/null"}));
  CHECK(k1 != key({"/usr/bin/clang", "-Werror", "-O2", "-c", "-x", "c", "/dev/null", "-o", ".tmp_1/tmp"}));
  CHECK(k1 != key({"/usr/bin/clang", "-Werror", "-c", "-x", "c", "/dev/null", "-o", ".tmp_1/tmp"}, "/v"));
  CHECK(k1 != key({"/usr/bin/clang", "-Werror", "-c", "-x", "c", "/dev/null", "-o", ".tmp_1/tmp"}, "/w", "x"));
  CHECK(pr::key("cid", "cfg", {"/usr/bin/clang", "--version"}, pr::classify({"/usr/bin/clang", "--version"}), "/w", {}, "") !=
        pr::key("cid2", "cfg", {"/usr/bin/clang", "--version"}, pr::classify({"/usr/bin/clang", "--version"}), "/w", {}, ""));
  // stdin naming files; failure classification; result encoding
  CHECK(pr::input_reads_files("#include <x.h>\n") && pr::input_reads_files(".incbin \"f\"") && !pr::input_reads_files("vpdpbusd %xmm0,%xmm1,%xmm2\n"));
  CHECK(pr::cacheable_failure("clang: error: unknown argument: '-fno-such'\n"));
  CHECK(!pr::cacheable_failure("error: unable to open output file 'x/tmp': 'No such file or directory'\n"));
  CHECK(!pr::cacheable_failure(""));
  pr::Result r; r.rc = 1; r.out = "o"; r.err = std::string("e\0e", 3); r.has_obj = true; r.obj = std::string(1000, '\x7f');
  pr::Result r2;
  CHECK(pr::decode(pr::encode(r), &r2) && r2.rc == 1 && r2.out == "o" && r2.err == r.err && r2.has_obj && r2.obj == r.obj);
  CHECK(!pr::decode("OPP1", &r2));
}

// A symlink inside a fingerprinted directory that points OUTSIDE every fingerprinted directory
// (a file, or a whole directory) is followed: an edit of the target changes both the stat signature and the content
// fingerprint. A symlink to a file inside the roots stays a link string (that file is covered where it lives).
OPTICC_TEST(sysinc_follows_symlinks_out_of_the_roots) {
  const std::string d = opticc::util::make_temp_dir("opticc-sysinc-l-");
  opticc::util::mkdir_p(d + "/inc"); opticc::util::mkdir_p(d + "/opt/pkg/include");
  opticc::util::write_file(d + "/inc/a.h", "int a;\n");
  opticc::util::write_file(d + "/opt/x.h", "int x;\n");
  opticc::util::write_file(d + "/opt/pkg/include/y.h", "int y;\n");
  CHECK(::symlink((d + "/opt/x.h").c_str(), (d + "/inc/x.h").c_str()) == 0);            // file out of the roots
  CHECK(::symlink((d + "/opt/pkg/include").c_str(), (d + "/inc/pkg").c_str()) == 0);    // directory out of the roots
  CHECK(::symlink("a.h", (d + "/inc/alias.h").c_str()) == 0);                            // inside: link string only
  const std::vector<std::string> dirs{d + "/inc"};
  std::map<std::string, opticc::sysinc::FileSig> memo;
  const std::string s0 = opticc::sysinc::stat_signature(dirs), c0 = opticc::sysinc::content_fingerprint(dirs, &memo);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  opticc::util::write_file(d + "/opt/x.h", "int X;\n");                                 // same size, other bytes
  const std::string s1 = opticc::sysinc::stat_signature(dirs), c1 = opticc::sysinc::content_fingerprint(dirs, &memo);
  CHECK(s1 != s0); CHECK(c1 != c0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  opticc::util::write_file(d + "/opt/pkg/include/y.h", "int Y;\n");
  CHECK(opticc::sysinc::stat_signature(dirs) != s1); CHECK(opticc::sysinc::content_fingerprint(dirs, &memo) != c1);
  // an update-alternatives style chain whose END lies inside the roots: inc/c.h -> alt/c.h (outside)
  // -> inc/impl1/c.h; retargeting the middle link to inc/impl2/c.h changes what the compiler reads, and so must change
  // both the stat signature and the content fingerprint (the first link's own text and stat stay the same)
  opticc::util::mkdir_p(d + "/inc/impl1"); opticc::util::mkdir_p(d + "/inc/impl2"); opticc::util::mkdir_p(d + "/alt");
  opticc::util::write_file(d + "/inc/impl1/c.h", "int c1;\n"); opticc::util::write_file(d + "/inc/impl2/c.h", "int c2;\n");
  CHECK(::symlink((d + "/inc/impl1/c.h").c_str(), (d + "/alt/c.h").c_str()) == 0);
  CHECK(::symlink((d + "/alt/c.h").c_str(), (d + "/inc/c.h").c_str()) == 0);
  const std::string s2 = opticc::sysinc::stat_signature(dirs), c2 = opticc::sysinc::content_fingerprint(dirs, &memo);
  ::unlink((d + "/alt/c.h").c_str());
  CHECK(::symlink((d + "/inc/impl2/c.h").c_str(), (d + "/alt/c.h").c_str()) == 0);
  CHECK(opticc::sysinc::stat_signature(dirs) != s2); CHECK(opticc::sysinc::content_fingerprint(dirs, &memo) != c2);
  // a fingerprinted header using a time macro is marked ("T:"), others are not
  opticc::util::write_file(d + "/inc/stamp.h", "#define BUILT __TIME__\n");
  opticc::sysinc::content_fingerprint(dirs, &memo);
  CHECK(memo.count(d + "/inc/stamp.h") && memo[d + "/inc/stamp.h"].sha.rfind("T:", 0) == 0);
  CHECK(memo.count(d + "/inc/a.h") && memo[d + "/inc/a.h"].sha.rfind("T:", 0) != 0);
  opticc::util::remove_tree(d);
}

// A time-macro system header refuses only the TUs that may read it (by the -MD dependency file, else by the
// directories the compile searches, -nostdinc honoured), never every TU of the compiler.
OPTICC_TEST(sysinc_time_macro_input_per_tu) {
  namespace si = opticc::sysinc;
  const std::vector<std::string> tf{"/sys/inc/stamp.h"}, cdirs{"/sys/inc", "/res/include"};
  const std::vector<std::string> md{"cc", "-MD", "-c", "f.c"}, mmd{"cc", "-MMD", "-c", "f.c"};
  CHECK(si::time_macro_input({}, cdirs, md, "/w", {"/sys/inc/stamp.h"}, true).empty());                  // nothing marked
  CHECK(si::time_macro_input(tf, cdirs, md, "/w", {"/w/f.c", "/sys/inc/stamp.h"}, true) == "/sys/inc/stamp.h");
  CHECK(si::time_macro_input(tf, cdirs, md, "/w", {"/w/f.c", "/sys/inc/plain.h"}, true).empty());      // -MD: exact
  CHECK(!si::time_macro_input(tf, cdirs, mmd, "/w", {"/w/f.c"}, false).empty());                       // -MMD: searched
  const std::vector<std::string> kern{"cc", "-nostdinc", "-isystem", "/res/include", "-Wp,-MMD,x.d", "-c", "f.c"};
  CHECK(si::time_macro_input(tf, cdirs, kern, "/w", {"/w/f.c"}, false).empty());                       // kbuild: not searched
  const std::vector<std::string> kern2{"cc", "-nostdinc", "-isystem/sys/inc", "-MMD", "-c", "f.c"};
  CHECK(!si::time_macro_input(tf, cdirs, kern2, "/w", {"/w/f.c"}, false).empty());                     // searched again
  const std::vector<std::string> sr{"cc", "--sysroot=/x", "-MD", "-c", "f.c"};
  CHECK(!si::time_macro_input(tf, cdirs, sr, "/w", {"/w/f.c"}, true).empty());                          // conservative
  CHECK(si::explicit_system_dirs({"cc", "-isystem", "inc", "-idirafter/a", "-isystem-after", "/b"}, "/w") ==
        std::vector<std::string>({"/w/inc", "/a", "/b"}));
}

// System-include fingerprints. The stat signature changes on any edit (even a same-content
// rewrite); the content fingerprint changes only with content; peek() never blocks and becomes ready in the background.
OPTICC_TEST(sysinc_signatures_and_tracker) {
  const std::string d = opticc::util::make_temp_dir("opticc-sysinc-t-");
  opticc::util::mkdir_p(d + "/inc/sub");
  opticc::util::write_file(d + "/inc/a.h", "int a;\n"); opticc::util::write_file(d + "/inc/sub/b.h", "int b;\n");
  const std::vector<std::string> dirs{d + "/inc"};
  const std::string s0 = opticc::sysinc::stat_signature(dirs);
  std::map<std::string, opticc::sysinc::FileSig> memo;
  const std::string c0 = opticc::sysinc::content_fingerprint(dirs, &memo);
  CHECK(memo.size() == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  opticc::util::write_file(d + "/inc/sub/b.h", "int b;\n");                  // same content, new stat
  CHECK(opticc::sysinc::stat_signature(dirs) != s0);
  CHECK(opticc::sysinc::content_fingerprint(dirs, &memo) == c0);
  opticc::util::write_file(d + "/inc/sub/b.h", "int b2;\n");                 // new content
  CHECK(opticc::sysinc::content_fingerprint(dirs, &memo) != c0);
  opticc::util::write_file(d + "/inc/new.h", "int n;\n");                    // a new file appears
  const std::string s1 = opticc::sysinc::stat_signature(dirs);
  CHECK(s1 != s0);
  // the tracker: get() computes, peek() is non-blocking and reports readiness
  opticc::sysinc::Tracker tr(d + "/memo", 0.2);
  opticc::sysinc::State st = tr.get("cc", true);
  CHECK(st.ready); CHECK(st.stat_sig.size() == 64); CHECK(st.content_fp.size() == 64);
  CHECK(tr.peek("cc", true).content_fp == st.content_fp);
  // a compiler whose search list cannot be parsed is "unknown": never ready (nothing would be cached against it)
  CHECK(!tr.peek("/nonexistent/compiler", false).ready);
  const std::string cxx = opticc::cachekey::resolve_compiler("c++");
  if (cxx.find('/') == std::string::npos) { std::printf("  skipped: no c++\n"); return; }
  opticc::sysinc::State p = tr.peek(cxx, false);
  CHECK(!p.ready);
  bool ready = false;
  // liveness, not timing: the tracker thread runs at background QoS and a loaded -j8 suite can delay it for seconds
  for (int i = 0; i < 2400 && !ready; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); ready = tr.peek(cxx, false).ready; }
  CHECK(ready);
  CHECK(opticc::direct::with_sysinc("k", "s1") != opticc::direct::with_sysinc("k", "s2"));
  CHECK(opticc::direct::with_sysinc("k", "s1") == opticc::direct::with_sysinc("k", "s1"));
}

// Probe classification refuses host-dependent and external-assembler shapes; PATH is part
// of the probe environment; the implicit-input identity is known for the local compiler.
OPTICC_TEST(probe_premises) {
  using opticc::probe::classify;
  CHECK(!classify({"/usr/bin/clang", "-march=native", "-c", "-x", "c", "/dev/null", "-o", "/tmp/x"}).ok);
  CHECK(!classify({"/usr/bin/clang", "-mtune=native", "-c", "-x", "c", "/dev/null", "-o", "/tmp/x"}).ok);
  CHECK(!classify({"/usr/bin/clang", "-fno-integrated-as", "-c", "-x", "c", "/dev/null", "-o", "/tmp/x"}).ok);
  CHECK(classify({"/usr/bin/clang", "-c", "-x", "c", "/dev/null", "-o", "/tmp/x"}).ok);
  bool has_path = false; for (auto& e : opticc::probe::env()) if (e.rfind("PATH=", 0) == 0) has_path = true;
  CHECK(has_path);
  CHECK(opticc::probe::lang_of({"cc", "-x", "assembler-with-cpp", "-"}) == "assembler-with-cpp");
  CHECK(opticc::probe::lang_of({"cc", "-xc++", "-"}) == "c++");
  const std::string m = opticc::util::make_temp_dir("opticc-pimp-");
  const std::string real = opticc::cachekey::resolve_compiler("cc");
  if (real.find('/') == std::string::npos) { std::printf("  skipped: no cc\n"); return; }
  const std::string i1 = opticc::probe::implicit_identity(real, "c", m);
  CHECK(i1.size() == 64);
  CHECK(opticc::probe::implicit_identity(real, "c", m) == i1);   // memoised and stable
  CHECK(opticc::probe::implicit_identity(real, "assembler", m) == "none");
  // the local driver's plan decides: clang assembles in-process for C; the answer is memoised
  const bool ext = opticc::probe::external_assembler(real, "c", m);
  CHECK(opticc::probe::external_assembler(real, "c", m) == ext);
}
