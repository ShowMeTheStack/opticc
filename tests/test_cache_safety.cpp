// SPDX-License-Identifier: GPL-2.0-only
// Cache-safety checks in libopticc: key kinds (rdirect), extra-output flags (direct::safe_args), compiler identity
// (cachekey::toolchain_parts), the assembler file-directive detector (file_directive), comment-sensitive flags
// (keeps_comments) and single-flight compiler identity (compiler_id).
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "opticc/cachekey.hpp"
#include "opticc/direct.hpp"
#include "opticc/rdirect.hpp"
#include "opticc/util.hpp"
#include "test_harness.hpp"

namespace rd = opticc::rdirect;
namespace ut = opticc::util;

// A direct key ("", older clients) serves only entries with inputs; "pp"/"ppidx" only entries with none; the kind
// survives the PUT and GET codecs; an unknown kind fits nothing
OPTICC_TEST(rdirect_key_kinds) {
  rd::PEntry direct; direct.obj = "o"; direct.pos.push_back(rd::PPos{"/a.h", "s", 1, ""});
  rd::PEntry pp; pp.obj = "p";
  rd::PEntry bad; bad.obj = "b"; bad.neg.push_back(rd::PNeg{"/d", {"x.h"}});   // negatives without positives
  CHECK(rd::entry_fits_kind(direct, "") && !rd::entry_fits_kind(direct, "pp") && !rd::entry_fits_kind(direct, "ppidx"));
  CHECK(!rd::entry_fits_kind(pp, "") && rd::entry_fits_kind(pp, "pp") && rd::entry_fits_kind(pp, "ppidx"));
  CHECK(!rd::entry_fits_kind(bad, "") && !rd::entry_fits_kind(bad, "pp"));
  CHECK(!rd::entry_fits_kind(direct, "other") && !rd::entry_fits_kind(pp, "other"));
  auto f = rd::filter_kind({direct, pp, bad}, "");
  CHECK(f.size() == 1 && f[0].obj == "o");
  f = rd::filter_kind({direct, pp, bad}, "pp");
  CHECK(f.size() == 1 && f[0].obj == "p");
  rd::PutMsg m; m.pkey = std::string(64, 'a'); m.entry = pp; m.kind = "pp"; m.obj.okey = "p"; m.obj.object = "x";
  CHECK(rd::decode_put(rd::encode_put(m)).kind == "pp");
  m.kind.clear(); CHECK(rd::decode_put(rd::encode_put(m)).kind.empty());
  rd::GetMsg g{7, std::string(64, 'b'), "", "pp"};
  CHECK(rd::decode_get(rd::encode_get(g)).kind == "pp");
  g.kind.clear(); CHECK(rd::decode_get(rd::encode_get(g)).kind.empty());
}

// Flags that write files besides the object and the dependency file are refused on every cache path
OPTICC_TEST(safe_args_refuses_extra_outputs) {
  for (const char* f : {"-ftime-trace", "-ftime-trace=x.json", "-fstack-usage", "-fsave-optimization-record", "-fsave-optimization-record=yaml",
                        "-foptimization-record-file=r.yaml", "-MJ", "-save-temps", "-save-temps=obj", "--save-temps", "-fdump-tree-all",
                        "-Wa,-adhln=x.lst", "-gsplit-dwarf", "-fcallgraph-info", "-ftest-coverage", "--coverage",
                        // further flags that write side files or change the output form
                        "-fopt-info-vec=v.txt", "-aux-info", "-fdiagnostics-format=sarif-file", "-fdiagnostics-add-output=sarif",
                        "-Wp,-P", "-Xpreprocessor", "-Wa,--noexecstack,-adhln=x.lst"}) {
    std::string why;
    CHECK(!opticc::direct::safe_args({"cc", "-c", "a.c", f}, &why));
    if (why != f) std::printf("  flag %s not refused (why '%s')\n", f, why.c_str());
  }
  CHECK(opticc::direct::safe_args({"cc", "-c", "a.c", "-O2", "-Wall", "-MD", "-MF", "a.d", "-o", "a.o"}));
  CHECK(!opticc::direct::safe_args({"cc", "-c", "a.c", "-Xassembler", "-adhln=x.lst"}));
  CHECK(opticc::direct::safe_args({"cc", "-c", "a.c", "-Wa,--noexecstack"}));
}

// The compiler identity covers the programs the driver runs (cc1/cc1plus/as) and its shared libraries, by CONTENT
OPTICC_TEST(compiler_identity_covers_helpers) {
  std::string d = ut::make_temp_dir("opticc-cid-t-");
  ut::write_file(d + "/myas", "assembler v1\n"); ut::write_file(d + "/mycc1", "cc1 v1\n");
  ut::write_file(d + "/fakecc",
                 "#!/bin/sh\ncase \"$1\" in\n  --version) echo 'fakecc 1.0';;\n  -print-prog-name=as) echo '" + d + "/myas';;\n"
                 "  -print-prog-name=cc1) echo '" + d + "/mycc1';;\n  -print-prog-name=*) echo \"${1#-print-prog-name=}\";;\n  *) exit 1;;\nesac\n");
  ::chmod((d + "/fakecc").c_str(), 0755);
  bool ok = false;
  const std::string p1 = opticc::cachekey::toolchain_parts(d + "/fakecc", &ok);
  CHECK(ok);
  std::printf("  fake driver parts: %s\n", p1.c_str());
  CHECK(p1.find("|as:myas:") != std::string::npos && p1.find("|cc1:mycc1:") != std::string::npos);
  ut::write_file(d + "/myas", "assembler v2\n");
  CHECK(opticc::cachekey::toolchain_parts(d + "/fakecc") != p1);   // another assembler -> another identity
  ut::write_file(d + "/myas", "assembler v1\n"); ut::write_file(d + "/mycc1", "cc1 v2\n");
  CHECK(opticc::cachekey::toolchain_parts(d + "/fakecc") != p1);   // another cc1 -> another identity
#ifndef __APPLE__
  // a dynamically linked program: its shared libraries are listed (by content)
  const std::string pl = opticc::cachekey::toolchain_parts("/bin/ls");
  CHECK(pl.find("|lib:") != std::string::npos);
#endif
  // cost (reported, not asserted): the first identity of the real compilers on this machine
  for (const char* cc : {"clang", "clang++", "gcc", "g++"}) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string id = opticc::cachekey::compiler_id(cc);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  compiler_id(%s): %.3f s (first call, includes helpers and libraries) %s\n", cc, s, id.substr(0, 12).c_str());
  }
  ut::remove_tree(d);
}

// Sanitizers, sanitizer coverage and OpenMP embed source COLUMNS: no comment-insensitive key
OPTICC_TEST(keeps_comments_sanitizers_openmp) {
  namespace ck = opticc::cachekey;
  CHECK(!ck::keeps_comments({"cc", "-c", "a.c", "-O2"}));
  for (const char* f : {"-fsanitize=undefined", "-fsanitize=address", "-fsanitize-coverage=trace-pc", "-fopenmp", "-fopenmp=libomp", "-fopenmp-simd"})
    CHECK(ck::keeps_comments({"cc", "-c", "a.c", f}));
}

// The assembler-file-directive detector. Inline asm in C/C++ counts; a mere string (LLVM's
// DirectiveKindMap[".incbin"]) does not; assembler input counts anywhere
OPTICC_TEST(file_directive_detector) {
  namespace ck = opticc::cachekey;
  CHECK(ck::file_directive("asm(\".incbin \\\"blob.bin\\\"\");\n", false));
  CHECK(ck::file_directive("__asm__ (\n\"\t.section .rodata\\n\"\n\"\t.incbin \\\"kernel/config_data.gz\\\"\\n\");\n", false));
  CHECK(ck::file_directive("__asm__(\".include \\\"macros.s\\\"\");\n", false));
  CHECK(!ck::file_directive("DirectiveKindMap[\".incbin\"] = DK_INCBIN; DirectiveKindMap[\".include\"] = DK_INCLUDE;\n", false));
  CHECK(!ck::file_directive("int plasma = 1; /* .included */\n", false));
  // in a comment (the kernel's arch/x86/include/asm/nospec-branch.h; LLVM's MCTargetOptions.h): reads nothing
  CHECK(!ck::file_directive("/*\n * from C via asm(\".include <asm/nospec-branch.h>\") but let's not go there.\n */\n", false));
  CHECK(!ck::file_directive("  /// Additional paths to search for `.include` directives when using the asm parser\n", false));
  CHECK(ck::file_directive("/* c */ asm(\".incbin \\\"b\\\"\"); // tail\n", false));
  CHECK(ck::file_directive("\t.incbin \"x.bin\"\n", true));
  CHECK(ck::file_directive(".include \"m.s\"\n", true));
  CHECK(!ck::file_directive("\tmovl %eax, %ebx\n", true));
}

// The identity is computed ONCE per process and compiler even when many jobs ask at once (they wait for the first),
// and a failed helper query gives "", never cached, so the next call retries
OPTICC_TEST(compiler_identity_single_flight) {
  std::string d = ut::make_temp_dir("opticc-cid-sf-");
  ut::write_file(d + "/myas", "as\n");
  ut::write_file(d + "/count", "");
  ut::write_file(d + "/fakecc",
                 "#!/bin/sh\ncase \"$1\" in\n  --version) echo x >> '" + d + "/count'; sleep 1; echo 'fakecc 2.0';;\n"
                 "  -print-prog-name=as) [ -f '" + d + "/fail' ] && exit 3; echo '" + d + "/myas';;\n"
                 "  -print-prog-name=*) echo \"${1#-print-prog-name=}\";;\n  *) exit 1;;\nesac\n");
  ::chmod((d + "/fakecc").c_str(), 0755);
  std::vector<std::string> ids(8); std::vector<std::thread> th;
  for (int i = 0; i < 8; ++i) th.emplace_back([&, i] { ids[i] = opticc::cachekey::compiler_id(d + "/fakecc"); });
  for (auto& t : th) t.join();
  const size_t runs = ut::read_file(d + "/count").size() / 2;   // "x\n" per --version
  std::printf("  8 concurrent callers: --version ran %zu time(s), id %s\n", runs, ids[0].substr(0, 12).c_str());
  CHECK(!ids[0].empty());
  for (auto& x : ids) CHECK(x == ids[0]);
  CHECK(runs == 1);
  // a helper query that fails: no identity, and nothing cached (another driver path, same failure, then a retry)
  ut::write_file(d + "/fakecc2", ut::read_file(d + "/fakecc")); ::chmod((d + "/fakecc2").c_str(), 0755);
  ut::write_file(d + "/fail", "1");
  CHECK(opticc::cachekey::compiler_id(d + "/fakecc2").empty());
  std::remove((d + "/fail").c_str());
  CHECK(!opticc::cachekey::compiler_id(d + "/fakecc2").empty());   // recomputed: the failure was not cached
  ut::remove_tree(d);
}
