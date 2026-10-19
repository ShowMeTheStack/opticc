// Unit tests for protocol robustness and security.
// SPDX-License-Identifier: GPL-2.0-only
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "opticc/protocol.hpp"
#include "opticc/servescope.hpp"
#include "opticc/transport.hpp"
#include "opticc/util.hpp"
#include "test_harness.hpp"

using namespace opticc;

// A declared length above OPTICC_MAX_BLOB_MB is refused BEFORE allocation (the message proves which check fired:
// without the bound the decoder would allocate the declared size and fail later with "length mismatch").
OPTICC_TEST(hard_zstd_declared_length_bounded) {
  const std::string z = proto::zstd_compress("hello world", 3);
  CHECK(proto::zstd_decompress(z, 11) == "hello world");
  std::string w;
  try { proto::zstd_decompress(z, proto::max_blob_bytes() + 1); } catch (const std::exception& e) { w = e.what(); }
  CHECK(w.find("exceeds") != std::string::npos);
  // a forged length within the bound but different from the frame's recorded content size is refused before allocation
  w.clear();
  try { proto::zstd_decompress(z, 64u << 20); } catch (const std::exception& e) { w = e.what(); }
  CHECK(w.find("does not match the frame") != std::string::npos);
  // not a zstd frame
  w.clear();
  try { proto::zstd_decompress(std::string("garbage-garbage"), 5); } catch (const std::exception& e) { w = e.what(); }
  CHECK(!w.empty());
}

// BLOBS with a forged per-blob or total raw length is refused before allocation.
OPTICC_TEST(hard_blobs_total_bounded) {
  ::setenv("OPTICC_MAX_BLOB_MB", "16", 1);   // this process only (one test per process); read once, on first use
  CHECK(proto::max_blob_bytes() == (16u << 20));
  proto::BlobsMsg m; m.id = 7;
  m.blobs.push_back({"sha-a", std::string(1000, 'a')});
  proto::Frame f = proto::encode_blobs(m, 3, nullptr);
  proto::BlobsMsg d = proto::decode_blobs(f);
  CHECK(d.blobs.size() == 1 && d.blobs[0].second.size() == 1000);
  // forge raw_len to just above the bound
  proto::json h; std::string data; proto::unpack(f.payload, &h, &data);
  h["blobs"][0]["raw_len"] = proto::max_blob_bytes() + 7;
  proto::Frame g{proto::Type::Blobs, proto::pack(h, data)};
  std::string w;
  try { proto::decode_blobs(g); } catch (const std::exception& e) { w = e.what(); }
  CHECK(w.find("exceeds") != std::string::npos);
  // a zstd_len pointing past the data is "truncated", not an out-of-bounds read
  h["blobs"][0]["raw_len"] = 1000; h["blobs"][0]["zstd_len"] = data.size() + 1;
  proto::Frame t{proto::Type::Blobs, proto::pack(h, data)};
  w.clear();
  try { proto::decode_blobs(t); } catch (const std::exception& e) { w = e.what(); }
  CHECK(w.find("truncated") != std::string::npos);
  // genuine blobs, each under the bound, whose SUM exceeds it: refused by the total check (12 + 12 MiB > 16 MiB)
  proto::BlobsMsg big; big.id = 8;
  big.blobs.push_back({"sha-b", std::string(12u << 20, 'b')});
  big.blobs.push_back({"sha-c", std::string(12u << 20, 'c')});
  proto::Frame bf = proto::encode_blobs(big, 1, nullptr);
  big.blobs.clear();
  w.clear();
  try { proto::decode_blobs(bf); } catch (const std::exception& e) { w = e.what(); }
  CHECK(w.find("total declared length exceeds") != std::string::npos);
}

// An unauthenticated peer cannot make read_frame allocate more than kPreAuthFrame.
OPTICC_TEST(hard_frame_limit_pre_auth) {
  int sv[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  net::Socket rd(sv[0]);
  net::Socket wr(sv[1]);
  rd.set_max_frame(net::Socket::kPreAuthFrame);
  std::string hdr; proto::put_u32(hdr, 100u << 20); hdr.push_back(static_cast<char>(proto::Type::Hello));
  wr.send_all(hdr.data(), hdr.size());
  wr.close();
  std::string w;
  try { rd.read_frame(); } catch (const std::exception& e) { w = e.what(); }
  CHECK(w.find("bad frame length") != std::string::npos);
  CHECK(net::Socket::default_max_frame() >= (64u << 20));
}

// Bounded deterministic fuzz of every frame decoder. A decoder may throw std::exception on malformed input but
// must never crash or allocate beyond the bound (the bound is lowered to 16 MiB for this process so that mutated
// length fields cannot make the test itself allocate much).
OPTICC_TEST(hard_decoder_fuzz_bounded) {
  ::setenv("OPTICC_MAX_BLOB_MB", "16", 1);
  CHECK(proto::max_blob_bytes() == (16u << 20));
  std::vector<proto::Frame> seeds;
  { proto::CompileRequest r; r.id = 1; r.argv = {"cc", "-c", "a.c"}; r.preprocessed = std::string(5000, 'x'); seeds.push_back(proto::encode_compile(r, 3, nullptr)); }
  { proto::CompileResult r; r.id = 2; r.rc = 0; r.object = std::string(3000, 'o'); r.err = std::string(9000, 'e'); seeds.push_back(proto::encode_result(r, 3)); }
  { proto::HpRequest r; r.id = 3; r.argv = {"cc", "-c", "b.c"}; r.source = "int x;"; r.deps.push_back({"/p/h.h", "sha", 3}); seeds.push_back(proto::encode_compile_hp(r, 3, nullptr)); }
  { proto::BlobsMsg m; m.id = 4; m.blobs.push_back({"s1", std::string(800, 'b')}); m.blobs.push_back({"s2", std::string(10, 'c')}); seeds.push_back(proto::encode_blobs(m, 3, nullptr)); }
  { proto::MissingMsg m{5, "x.h", "/p/a.c", 1}; seeds.push_back(proto::encode_missing(m)); }
  { proto::NeedMsg m{6, {"a", "b"}}; seeds.push_back(proto::encode_need(m)); }
  uint64_t st = 0x9e3779b97f4a7c15ULL;
  auto rnd = [&]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; };
  int decoded = 0, rejected = 0;
  for (int it = 0; it < 6000; ++it) {
    proto::Frame f = seeds[rnd() % seeds.size()];
    std::string& p = f.payload;
    int kind = static_cast<int>(rnd() % 4);
    if (kind == 0 && !p.empty()) { for (int k = 0; k < 4; ++k) p[rnd() % p.size()] = static_cast<char>(rnd()); }
    else if (kind == 1 && !p.empty()) p.resize(rnd() % p.size());
    else if (kind == 2) { size_t pos = p.find("_len\":"); if (pos != std::string::npos) p.insert(pos + 6, std::to_string(rnd() % 4000000000ULL)); }
    else if (kind == 3 && p.size() > 4) { p[rnd() % 4] = static_cast<char>(rnd()); }
    try {
      switch (f.type) {
        case proto::Type::Compile: proto::decode_compile(f, nullptr); break;
        case proto::Type::Result: proto::decode_result(f); break;
        case proto::Type::CompileHp: proto::decode_compile_hp(f, nullptr); break;
        case proto::Type::Blobs: proto::decode_blobs(f); break;
        case proto::Type::Missing: proto::decode_missing(f); break;
        case proto::Type::Need: proto::decode_need(f); break;
        default: break;
      }
      decoded++;
    } catch (const std::exception&) { rejected++; }
  }
  CHECK(decoded + rejected == 6000);
  CHECK(rejected > 1000);   // the mutations really exercise the error paths
}

// Every untrusted client path stays strictly inside the job root after under_root (lexical normalisation with
// POSIX root clamping). A plain concatenation would fail the containment assertions.
OPTICC_TEST(hard_under_root_contains_hostile_paths) {
  const std::string root = "/tmp/opticcd-hp-TEST";
  const char* hostile[] = {"/../../etc/passwd", "/a/../../../etc/shadow", "/x/./../../root/.ssh/authorized_keys", "//..//..//etc",
                           "/a/b/../../../../../../usr/bin/cc", "relative/../../../etc/x", "/../", "/a/.."};
  for (const char* h : hostile) {
    std::string r;
    try { r = util::under_root(root, h); } catch (const std::exception&) { continue; }   // refusing is also safe
    std::string n = util::lexically_normal_abs(r);
    CHECK(n == r);                                   // already normal: no ".." left for a later join to resolve
    CHECK(util::path_within(n, root) && n != root);  // strictly inside the root
  }
  CHECK(util::under_root(root, "/usr/include/./stdio.h") == root + "/usr/include/stdio.h");
  CHECK(util::under_root(root, "/a//b///c") == root + "/a/b/c");
  bool threw = false;
  try { util::under_root(root, "/"); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { util::under_root(root, "/a/.."); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { util::under_root(root, std::string("/a\0b", 4)); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  CHECK(util::lexically_normal_abs("/a/./b/../c") == "/a/c");
  CHECK(util::lexically_normal_abs("/../..") == "/");
}

// The MISSING serving scope. Built over a real temporary tree (canonicalisation needs existing files).
OPTICC_TEST(hard_servescope_rule) {
  const std::string t = util::make_temp_dir("opticc-hardening-scope-");
  for (const char* d : {"/home/.config/opticc", "/home/.ssh", "/proj/src/sub", "/proj/inc", "/proj/other", "/outside"}) util::mkdir_p(t + d);
  util::write_file(t + "/proj/src/sub/a.c", "int a;\n");
  util::write_file(t + "/proj/inc/x.h", "#define X\n");
  util::write_file(t + "/proj/other/y.h", "#define Y\n");      // reachable by a quote include "../../other/y.h"
  util::write_file(t + "/outside/z.h", "#define Z\n");
  util::write_file(t + "/home/.config/opticc/client.key", "secret\n");
  util::write_file(t + "/home/.ssh/id_ed25519", "secret\n");
  CHECK(::symlink((t + "/home/.config/opticc/client.key").c_str(), (t + "/proj/inc/link.h").c_str()) == 0);   // a link inside the project
  ::setenv("HOME", (t + "/home").c_str(), 1);
  ::unsetenv("OPTICC_KEY_DIR");
  auto s = servescope::build({"cc", "-I../../inc", "-c", "a.c"}, t + "/proj/src/sub", "a.c", "", {});
  std::string why;
  CHECK(servescope::allowed(s, t + "/proj/inc/x.h", &why));
  CHECK(servescope::allowed(s, t + "/proj/src/sub/a.c", &why));
  CHECK(servescope::allowed(s, t + "/proj/other/y.h", &why));            // project root = common ancestor of src and inc
  CHECK(!servescope::allowed(s, t + "/outside/z.h", &why));
  CHECK(!servescope::allowed(s, t + "/home/.config/opticc/client.key", &why));
  CHECK(!servescope::allowed(s, t + "/home/.ssh/id_ed25519", &why));
  CHECK(!servescope::allowed(s, t + "/proj/inc/link.h", &why));         // the link resolves into a denied directory
  CHECK(why.find("denied") != std::string::npos);
  CHECK(!servescope::allowed(s, t + "/proj/inc", &why));                // not a regular file
  CHECK(!servescope::allowed(s, "/etc/passwd", &why));
  CHECK(servescope::common_ancestor("/a/bc", "/a/b") == "/a");
  CHECK(servescope::common_ancestor("/x", "/y") == "/");
  util::remove_tree(t);
}

// The classic-mode input name can never name a path outside the job directory.
OPTICC_TEST(hard_safe_file_stem) {
  CHECK(util::safe_file_stem("x.c") == "x");
  CHECK(util::safe_file_stem("../../etc/cron.d/evil.c") == "evil");
  CHECK(util::safe_file_stem("/abs/path/k.cpp") == "k");
  CHECK(util::safe_file_stem("..") == "input");
  CHECK(util::safe_file_stem("") == "input");
  CHECK(util::safe_file_stem("dir/") == "input");
  CHECK(util::safe_file_stem(std::string("a\0b.c", 5)) == "input");
}

// A worker answers a request that does not decode with a RESULT for its id when the id is readable (header-framed
// requests and plain-JSON frames alike), and never throws while looking for it.
OPTICC_TEST(hard_frame_id_of_malformed_request) {
  CHECK(proto::frame_id(proto::Frame{proto::Type::Compile, proto::pack(proto::json{{"id", 77}, {"argv", "not-a-list"}}, "zz")}) == 77);
  CHECK(proto::frame_id(proto::Frame{proto::Type::Need, proto::json{{"id", 9}, {"shas", 3}}.dump()}) == 9);
  CHECK(proto::frame_id(proto::Frame{proto::Type::Compile, std::string("\x00\x01garbage", 9)}) == -1);
  CHECK(proto::frame_id(proto::Frame{proto::Type::Compile, proto::pack(proto::json{{"id", "x"}}, "")}) == -1);
  CHECK(proto::frame_id(proto::Frame{proto::Type::Compile, ""}) == -1);
  bool threw = false;
  try { proto::decode_compile(proto::Frame{proto::Type::Compile, proto::pack(proto::json{{"id", 77}, {"argv", "not-a-list"}}, "zz")}, nullptr); }
  catch (const std::exception&) { threw = true; }
  CHECK(threw);   // the case the RESULT is for
}

// The bounded waits return within their limits: an HP session with no event times out, one completed does not;
// a socket read with a receive timeout fails instead of blocking.
OPTICC_TEST(hard_timed_waits) {
  net::WorkerConn::HpSession s;
  proto::Frame ev; bool to = false;
  double t0 = util::now_s();
  CHECK(!s.wait_event_for(&ev, 0.2, &to));
  CHECK(to);
  CHECK(util::now_s() - t0 < 5.0);
  { std::lock_guard<std::mutex> g(s.m); s.done = true; }
  CHECK(!s.wait_event_for(&ev, 5.0, &to));
  CHECK(!to);
  CHECK(net::WorkerConn::attempt_timeout_s() > 0);
  int sv[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  net::Socket rd(sv[0]);
  net::Socket wr(sv[1]);
  rd.set_recv_timeout(0.3);
  t0 = util::now_s();
  bool threw = false; std::string line;
  try { rd.recv_line(&line); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  CHECK(util::now_s() - t0 < 5.0);
}
