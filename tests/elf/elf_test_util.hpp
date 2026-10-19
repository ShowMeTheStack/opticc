// SPDX-License-Identifier: GPL-2.0-only
// Helpers for the linker-domain tests: tool discovery, process execution, and a generated multi-object test
// program (static functions, extern calls, string literals, data pointers, function-pointer tables, switch
// jump tables, __start_/__stop_ sections, C++ COMDAT groups, a few -mcmodel=kernel objects).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

namespace elftest {

struct Run { int rc; std::string out; };
inline Run run(const std::string& cmd) {
  std::string full = cmd + " 2>&1"; FILE* f = popen(full.c_str(), "r"); if (!f) return {127, "popen failed"};
  std::string s; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  int st = pclose(f); return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, s};
}
inline bool exists(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0; }
inline std::string tool(const std::string& name) {
  std::vector<std::string> dirs;
  if (const char* e = std::getenv("OPTICC_LLVM_BIN")) dirs.push_back(e);
  for (const char* d : {"/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin", "/usr/local/opt/llvm/bin", "/usr/lib/llvm-20/bin", "/usr/local/bin", "/usr/bin"}) dirs.push_back(d);
  for (auto& d : dirs) { if (exists(d + "/" + name)) return d + "/" + name; if (exists(d + "/" + name + "-20")) return d + "/" + name + "-20"; }
  return name;  // hope it is on PATH
}
inline std::string tmpdir(const std::string& prefix) {
  const char* base = std::getenv("TMPDIR"); std::string t = std::string(base && *base ? base : "/tmp"); if (t.back() != '/') t += '/';
  std::string tpl = t + prefix + "XXXXXX"; std::vector<char> buf(tpl.begin(), tpl.end()); buf.push_back(0);
  if (!mkdtemp(buf.data())) throw std::runtime_error("mkdtemp failed"); return std::string(buf.data());
}
inline void write(const std::string& p, const std::string& s) { std::ofstream f(p); f << s; }
inline std::string read(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

struct Program {
  std::string dir;
  std::vector<std::string> objs;     // all objects in link order (entry first)
  std::string entry = "_start";
};

// `eh`: emit .eh_frame (asynchronous unwind tables). `clang` is Apple clang on macOS (cross target works); C++ via clang++.
// `cc_override`/`cxx_override`: compiler command prefixes (e.g. "<opticc> clang") so the objects can be produced through
// the real distributed tool; empty = direct clang/clang++.
inline Program gen_program(const std::string& dir, bool eh = false, int nfiles = 10, const std::string& cc_override = "", const std::string& cxx_override = "") {
  Program P; P.dir = dir;
  std::string common = "#pragma once\n";
  for (int i = 0; i < nfiles; ++i) { char b[256]; std::snprintf(b, sizeof b, "int f%d(int); const char* name%d(void); extern int tab%d[4]; extern int (*fp%d[2])(int); int deadglob%d(int);\n", i, i, i, i, i); common += b; }
  common += "int cppA(int); int cppB(int);\n";
  write(dir + "/common.h", common);
  for (int i = 0; i < nfiles; ++i) {
    int nx = (i + 1) % nfiles; std::ostringstream s;
    s << "#include \"common.h\"\n"
      << "static int helper" << i << "(int x) { return x * " << (i + 2) << " + 1; }\n"
      << "static int dead" << i << "(int x) { return x - 7; }\n"
      << "int deadglob" << i << "(int x) { return dead" << i << "(x) + 11; }\n"
      << "int tab" << i << "[4] = {" << i << "," << i + 1 << "," << i + 2 << "," << i + 3 << "};\n"
      << "int *ptr" << i << " = tab" << i << ";\n"
      << "int (*fp" << i << "[2])(int) = { f" << i << ", f" << nx << " };\n"
      << "static const char *strs" << i << "[3] = {\"alpha_" << i << "\", \"beta_" << i << "\", \"gamma_" << i << "\"};\n"
      << "const char* name" << i << "(void) { return strs" << i << "[1]; }\n"
      << "int f" << i << "(int x) {\n  int r = helper" << i << "(x);\n"
      << "  switch (x & 7) { case 0: r += tab" << nx << "[1]; break; case 1: r ^= 3; break; case 2: r += name" << i << "()[0]; break; case 3: r *= 5; break;\n"
      << "    case 4: r -= tab" << i << "[2]; break; case 5: r += fp" << i << "[1](r & 3); break; case 6: r += 100; break; default: r += ptr" << i << "[0]; }\n"
      << "  if (x > 0) r += f" << nx << "(x - 1);\n  return r;\n}\n";
    write(dir + "/f" + std::to_string(i) + ".c", s.str());
  }
  write(dir + "/mytabA.c", "__attribute__((section(\"mytab\"))) int tabentryA = 1;\nstatic int deadA(int x) { return x; }\nint deadglobA(int x) { return deadA(x) + 1; }\n");
  write(dir + "/mytabB.c", "__attribute__((section(\"mytab\"))) int tabentryB = 2;\n__attribute__((section(\"mytab\"))) int tabentryB2 = 3;\n");
  write(dir + "/entry.c", "#include \"common.h\"\nextern int __start_mytab[], __stop_mytab[];\n__attribute__((section(\"mytab\"))) int tabentry_main = 42;\nvolatile int result;\n"
                          "void _start(void) { int s = 0; for (int *p = __start_mytab; p < __stop_mytab; ++p) s += *p; result = f0(5) + s + name3()[0] + cppA(3) + cppB(4); for(;;); }\n");
  write(dir + "/cpp1.cpp", "extern \"C\" int f1(int);\n__attribute__((noinline)) inline int inl_shared(int x) { return x * 3 + f1(x); }\ntemplate<int K> __attribute__((noinline)) int tmpl(int x) { return x + K; }\n"
                           "__attribute__((noinline)) inline int unused_inl(int x) { return x * 7; }\nstatic int deadcpp(int x) { return unused_inl(x); }\nint deadcppglob(int x) { return deadcpp(x); }\n"
                           "extern \"C\" int cppA(int x) { return inl_shared(x) + tmpl<3>(x); }\n");
  write(dir + "/cpp2.cpp", "extern \"C\" int f1(int);\n__attribute__((noinline)) inline int inl_shared(int x) { return x * 3 + f1(x); }\ntemplate<int K> __attribute__((noinline)) int tmpl(int x) { return x + K; }\n"
                           "extern \"C\" int cppB(int x) { return inl_shared(x) + tmpl<3>(x) + tmpl<4>(x); }\n");
  std::string cc = tool("clang"), cxx = tool("clang++");
  if (!exists(cc)) cc = "clang"; if (!exists(cxx)) cxx = "clang++";
  if (!cc_override.empty()) cc = cc_override; if (!cxx_override.empty()) cxx = cxx_override;
  std::string base = " --target=x86_64-linux-gnu -ffreestanding -nostdinc -O2 -ffunction-sections -fdata-sections -c ";
  base += eh ? "-fasynchronous-unwind-tables " : "-fno-asynchronous-unwind-tables ";
  auto compile = [&](const std::string& comp, const std::string& src, const std::string& extra) {
    std::string obj = dir + "/" + src.substr(0, src.find('.')) + ".o";
    Run r = run(comp + base + extra + " " + dir + "/" + src + " -o " + obj);
    if (r.rc) throw std::runtime_error("compile failed: " + src + "\n" + r.out);
    P.objs.push_back(obj);
  };
  compile(cc, "entry.c", "-fPIC");
  for (int i = 0; i < nfiles; ++i) compile(cc, "f" + std::to_string(i) + ".c", (i >= nfiles - 2) ? "-mcmodel=kernel -fno-PIC" : "-fPIC");
  compile(cc, "mytabA.c", "-fPIC"); compile(cc, "mytabB.c", "-mcmodel=kernel -fno-PIC");
  compile(cxx, "cpp1.cpp", "-fPIC -nostdinc++ -fno-exceptions -fno-rtti -std=c++17"); compile(cxx, "cpp2.cpp", "-fPIC -nostdinc++ -fno-exceptions -fno-rtti -std=c++17");
  return P;
}

inline std::string join(const std::vector<std::string>& v, const std::string& sep = " ") { std::string s; for (size_t i = 0; i < v.size(); ++i) { if (i) s += sep; s += v[i]; } return s; }

// Final static link with lld; returns rc/out. `gc` adds --gc-sections. Always -z keep-text-section-prefix (so the
// hot/unlikely/startup/exit grouping matches the prelink merge table) and -z nostart-stop-gc (__start_/__stop_ references
// retain C-identifier sections, as GNU ld and the kernel assume).
inline Run lld_link(const std::vector<std::string>& objs, const std::string& out, bool gc, const std::string& extra = "") {
  return run(tool("ld.lld") + " -static -nostdlib -e _start -z keep-text-section-prefix -z nostart-stop-gc " + std::string(gc ? "--gc-sections " : "") + extra + " -o " + out + " " + join(objs));
}

// Normalised description of a linked executable for byte-level comparison of alloc sections + global symbol addresses.
// Compare two signatures: sections, data and disassembly must be identical; symbol lines are compared on the names present in
// both (the prelink localises/strips symbols) and the common set must cover at least `min_common` of the reference's names.
inline bool signatures_equal(const std::string& ref, const std::string& got, std::string* why, double min_common = 0.9) {
  auto split = [](const std::string& s) { std::vector<std::string> pre; std::map<std::string, std::string> nm; std::istringstream is(s); std::string l; bool in_nm = false;
    while (std::getline(is, l)) { if (l == "NM") { in_nm = true; continue; } if (l == "DIS") { in_nm = false; } if (in_nm) { auto sp = l.find(' '); if (sp != std::string::npos) nm[l.substr(0, sp)] = l.substr(sp + 1); } else pre.push_back(l); }
    return std::make_pair(pre, nm); };
  auto [pa, na] = split(ref); auto [pb, nb] = split(got);
  if (pa != pb) { for (size_t i = 0; i < std::min(pa.size(), pb.size()); ++i) if (pa[i] != pb[i]) { *why = "ref: " + pa[i].substr(0, 160) + "\n    got: " + pb[i].substr(0, 160); return false; } *why = "different line counts"; return false; }
  size_t common = 0; for (auto& [n, a] : na) { auto it = nb.find(n); if (it == nb.end()) continue; common++; if (it->second != a) { *why = "symbol " + n + " at " + a + " vs " + it->second; return false; } }
  if (static_cast<double>(common) < min_common * static_cast<double>(na.size())) { *why = "only " + std::to_string(common) + " of " + std::to_string(na.size()) + " symbols in common"; return false; }
  return true;
}

inline std::string link_signature(const std::string& exe, const std::string& workdir) {
  std::string sig;
  Run secs = run(tool("llvm-readelf") + " -S -W " + exe);
  std::vector<std::string> names;
  std::istringstream is(secs.out); std::string line;
  while (std::getline(is, line)) {
    auto p = line.find(']'); if (p == std::string::npos) continue;
    std::istringstream ls(line.substr(p + 1)); std::string name, type, addr, off, size, es, flg; ls >> name >> type >> addr >> off >> size >> es >> flg;
    if (name.empty() || name == "Name" || name == "NULL") continue;
    if (flg.find('A') == std::string::npos) continue;        // alloc only
    sig += "SEC " + name + " " + type + " " + addr + " " + size + " " + flg + "\n"; names.push_back(name);
  }
  for (auto& n : names) {
    std::string bin = workdir + "/sec.bin"; run(tool("llvm-objcopy") + " -O binary --only-section=" + n + " " + exe + " " + bin);
    std::string data = read(bin); std::ostringstream hex; hex << n << " "; for (unsigned char c : data) { char b[3]; std::snprintf(b, sizeof b, "%02x", c); hex << b; } sig += "DATA " + hex.str() + "\n";
  }
  // symbol addresses: all defined symbols, as name->address lines (compared on the common name set by the caller)
  Run nm = run(tool("llvm-nm") + " --defined-only " + exe + " | awk '$3 != \"\" {print $3, $1}' | sort");
  sig += "NM\n" + nm.out;
  Run dis = run(tool("llvm-objdump") + " -d --no-show-raw-insn " + exe + " | grep -v 'file format' | grep -v '^$'");
  sig += "DIS\n" + dis.out;
  return sig;
}

}  // namespace elftest
