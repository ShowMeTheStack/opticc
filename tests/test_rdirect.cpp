// SPDX-License-Identifier: GPL-2.0-only
// Shared direct manifests (opticc/rdirect.hpp): format, wire, store, content validation, fingerprint.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "opticc/direct.hpp"
#include "opticc/rdirect.hpp"
#include "opticc/util.hpp"
#include "test_harness.hpp"

namespace rd = opticc::rdirect;
namespace dr = opticc::direct;
namespace ut = opticc::util;
namespace fs = std::filesystem;

static rd::PPos ppos(const std::string& p) { return rd::PPos{p, ut::sha256_hex(ut::read_file(p)), ut::file_size(p)}; }
static void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(700)); }   // > margin (OPTICC_RACY_S=0.5)

static std::string find_clang() {
  const char* path = std::getenv("PATH");
  std::string p = path ? path : "/usr/bin";
  size_t i = 0;
  while (i <= p.size()) {
    size_t j = p.find(':', i); if (j == std::string::npos) j = p.size();
    std::string c = p.substr(i, j - i) + "/clang";
    if (::access(c.c_str(), X_OK) == 0) { std::error_code ec; auto r = fs::canonical(c, ec); return ec ? c : r.string(); }
    i = j + 1;
  }
  return {};
}

OPTICC_TEST(rdirect_encode_decode_and_wire) {
  rd::PEntry e; e.obj = std::string(64, 'a'); e.obj_sha = std::string(64, 'b'); e.keysha = true; e.recorded_wall = 12.5;
  e.pos.push_back(rd::PPos{"/x/a.h", "n:" + std::string(64, 'c'), 7}); e.neg.push_back(rd::PNeg{"/x/inc", {"a.h", "b.h"}});
  std::vector<rd::PEntry> out;
  CHECK(rd::decode(rd::encode({e, e}), &out) && out.size() == 2);
  CHECK(out[1].obj == e.obj && out[1].obj_sha == e.obj_sha && out[1].keysha && out[1].pos[0].size == 7 && out[1].neg[0].names[1] == "b.h");
  std::string bad = rd::encode({e}); bad.pop_back();
  CHECK(!rd::decode(bad, &out));
  rd::PutMsg pm; pm.pkey = std::string(64, 'd'); pm.entry = e; pm.obj.okey = e.obj; pm.obj.obj_sha = e.obj_sha; pm.obj.object = "OBJ";
  pm.obj.depfile_body = " a.h"; pm.obj.err = "warn"; pm.obj.rc = 0;
  rd::PutMsg pm2 = rd::decode_put(rd::encode_put(pm));
  CHECK(pm2.pkey == pm.pkey && pm2.entry.obj == e.obj && pm2.obj.object == "OBJ" && pm2.obj.err == "warn" && pm2.obj.depfile_body == " a.h");
  rd::GetMsg gm2 = rd::decode_get(rd::encode_get(rd::GetMsg{42, pm.pkey, ""}));
  CHECK(gm2.id == 42 && gm2.pkey == pm.pkey && gm2.okey.empty());
  rd::ReplyMsg rm; rm.id = 42; rm.have = true; rm.entries = rd::encode({e}); rm.obj = pm.obj;
  rd::ReplyMsg rm2 = rd::decode_reply(rd::encode_reply(rm));
  CHECK(rm2.id == 42 && rm2.have && rm2.entries == rm.entries && rm2.obj.object == "OBJ");
  rd::ReplyMsg empty; empty.id = 7;
  rd::ReplyMsg e2 = rd::decode_reply(rd::encode_reply(empty));
  CHECK(e2.id == 7 && !e2.have && e2.entries.empty() && e2.obj.object.empty());
}

// Two publishers of one object key whose compiles produced different bytes. The worker keeps BOTH objects (key =
// object key + sha) and a GET for "okey@sha" serves the entry with those bytes. A worker that kept only the first
// object and served it for every entry of that key would make the later publisher's entries fail the client's sha
// check forever.
OPTICC_TEST(rdirect_two_publishers_one_object_key) {
  const std::string ok(64, 'a'), s1(64, '1'), s2(64, '2');
  CHECK(rd::worker_object_key(ok, s1) != rd::worker_object_key(ok, s2));
  std::vector<rd::PEntry> es(3);
  es[0].obj = ok; es[0].obj_sha = s1; es[1].obj = std::string(64, 'b'); es[1].obj_sha = s1; es[2].obj = ok; es[2].obj_sha = s2;
  CHECK(rd::select_for_get(es, "") == &es[2]);                 // the newest
  CHECK(rd::select_for_get(es, ok) == &es[2]);                 // the newest with that key
  CHECK(rd::select_for_get(es, ok + "@" + s1) == &es[0]);      // by key and bytes
  CHECK(rd::select_for_get(es, ok + "@" + s2) == &es[2]);
  CHECK(rd::select_for_get(es, ok + "@" + std::string(64, '3')) == nullptr);
  CHECK(rd::select_for_get({}, "") == nullptr);
}

OPTICC_TEST(rdirect_pkey_and_env) {
  const std::vector<std::string> a{"/usr/bin/clang", "-c", "a.c", "-o", "a.o"};
  const std::string k = rd::pkey("F", a, "/w", {}, "s");
  CHECK(k.size() == 64);
  CHECK(k != rd::pkey("G", a, "/w", {}, "s"));
  CHECK(k != rd::pkey("F", {"/usr/bin/clang", "-c", "a.c", "-o", "b.o"}, "/w", {}, "s"));
  CHECK(k != rd::pkey("F", a, "/v", {}, "s"));
  CHECK(k != rd::pkey("F", a, "/w", {"TZ=UTC"}, "s"));
  CHECK(k != rd::pkey("F", a, "/w", {}, "t"));
  CHECK(rd::env_ok({}) && rd::env_ok({"SOURCE_DATE_EPOCH=1", "TZ=UTC"}));
  CHECK(!rd::env_ok({"CPATH=/x"}) && !rd::env_ok({"C_INCLUDE_PATH=/y"}) && !rd::env_ok({"COMPILER_PATH=/z"}));
  auto s = rd::system_search_args({"clang", "-nostdinc", "-I", "inc", "-isystem", "/r/include", "-m64", "-O2", "--sysroot=/s", "-c", "a.c"});
  CHECK((s == std::vector<std::string>{"-nostdinc", "-isystem", "/r/include", "-m64", "--sysroot=/s"}));
}

// positives are validated by CONTENT only: another machine's stat says nothing (new inode, new times: still valid);
// a content change, a size change or a vanished file is a miss; a key-hash entry ignores comment-only edits
OPTICC_TEST(rdirect_validator_content) {
  setenv("OPTICC_RACY_S", "0.5", 1);
  std::string d = ut::make_temp_dir("opticc-rdv-");
  ut::write_file(d + "/a.h", "int a;\n"); ut::write_file(d + "/b.h", "int b;\n");
  rd::PEntry e; e.obj = "o"; e.pos = {ppos(d + "/a.h"), ppos(d + "/b.h")};
  // "the other machine": same paths and bytes, different inodes and times
  ut::write_file(d + "/a.h.new", "int a;\n"); std::rename((d + "/a.h.new").c_str(), (d + "/a.h").c_str());
  settle();
  rd::Validator v; std::string why; dr::Entry local;
  CHECK(v.valid(e, &why, &local));
  CHECK(local.obj == "o" && local.pos.size() == 2 && local.pos[0].st.ino == dr::stat_path(d + "/a.h").ino);   // settled: recordable locally
  CHECK(v.valid(e, &why));                                     // memo hits now
  ut::write_file(d + "/b.h", "int c;\n"); settle();            // same size, other bytes
  CHECK(!v.valid(e, &why) && why.find("positive content") == 0);
  ut::write_file(d + "/b.h", "int bb;\n"); settle();
  CHECK(!v.valid(e, &why) && why.find("positive size") == 0);
  std::remove((d + "/b.h").c_str());
  CHECK(!v.valid(e, &why) && why.find("positive gone") == 0);
  ut::write_file(d + "/b.h", "int b;\n");                       // restored, but younger than the margin
  CHECK(v.valid(e, &why, &local));
  CHECK(local.obj.empty());                                     // valid now, not recordable as a stat-trusting entry
  // fsha (exact-bytes BLAKE2b, published when provably the recorded bytes): compared first; a stale fsha falls back to sha
  {
    ut::write_file(d + "/f.h", "int f;\n"); settle();
    rd::PEntry f; f.obj = "f"; f.pos.push_back(ppos(d + "/f.h"));
    f.pos[0].fsha = v.hash_of(d + "/f.h", dr::stat_path(d + "/f.h"), rd::Validator::Fast);
    CHECK(f.pos[0].fsha.size() == 64 && f.pos[0].fsha != f.pos[0].sha);
    CHECK(v.valid(f, &why));
    rd::PEntry g = f; g.pos[0].fsha = std::string(64, '0');      // stale fast hash, right sha: valid through the fallback
    CHECK(v.valid(g, &why));
    ut::write_file(d + "/f.h", "int g;\n"); settle();            // other bytes: both hashes differ
    CHECK(!v.valid(f, &why) && why.find("positive content") == 0);
  }
  // keysha entries: a comment-only edit keeps the key hash
  rd::PEntry k; k.obj = "k"; k.keysha = true;
  k.pos.push_back(rd::PPos{d + "/a.h", dr::content_sha("int a;\n", true), 0});
  ut::write_file(d + "/a.h", "int a; /* comment */\n"); settle();
  CHECK(v.valid(k, &why));
  ut::write_file(d + "/a.h", "int A;\n"); settle();
  CHECK(!v.valid(k, &why));
  ut::remove_tree(d);
}

// negatives: every probed name must be absent; a shadowing header created after the listing was memoised is found
// (the directory's stat changes), and so is one created in a directory that did not exist
OPTICC_TEST(rdirect_validator_negatives) {
  setenv("OPTICC_RACY_S", "0.5", 1);
  std::string d = ut::make_temp_dir("opticc-rdn-");
  ut::mkdir_p(d + "/inc1"); ut::mkdir_p(d + "/inc2");
  ut::write_file(d + "/inc2/x.h", "int x;\n"); ut::write_file(d + "/inc1/other.h", "\n");
  rd::PEntry e; e.obj = "o"; e.pos = {ppos(d + "/inc2/x.h")};
  e.neg.push_back(rd::PNeg{d + "/inc1", {"x.h", "y.h"}});
  e.neg.push_back(rd::PNeg{d + "/gone", {"x.h"}});
  settle();
  rd::Validator v; std::string why;
  CHECK(v.valid(e, &why));
  CHECK(v.valid(e, &why));                                       // from the listing memo (Linux)
  ut::write_file(d + "/inc1/x.h", "int shadow;\n");              // shadows inc2/x.h
  CHECK(!v.valid(e, &why) && why.find("shadowing candidate") == 0);
  std::remove((d + "/inc1/x.h").c_str()); settle();
  CHECK(v.valid(e, &why));
  ut::mkdir_p(d + "/gone"); ut::write_file(d + "/gone/x.h", "\n");   // a directory that did not exist appears
  CHECK(!v.valid(e, &why));
  ut::remove_tree(d);
}

OPTICC_TEST(rdirect_store_merge) {
  std::string d = ut::make_temp_dir("opticc-rds-");
  rd::Store st; st.init(d + "/rd");
  const std::string pk(64, 'e');
  CHECK(!st.put("../escape", rd::PEntry{}));                     // keys are 64 hex digits only
  for (int i = 0; i < 10; ++i) { rd::PEntry e; e.obj = "o" + std::to_string(i); e.pos.push_back(rd::PPos{"/p", "s", 1}); CHECK(st.put(pk, e)); }
  { rd::PEntry e; e.obj = "o3"; e.pos.push_back(rd::PPos{"/p", "s2", 1}); CHECK(st.put(pk, e)); }   // re-published: moves to newest
  std::string enc; std::vector<rd::PEntry> es;
  CHECK(st.get(pk, &enc) && rd::decode(enc, &es));
  CHECK(es.size() == rd::kMaxEntries && es.back().obj == "o3" && es.back().pos[0].sha == "s2");
  CHECK(st.count() == 1);
  rd::Store st2; st2.init(d + "/rd");                            // keys survive a restart (HELLO Bloom filter)
  CHECK(st2.count() == 1 && st2.keys()[0] == pk);
  CHECK(!st.get(std::string(64, 'f'), &enc));
  CHECK(rd::worker_object_key("o3", "s") != "o3" && rd::worker_object_key("o3", "s").size() == 64);
  ut::remove_tree(d);
}

// the fingerprint is the toolchain identity (compiler id + system-include content fingerprint) plus this compile's
// system search; under -MMD every searched system directory must lie inside the fingerprinted (sysinc) directories
OPTICC_TEST(rdirect_fingerprint) {
  setenv("OPTICC_RACY_S", "0.5", 1);
  const std::string cc = find_clang();
  if (cc.empty()) { std::printf("  (no clang on PATH: skipped)\n"); return; }
  std::string d = ut::make_temp_dir("opticc-rdf-");
  { std::error_code ec; d = fs::canonical(d, ec).string(); }
  ut::mkdir_p(d + "/sys"); ut::write_file(d + "/sys/s.h", "int s;\n");
  const std::vector<std::string> argv{cc, "-nostdinc", "-isystem", d + "/sys", "-I", d, "-c", "a.c", "-o", "a.o"};
  const std::vector<std::string> covered{d};          // the sysinc directories cover d/sys
  std::string why;
  rd::Toolchain t1;
  const std::string md = t1.fingerprint(cc, argv, "c", true, "cid", "fp", covered, &why);
  CHECK(!md.empty());
  if (md.empty()) std::printf("  why: %s\n", why.c_str());
  const std::string mmd = t1.fingerprint(cc, argv, "c", false, "cid", "fp", covered, &why);
  CHECK(!mmd.empty() && mmd != md);
  CHECK(t1.fingerprint(cc, argv, "c", false, "cid", "fp", covered, &why) == mmd);           // memoised
  CHECK(t1.fingerprint(cc, argv, "c", false, "cid", "fp2", covered, &why) != mmd);          // system headers changed
  CHECK(t1.fingerprint(cc, argv, "c", false, "cid2", "fp", covered, &why) != mmd);          // other compiler bytes
  CHECK(t1.fingerprint(cc, argv, "c", false, "", "fp", covered, &why).empty());             // identity unavailable
  CHECK(t1.fingerprint(cc, argv, "c", false, "cid", "fp", {d + "/other"}, &why).empty());   // -MMD, directory not covered
  CHECK(why.find("outside the system-include fingerprint") != std::string::npos);
  CHECK(!t1.fingerprint(cc, argv, "c", true, "cid", "fp", {d + "/other"}, &why).empty());   // -MD: positives cover it
  rd::Toolchain t3;
  CHECK(t3.fingerprint(cc, {cc, "-nostdinc", "-c", "a.c"}, "c", true, "cid", "fp", covered, &why) != md);   // other search
  ut::remove_tree(d);
}

// Concurrent validations of entries sharing a settled header digest it ONCE (the others wait for that
// digest); an edited header, a new shadowing header and a changed "system" header (any positive outside the project)
// still miss under concurrency; an UNSETTLED file is never shared (each reader digests it itself).
OPTICC_TEST(rdirect_validator_concurrent_once_and_still_misses) {
  setenv("OPTICC_RACY_S", "0.5", 1);
  std::string d = ut::make_temp_dir("opticc-rdc-");
  ut::mkdir_p(d + "/inc"); ut::mkdir_p(d + "/sys"); ut::mkdir_p(d + "/shadow");
  std::string big(4 << 20, 'x');                                  // a large header: digests overlap in time
  ut::write_file(d + "/inc/big.h", big); ut::write_file(d + "/sys/sys.h", "int sys;\n"); ut::write_file(d + "/inc/x.h", "int x;\n");
  settle();
  rd::Validator rec;
  auto pos = [&](const std::string& f) {
    rd::PPos p = ppos(f); p.fsha = rec.hash_of(f, dr::stat_path(f), rd::Validator::Fast); return p; };
  rd::PEntry e; e.obj = "o"; e.pos = {pos(d + "/inc/big.h"), pos(d + "/sys/sys.h"), pos(d + "/inc/x.h")};
  e.neg.push_back(rd::PNeg{d + "/shadow", {"x.h"}});
  auto run = [&](rd::Validator& v, int n) {
    std::vector<std::thread> th; std::atomic<int> ok{0};
    for (int i = 0; i < n; ++i) th.emplace_back([&] { std::string why; if (v.valid(e, &why)) ok++; });
    for (auto& t : th) t.join();
    return ok.load();
  };
  {
    rd::Validator v;
    CHECK(run(v, 16) == 16);
    std::printf("  16 concurrent: fast digests %llu (distinct %llu), shared waits %llu\n", (unsigned long long)v.n_digest[rd::Validator::Fast].load(),
                (unsigned long long)v.distinct_files(), (unsigned long long)v.shared_waits.load());
    CHECK(v.n_digest[rd::Validator::Fast].load() == 3);           // one digest per file, not per validation
  }
  {
    rd::Validator v;
    ut::write_file(d + "/inc/x.h", "int y;\n"); settle();        // edited header
    CHECK(run(v, 8) == 0);
    ut::write_file(d + "/inc/x.h", "int x;\n"); settle();
    CHECK(run(v, 8) == 8);
    ut::write_file(d + "/shadow/x.h", "int x;\n"); settle();     // new shadowing header (negative entry)
    CHECK(run(v, 8) == 0);
    std::remove((d + "/shadow/x.h").c_str()); settle();
    CHECK(run(v, 8) == 8);
    ut::write_file(d + "/sys/sys.h", "int SYS;\n"); settle();    // changed system header
    CHECK(run(v, 8) == 0);
  }
  {
    rd::Validator v;                                              // unsettled (just written): never shared
    ut::write_file(d + "/sys/sys.h", "int sys;\n");
    std::vector<std::thread> th;
    for (int i = 0; i < 8; ++i) th.emplace_back([&] { v.hash_of(d + "/sys/sys.h", dr::stat_path(d + "/sys/sys.h"), rd::Validator::Fast); });
    for (auto& t : th) t.join();
    CHECK(v.shared_waits.load() == 0);
    CHECK(v.n_digest[rd::Validator::Fast].load() == 8);
  }
  ut::remove_tree(d);
}

// Absent names with '/' (LLVM: "llvm/ADT/X.h" probed in every -I directory) are decided component by
// component from memoised listings instead of one access() each. They must still see: a shadowing header created in a
// subdirectory that already existed, in one created after the listing was memoised, and in a parent reached through
// "..", while a dangling symlink stays absent (as access() decided before).
OPTICC_TEST(rdirect_validator_slash_names) {
  setenv("OPTICC_RACY_S", "0.5", 1);
  std::string d = ut::make_temp_dir("opticc-rds-");
  ut::mkdir_p(d + "/inc1"); ut::mkdir_p(d + "/inc2/llvm/ADT"); ut::mkdir_p(d + "/inc1/llvm");
  ut::write_file(d + "/inc2/llvm/ADT/X.h", "int x;\n"); ut::write_file(d + "/inc2/up.h", "int up;\n");
  rd::PEntry e; e.obj = "o"; e.pos = {ppos(d + "/inc2/llvm/ADT/X.h")};
  e.neg.push_back(rd::PNeg{d + "/inc1", {"llvm/ADT/X.h", "llvm/Other/Y.h", "z.h"}});
  settle();
  rd::Validator v; std::string why;
  CHECK(v.valid(e, &why));
  CHECK(v.valid(e, &why));                                          // memoised listings
#ifndef __APPLE__
  std::printf("  neg decided by listing %llu, by access %llu\n", (unsigned long long)v.neg_listed.load(), (unsigned long long)v.neg_access.load());
  CHECK(v.neg_listed.load() >= 6 && v.neg_access.load() == 0);    // no access() for the absent names
#endif
  ut::mkdir_p(d + "/inc1/llvm/ADT"); settle();                     // an intermediate directory appears: still absent
  CHECK(v.valid(e, &why));
  ut::write_file(d + "/inc1/llvm/ADT/X.h", "int shadow;\n");       // shadows inc2/llvm/ADT/X.h (memoised parent listings)
  CHECK(!v.valid(e, &why) && why.find("shadowing candidate") == 0);
  settle();                                                          // now from a settled, memoised listing
  CHECK(!v.valid(e, &why) && why.find("shadowing candidate") == 0);
  std::remove((d + "/inc1/llvm/ADT/X.h").c_str()); settle();
  CHECK(v.valid(e, &why));
  ut::mkdir_p(d + "/inc1/llvm/Other"); ut::write_file(d + "/inc1/llvm/Other/Y.h", "\n");   // created right away (unsettled)
  CHECK(!v.valid(e, &why));
  std::remove((d + "/inc1/llvm/Other/Y.h").c_str()); settle();
  CHECK(v.valid(e, &why));
  // a dangling symlink at the probed path: absent for the preprocessor (and for access()), so still valid
  CHECK(::symlink((d + "/nowhere.h").c_str(), (d + "/inc1/llvm/ADT/X.h").c_str()) == 0); settle();
  CHECK(v.valid(e, &why));
  std::remove((d + "/inc1/llvm/ADT/X.h").c_str()); settle();
  // ".." in a probed name: resolved by the kernel, never by a listing
  rd::PEntry u = e; u.neg = {rd::PNeg{d + "/inc1", {"../inc2/up.h"}}};
  CHECK(!v.valid(u, &why));                                         // it exists: a shadowing candidate
  u.neg = {rd::PNeg{d + "/inc1", {"../inc2/none.h"}}};
  CHECK(v.valid(u, &why));
  ut::remove_tree(d);
}

// Names probed in a directory that does not exist (most negatives of the LLVM and kernel trees) are
// decided once per group from the nearest existing ancestor's pinned listing, not by one access() per name. The
// directory (or one of its parents) appearing later, with or without the header, must still be seen.
OPTICC_TEST(rdirect_validator_missing_dir_negatives) {
  setenv("OPTICC_RACY_S", "0.5", 1);
  std::string d = ut::make_temp_dir("opticc-rdm-");
  ut::mkdir_p(d + "/inc2"); ut::write_file(d + "/inc2/x.h", "int x;\n");
  rd::PEntry e; e.obj = "o"; e.pos = {ppos(d + "/inc2/x.h")};
  e.neg.push_back(rd::PNeg{d + "/gone/deeper", {"x.h", "y.h", "z.h"}});
  settle();
  rd::Validator v; std::string why;
  CHECK(v.valid(e, &why));
  CHECK(v.valid(e, &why));
#ifndef __APPLE__
  std::printf("  missing dir: decided by listing %llu, by access %llu\n", (unsigned long long)v.neg_listed.load(), (unsigned long long)v.neg_access.load());
  CHECK(v.neg_listed.load() == 6 && v.neg_access.load() == 0);
#endif
  ut::mkdir_p(d + "/gone"); settle();                              // the parent appears: still no candidate
  CHECK(v.valid(e, &why));
  ut::mkdir_p(d + "/gone/deeper"); ut::write_file(d + "/gone/deeper/y.h", "int shadow;\n");   // fresh: unsettled
  CHECK(!v.valid(e, &why) && why.find("shadowing candidate") == 0);
  settle();                                                          // settled and memoised
  CHECK(!v.valid(e, &why) && why.find("shadowing candidate") == 0);
  ut::remove_tree(d + "/gone"); settle();
  CHECK(v.valid(e, &why));
  ut::write_file(d + "/gone", "not a directory\n"); settle();       // a FILE in the parent's place: no candidate below it
  CHECK(v.valid(e, &why));
  std::remove((d + "/gone").c_str());
  ut::mkdir_p(d + "/gone/deeper"); ut::write_file(d + "/gone/deeper/z.h", "\n"); settle();
  CHECK(!v.valid(e, &why));
  ut::remove_tree(d);
}
