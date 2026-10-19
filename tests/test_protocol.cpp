// Unit tests: wire frames (zstd blobs) and compiler argument scanning/rewriting.
// SPDX-License-Identifier: GPL-2.0-only
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctime>
#include <string>
#include <vector>

#include "opticc/argscan.hpp"
#include "opticc/protocol.hpp"
#include "test_harness.hpp"

using namespace opticc;
using V = std::vector<std::string>;

static bool has(const V& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
static int idx(const V& v, const std::string& s) { auto it = std::find(v.begin(), v.end(), s); return it == v.end() ? -1 : static_cast<int>(it - v.begin()); }

OPTICC_TEST(frame_compile_roundtrip) {
  proto::CompileRequest r; r.id = 42; r.argv = {"cc", "-O2", "-c", "x.c", "-o", "x.o"}; r.lang = "c"; r.input_name = "x.c";
  r.preprocessed.assign(200000, 'a'); for (size_t i = 0; i < r.preprocessed.size(); i += 7) r.preprocessed[i] = static_cast<char>('b' + i % 13);
  uint64_t z = 0;
  proto::Frame f = proto::encode_compile(r, 3, &z);
  CHECK(f.type == proto::Type::Compile);
  CHECK(z < r.preprocessed.size() / 4);  // compressible
  uint64_t z2 = 0;
  proto::CompileRequest d = proto::decode_compile(f, &z2);
  CHECK(d.id == 42); CHECK(d.argv == r.argv); CHECK(d.lang == "c"); CHECK(d.input_name == "x.c");
  CHECK(d.preprocessed == r.preprocessed); CHECK(z2 == z);
}

OPTICC_TEST(frame_result_roundtrip) {
  proto::CompileResult r; r.id = 7; r.rc = 0; r.status = "ok"; r.out = "o"; r.err = "warning: x\n";
  r.object = std::string(5000, '\x7f'); r.cpu_s = 0.5; r.wall_s = 0.6; r.queue_s = 0.1; r.in_raw = 10; r.in_zstd = 5;
  proto::Frame f = proto::encode_result(r, 3);
  proto::CompileResult d = proto::decode_result(f);
  CHECK(d.id == 7 && d.rc == 0 && d.status == "ok" && d.err == r.err && d.object == r.object);
  CHECK_NEAR(d.cpu_s, 0.5, 1e-12); CHECK(d.in_raw == 10 && d.in_zstd == 5);
  // empty object (compile error) must round-trip too
  proto::CompileResult e; e.id = 8; e.rc = 1; e.status = "compile-error"; e.err = "error: boom";
  proto::CompileResult de = proto::decode_result(proto::encode_result(e, 3));
  CHECK(de.object.empty() && de.rc == 1 && de.status == "compile-error");
}

OPTICC_TEST(pack_unpack_rejects_garbage) {
  bool threw = false;
  try { proto::json h; proto::unpack("abc", &h, nullptr); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
  threw = false;
  try { proto::json h; std::string p; proto::put_u32(p, 999); p += "xx"; proto::unpack(p, &h, nullptr); } catch (const std::exception&) { threw = true; }
  CHECK(threw);
}

OPTICC_TEST(argscan_basic_c) {
  auto s = args::scan({"cc", "-O2", "-Wall", "-c", "foo.c", "-o", "foo.o"});
  CHECK(s.distributable); CHECK(s.lang == "c"); CHECK(s.source == "foo.c"); CHECK(s.output == "foo.o");
  auto pp = args::preprocess_argv({"cc", "-O2", "-Wall", "-c", "foo.c", "-o", "foo.o"}, s, "/t/foo.i");
  CHECK(!has(pp, "-c")); CHECK(has(pp, "-E")); CHECK(idx(pp, "-o") >= 0 && pp[static_cast<size_t>(idx(pp, "-o")) + 1] == "/t/foo.i");
  CHECK(!has(pp, "-MF"));
  auto rm = args::remote_argv({"cc", "-O2", "-Wall", "-c", "foo.c", "-o", "foo.o"}, s, "/w/foo.i", "/w/foo.o");
  CHECK(rm[0] == "cc"); CHECK(has(rm, "-c")); CHECK(has(rm, "/w/foo.i")); CHECK(!has(rm, "foo.c"));
  CHECK(rm[static_cast<size_t>(idx(rm, "-o")) + 1] == "/w/foo.o"); CHECK(has(rm, "-O2"));
}

OPTICC_TEST(argscan_cxx_deps) {
  V a = {"g++", "-std=c++17", "-Iinclude", "-I", "other", "-DX=1", "-D", "Y", "-isystem", "/usr/x", "-MMD", "-MP", "-c", "src/bar.cpp", "-o", "out/bar.o"};
  auto s = args::scan(a);
  CHECK(s.distributable); CHECK(s.lang == "c++"); CHECK(s.has_md && !s.has_mf && !s.has_mt);
  CHECK(args::preprocessed_suffix(s.lang) == ".ii");
  auto pp = args::preprocess_argv(a, s, "/t/bar.ii");
  int mf = idx(pp, "-MF"); CHECK(mf >= 0 && pp[static_cast<size_t>(mf) + 1] == "out/bar.d");
  int mt = idx(pp, "-MT"); CHECK(mt >= 0 && pp[static_cast<size_t>(mt) + 1] == "out/bar.o");
  CHECK(has(pp, "-MMD") && has(pp, "-Iinclude") && has(pp, "-DX=1"));
  auto rm = args::remote_argv(a, s, "/w/bar.ii", "/w/bar.o");
  CHECK(!has(rm, "-Iinclude") && !has(rm, "other") && !has(rm, "-DX=1") && !has(rm, "Y") && !has(rm, "/usr/x"));
  CHECK(!has(rm, "-MMD") && !has(rm, "-MP") && !has(rm, "src/bar.cpp"));
  CHECK(has(rm, "-std=c++17") && has(rm, "/w/bar.ii") && has(rm, "-c"));
}

OPTICC_TEST(argscan_x_flag_and_explicit_mf) {
  V a = {"clang", "-x", "c", "-c", "weird.src", "-MD", "-MF", "d/w.d", "-MT", "w.o", "-o", "w.o"};
  auto s = args::scan(a);
  CHECK(s.distributable); CHECK(s.lang == "c"); CHECK(s.has_mf && s.has_mt);
  auto pp = args::preprocess_argv(a, s, "/t/w.i");
  CHECK(std::count(pp.begin(), pp.end(), "-MF") == 1);
  auto rm = args::remote_argv(a, s, "/w/w.i", "/w/w.o");
  int x = idx(rm, "-x"); CHECK(x >= 0 && rm[static_cast<size_t>(x) + 1] == "cpp-output");
  CHECK(!has(rm, "-MF") && !has(rm, "d/w.d") && !has(rm, "-MT"));
}

OPTICC_TEST(argscan_rejects) {
  CHECK(!args::scan({"cc", "foo.c", "-o", "prog"}).distributable);          // link
  CHECK(!args::scan({"cc", "-c", "a.c", "b.c"}).distributable);             // two sources
  CHECK(!args::scan({"cc", "-E", "a.c"}).distributable);
  CHECK(!args::scan({"cc", "-c", "foo.S", "-o", "foo.o"}).distributable);   // asm
  CHECK(!args::scan({"cc", "-c", "-", "-o", "x.o"}).distributable);
  CHECK(!args::scan({"cc", "-c", "a.c", "-save-temps"}).distributable);
  CHECK(!args::scan({"cc", "-c", "a.c", "lib.o", "-o", "x.o"}).distributable);
  auto s = args::scan({"cc", "-c", "-MD", "dir/a.c"});
  CHECK(s.distributable && s.output == "a.o");
  auto pp = args::preprocess_argv({"cc", "-c", "-MD", "dir/a.c"}, s, "/t/a.i");
  int mf = idx(pp, "-MF"); CHECK(mf >= 0 && pp[static_cast<size_t>(mf) + 1] == "a.d");
}

// ---- header-push: hashing, dependency files, argv rewriting, include resolution ----------------
#include "opticc/closure.hpp"
#include "opticc/util.hpp"
#include <fstream>

OPTICC_TEST(sha256_known_vectors) {
  CHECK(util::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(util::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56-byte message exercises the two-block padding path
  CHECK(util::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::string million(1000000, 'a');
  CHECK(util::sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

OPTICC_TEST(depfile_parse) {
  std::string d = "out/f1.o: f1.c inc/shared.h \\\n  /usr/include/stdio.h \\\n inc/with\\ space.h\n\ninc/shared.h:\n/usr/include/stdio.h:\n";
  auto v = closure::parse_depfile(d);
  CHECK(v.size() == 4);
  CHECK(v[0] == "f1.c"); CHECK(v[1] == "inc/shared.h"); CHECK(v[2] == "/usr/include/stdio.h"); CHECK(v[3] == "inc/with space.h");
  // -MM output: "f1.o: f1.c inc/shared.h"
  auto w = closure::parse_depfile("f1.o: f1.c inc/shared.h\n");
  CHECK(w.size() == 2 && w[1] == "inc/shared.h");
  // target with a space before the colon
  auto x = closure::parse_depfile("f1.o : a.h b.h\n");
  CHECK(x.size() == 2 && x[0] == "a.h");
}

OPTICC_TEST(argscan_wp_mmd) {
  args::Scan s = args::scan({"gcc", "-Wp,-MMD,.foo.o.d", "-O2", "-c", "foo.c", "-o", "foo.o"});
  CHECK(s.distributable); CHECK(s.has_md); CHECK(s.has_mf); CHECK(s.dep_file == ".foo.o.d");
  args::Scan t = args::scan({"cc", "-MMD", "-MF", "x/y.d", "-MT", "tgt.o", "-c", "a.c", "-o", "x/a.o"});
  CHECK(t.dep_file == "x/y.d" && t.dep_target == "tgt.o");
  CHECK(args::default_dep_name("x/a.o") == "x/a.d");
  CHECK(args::default_dep_name("dir.v1/a") == "dir.v1/a.d");
}

OPTICC_TEST(hp_remote_argv_rewrite) {
  std::vector<std::string> argv{"cc", "-O2", "-I/proj/inc", "-Iinc", "-isystem", "/proj/third", "-include", "/proj/pre.h",
                                "-MMD", "-MF", "f.d", "-c", "/proj/src/f.c", "-o", "f.o"};
  args::Scan s = args::scan(argv);
  auto out = args::hp_remote_argv(argv, s, "/tmp/root", "/tmp/root/out.o", "/tmp/root/dep.d", "f.o");
  std::string joined; for (auto& a : out) joined += a + " ";
  CHECK(joined.find("-I/tmp/root/proj/inc ") != std::string::npos);   // absolute -I rooted
  CHECK(joined.find(" -Iinc ") != std::string::npos);                  // relative -I untouched (cwd is rooted)
  CHECK(joined.find("-isystem /tmp/root/proj/third ") != std::string::npos);
  CHECK(joined.find("-include /tmp/root/proj/pre.h ") != std::string::npos);
  CHECK(joined.find(" /tmp/root/proj/src/f.c ") != std::string::npos);  // absolute source rooted
  CHECK(joined.find("-MF f.d") == std::string::npos);                  // client's -MF replaced
  CHECK(joined.find("-MF /tmp/root/dep.d -MT f.o") != std::string::npos);
  { int n = 0; for (auto& a : out) n += a == "-MMD"; CHECK(n == 1); }   // the client's own -MMD is kept once
  CHECK(joined.find("-ffile-prefix-map=/tmp/root=") != std::string::npos);
  CHECK(out.back() == "/tmp/root/out.o" && out[out.size() - 2] == "-o");
  // -Wp,-MMD form: dropped and replaced by explicit -MMD
  std::vector<std::string> kb{"gcc", "-Wp,-MMD,.f.o.d", "-c", "f.c", "-o", "f.o"};
  auto o2 = args::hp_remote_argv(kb, args::scan(kb), "/r", "/r/o.o", "/r/d.d", "f.o");
  std::string j2; for (auto& a : o2) j2 += a + " ";
  CHECK(j2.find("-Wp,") == std::string::npos); CHECK(j2.find("-MMD -MF /r/d.d -MT f.o") != std::string::npos);
  // include dirs extraction keeps order and forms
  auto dirs = args::include_dirs(argv);
  CHECK(dirs.bracket.size() == 2 && dirs.bracket[0] == "/proj/inc" && dirs.bracket[1] == "inc");
  CHECK(dirs.system.size() == 1 && dirs.forced.size() == 1);
  // -MM command has no -c/-o/-M* and ends with -MM
  auto mm = args::mm_argv(argv, s); std::string j3; for (auto& a : mm) j3 += a + " ";
  CHECK(mm.back() == "-MM"); CHECK(j3.find(" -c ") == std::string::npos); CHECK(j3.find("-MF") == std::string::npos); CHECK(j3.find("-o ") == std::string::npos);
}

OPTICC_TEST(resolve_include_and_missing_diag) {
  std::string d = util::make_temp_dir("opticc-inc-");
  util::mkdir_p(d + "/inc"); util::mkdir_p(d + "/src/sub");
  util::write_file(d + "/inc/shared.h", "x"); util::write_file(d + "/src/sub/local.h", "y");
  args::IncludeDirs dirs; dirs.bracket = {"inc"};
  CHECK(closure::resolve_include("shared.h", d + "/src/sub/f.c", dirs, {}, d) == d + "/inc/shared.h");
  CHECK(closure::resolve_include("local.h", d + "/src/sub/f.c", dirs, {}, d) == d + "/src/sub/local.h");  // includer's dir first
  CHECK(closure::resolve_include("nope.h", d + "/src/sub/f.c", dirs, {}, d).empty());
  std::string name, inc;
  CHECK(closure::parse_missing_include("/r/proj/f1.c:1:10: fatal error: 'shared.h' file not found\n#include \"shared.h\"\n", &name, &inc));
  CHECK(name == "shared.h" && inc == "/r/proj/f1.c");
  CHECK(closure::parse_missing_include("f.c:2:10: fatal error: foo/bar.h: No such file or directory\n", &name, &inc));
  CHECK(name == "foo/bar.h" && inc == "f.c");
  CHECK(!closure::parse_missing_include("f.c:3:5: error: expected ';'\n", &name, &inc));
  CHECK(util::path_within("/usr/include/stdio.h", "/usr/include")); CHECK(!util::path_within("/usr/includex/a.h", "/usr/include"));
  util::remove_tree(d);
}

// kbuild `.cmd` closure source (fixdep output, kbuild 6.12): deps_ list minus $(wildcard ...) config pseudo-deps.
OPTICC_TEST(kbuild_cmd_parse) {
  std::string cmd =
      "savedcmd_kernel/power/qos.o := clang -Wp,-MMD,kernel/power/.qos.o.d -nostdinc -I/srv/src/linux/arch/x86/include -c -o kernel/power/qos.o /srv/src/linux/kernel/power/qos.c\n"
      "\n"
      "source_kernel/power/qos.o := /srv/src/linux/kernel/power/qos.c\n"
      "\n"
      "deps_kernel/power/qos.o := \\\n"
      "    $(wildcard include/config/PM_SLEEP) \\\n"
      "  include/linux/compiler-version.h \\\n"
      "    $(wildcard include/config/CC_VERSION_TEXT) \\\n"
      "  include/linux/kconfig.h \\\n"
      "  /srv/src/linux/include/linux/compiler_types.h \\\n"
      "  include/generated/autoconf.h \\\n"
      "  include/linux/kconfig.h \\\n"
      "  arch/x86/include/generated/asm/rwonce.h \\\n"
      "\n"
      "kernel/power/qos.o: $(deps_kernel/power/qos.o)\n"
      "\n"
      "$(deps_kernel/power/qos.o):\n";
  auto v = closure::parse_kbuild_cmd(cmd);
  CHECK(v.size() == 6);
  CHECK(v[0] == "/srv/src/linux/kernel/power/qos.c");          // source first
  CHECK(v[1] == "include/linux/compiler-version.h");
  CHECK(v[2] == "include/linux/kconfig.h");                    // duplicate kept once
  CHECK(v[3] == "/srv/src/linux/include/linux/compiler_types.h");
  CHECK(v[4] == "include/generated/autoconf.h");
  CHECK(v[5] == "arch/x86/include/generated/asm/rwonce.h");
  for (auto& p : v) CHECK(p.find("wildcard") == std::string::npos);
  CHECK(closure::kbuild_cmd_path("kernel/power/qos.o") == "kernel/power/.qos.o.cmd");
  CHECK(closure::kbuild_cmd_path("qos.o") == ".qos.o.cmd");
  CHECK(closure::parse_kbuild_cmd("savedcmd_x := y\n").empty());
}

// ---- include scanner -------------------------------------------------------------------------
#include "opticc/incscan.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

OPTICC_TEST(incscan_parse_directives) {
  std::string src = "#include <a/b.h>\n  #  include \"c.h\" // trailing\n/* #include \"x.h\" */\n#include_next <d.h>\n"
                    "#if 0\n#include \"still_counted.h\"\n#endif\n#define HDR <e.h>\n#import \"f.h\"\n#include HDR\n#include \\\n \"g.h\"\n";
  auto fs = opticc::incscan::parse_text(src);
  std::vector<std::string> names; for (auto& d : fs.incs) names.push_back(d.name);
  CHECK((names == std::vector<std::string>{"a/b.h", "c.h", "d.h", "still_counted.h", "f.h", "g.h"}));
  CHECK(fs.incs[0].angled && !fs.incs[1].angled && fs.incs[2].next);
  CHECK(fs.computed);   // #include HDR
  CHECK(!opticc::incscan::parse_text("#include \"only.h\"\n").computed);
}

namespace {
std::string mk_tree(const std::string& base, int headers, int tus, int fanout, std::mt19937& rng) {
  namespace fsys = std::filesystem;
  fsys::create_directories(base + "/include/linux"); fsys::create_directories(base + "/include/asm"); fsys::create_directories(base + "/src");
  std::uniform_int_distribution<int> pick(0, headers - 1);
  for (int h = 0; h < headers; ++h) {
    std::ofstream f(base + (h % 2 ? "/include/linux/h" : "/include/asm/h") + std::to_string(h) + ".h");
    f << "#ifndef H" << h << "\n#define H" << h << "\nstatic inline int f" << h << "(int x){return x+" << h << ";}\n";
    for (int k = 0; k < fanout; ++k) { int t = pick(rng); if (t < h) f << "#include <" << (t % 2 ? "linux/h" : "asm/h") << t << ".h>\n"; }
    f << "#endif\n";
  }
  for (int t = 0; t < tus; ++t) {
    std::ofstream f(base + "/src/tu" + std::to_string(t) + ".c");
    for (int k = 0; k < 12; ++k) { int h = headers - 1 - (t * 7 + k * 13) % headers; f << "#include <" << (h % 2 ? "linux/h" : "asm/h") << h << ".h>\n"; }
    f << "int g" << t << "(void){return " << t << ";}\n";
  }
  return base;
}
}  // namespace

OPTICC_TEST(incscan_closure_small_tree) {
  namespace fsys = std::filesystem;
  std::string base = opticc::util::make_temp_dir("opticc-incscan-");
  fsys::create_directories(base + "/inc/sub"); fsys::create_directories(base + "/sys");
  opticc::util::write_file(base + "/inc/a.h", "#include \"sub/b.h\"\n#include <sysonly.h>\n");
  opticc::util::write_file(base + "/inc/sub/b.h", "#include \"c.h\"\n");          // quoted: relative to the includer's dir
  opticc::util::write_file(base + "/inc/sub/c.h", "int c;\n");
  opticc::util::write_file(base + "/sys/sysonly.h", "#include \"never_entered.h\"\n");
  opticc::util::write_file(base + "/main.c", "#include <a.h>\n#include \"missing.h\"\n");
  opticc::args::IncludeDirs dirs; dirs.bracket = {base + "/inc"};
  opticc::incscan::Scanner sc; bool computed = true; int n = 0;
  auto cl = sc.closure(base + "/main.c", dirs, {base + "/sys"}, base, &computed, &n);
  std::vector<std::string> want{base + "/inc/a.h", base + "/inc/sub/b.h", base + "/inc/sub/c.h"};
  CHECK(cl == want);
  CHECK(!computed);
  CHECK(n == 4);   // main.c + 3 headers; the system header is not entered
  // cache: a second closure visits nothing new and gives the same answer
  auto cl2 = sc.closure(base + "/main.c", dirs, {base + "/sys"}, base, &computed, &n);
  CHECK(cl2 == want && sc.cache_size() == 4);
  opticc::util::remove_tree(base);
}

// Cost model behind trace_to_sim's q (q_a + q_b * includes): a kernel-shaped tree (2500 shared headers, TUs with
// ~400-file closures), 300 TUs scanned with a warm cache after the first. Prints the per-TU cost; asserts it stays
// far below a preprocessor run (tens of ms) once the cache is warm.
OPTICC_TEST(incscan_synthetic_tree_cost) {
  std::mt19937 rng(7);
  std::string base = mk_tree(opticc::util::make_temp_dir("opticc-incscan-big-"), 2500, 300, 8, rng);
  opticc::args::IncludeDirs dirs; dirs.bracket = {base + "/include"};
  opticc::incscan::Scanner sc;
  double first = 0, rest = 0; size_t total_files = 0; int visited = 0;
  for (int t = 0; t < 300; ++t) {
    auto t0 = std::chrono::steady_clock::now(); bool comp = false; int n = 0;
    auto cl = sc.closure(base + "/src/tu" + std::to_string(t) + ".c", dirs, {}, base, &comp, &n);
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (t == 0) first = dt; else rest += dt;
    total_files += cl.size(); visited += n;
  }
  double per_tu = rest / 299, mean_closure = static_cast<double>(total_files) / 300, per_file = per_tu / std::max(1.0, mean_closure);
  std::printf("  incscan: visited %d files in total\n", visited);
  std::printf("  incscan: first TU %.1f ms, warm mean %.2f ms/TU, mean closure %.0f files, %.1f us/file\n", first * 1e3, per_tu * 1e3, mean_closure, per_file * 1e6);
  CHECK(mean_closure > 100);
  CHECK(per_tu < 0.03);            // warm: bookkeeping only (stat + cache lookups)
  CHECK(per_file < 50e-6);
  opticc::util::remove_tree(base);
}


// kbuild/autoconf feature probes must never be distributed (a distributed probe answers for the wrong host)
OPTICC_TEST(probe_dev_null_is_local) {
  using namespace opticc;
  auto s1 = args::scan({"clang", "-Werror", "-Wno-address-of-packed-member", "-c", "-x", "c", "/dev/null", "-o", "/tmp/p.o"});
  CHECK(!s1.distributable);
  auto s2 = args::scan({"clang", "-c", "foo.c", "-o", "/dev/null"});
  CHECK(!s2.distributable);
  auto s3 = args::scan({"clang", "-c", "foo.c", "-o", "foo.o"});
  CHECK(s3.distributable);
}


// classic (.i) compiles must not carry preprocessor-only flags (kbuild -Werror=unused-command-line-argument)
OPTICC_TEST(classic_drops_macro_prefix_map) {
  using namespace opticc;
  std::vector<std::string> argv = {"clang", "-fmacro-prefix-map=/srv/src/linux/=", "-Werror=unused-command-line-argument", "-O2", "-c", "a.c", "-o", "a.o"};
  auto s = args::scan(argv);
  CHECK(s.distributable);
  auto r = args::remote_argv(argv, s, "/tmp/a.i", "/tmp/a.o");
  bool has = false; for (auto& x : r) if (x.rfind("-fmacro-prefix-map", 0) == 0) has = true;
  CHECK(!has);
}

// Per-child CPU accounting: a short child's measured CPU must not include the CPU of other children
// that exit concurrently (a RUSAGE_CHILDREN delta would, inflating w_meas ~10x on 16-slot workers).
#include <thread>
OPTICC_TEST(run_process_cpu_is_per_child) {
  using namespace opticc;
  const std::vector<std::string> burn = {"sh", "-c", "i=0; while [ $i -lt 400000 ]; do i=$((i+1)); done"};
  std::vector<std::thread> ts;
  std::vector<double> burn_cpu(4, 0.0);
  for (int k = 0; k < 4; ++k) ts.emplace_back([&, k] { burn_cpu[static_cast<size_t>(k)] = util::run_process(burn, "", "", "").cpu_s; });
  // a trivial child that overlaps the burners and exits after them
  auto quick = util::run_process({"sh", "-c", "sleep 1.5"}, "", "", "");
  for (auto& t : ts) t.join();
  for (double c : burn_cpu) CHECK(c > 0.05);   // burners did measurable work
  CHECK(quick.cpu_s < 0.05);                   // sleep's own CPU only, not the burners'
}

// distributed ThinLTO backend jobs (lang "ir"): distributable, the index path is captured and rooted on the
// worker, the module input is rooted like a source
OPTICC_TEST(argscan_thinlto_backend) {
  std::vector<std::string> a{"clang", "-O2", "-ffunction-sections", "-fdata-sections", "-c", "-x", "ir", "kernel/fork.o",
                             "-fthinlto-index=/b/nat/kernel/fork.o.thinlto.bc", "-o", "/b/nat/kernel/fork.o"};
  auto s = args::scan(a);
  CHECK(s.distributable); CHECK(s.lang == "ir"); CHECK(s.source == "kernel/fork.o");
  CHECK(s.thinlto_index == "/b/nat/kernel/fork.o.thinlto.bc"); CHECK(s.output == "/b/nat/kernel/fork.o");
  auto hp = args::hp_remote_argv(a, s, "/r", "/r/__out.o", "/r/__dep.d", "x");
  CHECK(has(hp, "-fthinlto-index=/r/b/nat/kernel/fork.o.thinlto.bc")); CHECK(has(hp, "kernel/fork.o"));   // relative module stays relative to the job cwd
  CHECK(has(hp, "-x")); CHECK(has(hp, "ir")); CHECK(hp[static_cast<size_t>(idx(hp, "-o")) + 1] == "/r/__out.o");
  // assembler input stays local
  auto asmj = args::scan({"clang", "-c", "-x", "assembler", "x.s", "-o", "x.o"}); CHECK(!asmj.distributable);
}

// The persisted parse memo survives a "restart" (a new Scanner), is used only for files whose (mtime, size, inode,
// ctime) are unchanged, and never for racy files (changed < 2 s before the parse): an edit always takes effect.
OPTICC_TEST(incscan_persisted_state_exact) {
  std::string d = opticc::util::make_temp_dir("opticc-ist-");
  opticc::util::mkdir_p(d + "/inc");
  opticc::util::write_file(d + "/inc/a.h", "#pragma once\n#include \"b.h\"\n");
  opticc::util::write_file(d + "/inc/b.h", "#pragma once\n");
  opticc::util::write_file(d + "/inc/c.h", "#pragma once\n");
  opticc::util::write_file(d + "/t.c", "#include \"inc/a.h\"\nint x;\n");
  // racy is judged on ctime as well (which cannot be backdated): let the files age past the 2 s window
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  auto dirs = opticc::args::include_dirs({"cc", "-I", d + "/inc", "-c", d + "/t.c"});
  bool comp = false;
  opticc::incscan::Scanner s1;
  auto c1 = s1.closure(d + "/t.c", dirs, {}, d, &comp);
  CHECK(std::find(c1.begin(), c1.end(), d + "/inc/b.h") != c1.end());
  std::string blob = s1.export_state(); CHECK(!blob.empty());
  // restart: a new scanner with the persisted memo gives the same closure without re-parsing unchanged files
  opticc::incscan::Scanner s2; s2.import_state(blob);
  auto c2 = s2.closure(d + "/t.c", dirs, {}, d, &comp);
  CHECK(c2 == c1); CHECK(s2.stats().parses == 0); CHECK(s2.stats().state_hits >= 3);
  // an edit with the SAME size but a different include (and a newer mtime) must be re-parsed, not taken from the memo
  opticc::util::write_file(d + "/inc/a.h", "#pragma once\n#include \"c.h\"\n");
  opticc::incscan::Scanner s3; s3.import_state(blob);
  auto c3 = s3.closure(d + "/t.c", dirs, {}, d, &comp);
  CHECK(std::find(c3.begin(), c3.end(), d + "/inc/c.h") != c3.end());
  CHECK(std::find(c3.begin(), c3.end(), d + "/inc/b.h") == c3.end());
  // the just-edited a.h is racy (parsed < 2 s after its mtime): it must not be exported
  std::string blob3 = s3.export_state();
  opticc::incscan::Scanner s4; s4.import_state(blob3);
  s4.closure(d + "/t.c", dirs, {}, d, &comp);
  CHECK(s4.stats().parses >= 1);
}
