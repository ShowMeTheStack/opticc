// SPDX-License-Identifier: GPL-2.0-only
// opticc compiler-probe cache; see opticc/probe.hpp for the design and the correctness argument.
#include "opticc/probe.hpp"
#include "opticc/closure.hpp"
#include <sstream>

#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "opticc/direct.hpp"
#include "opticc/util.hpp"

namespace opticc::probe {

namespace {

bool starts(const std::string& a, const char* p) { return a.rfind(p, 0) == 0; }

// Flags whose value is the NEXT argument (so it is not an input).
bool takes_arg(const std::string& a) {
  static const char* k[] = {"-o", "-I", "-D", "-U", "-x", "-Xclang", "-Xassembler", "-Xpreprocessor", "-Xlinker", "-mllvm",
                            "-target", "-arch", "-isystem", "-iquote", "-idirafter", "-iprefix", "-iwithprefix",
                            "-iwithprefixbefore", "-isysroot", "--sysroot", "-L", "-l", "-u", "-z", "-T", "--param",
                            "-ftemplate-depth", "-MF", "-MT", "-MQ", "-include", "-imacros", "-include-pch", "-B", "-G",
                            "-wrapper", "-specs", "--config", "-dependency-file", "-ivfsoverlay", "-fmodules-cache-path"};
  for (auto s : k) if (a == s) return true;
  return false;
}

// Arguments that make the compiler read a file the key cannot see, or write a file other than the -o output.
const char* reads_or_writes(const std::string& a) {
  static const char* prefixes[] = {
      // reads
      "-include", "--include", "-imacros", "--imacros", "-ivfsoverlay", "-fplugin", "-fpass-plugin", "-fprofile-use",
      "-fprofile-instr-use", "-fprofile-sample-use", "-fauto-profile", "-fsanitize-blacklist", "-fsanitize-ignorelist",
      "-fsanitize-coverage-allowlist", "-fsanitize-coverage-ignorelist", "-fsanitize-coverage-whitelist",
      "-fsanitize-coverage-blacklist", "-fmodule", "-fmodules", "-fprebuilt-module", "-fimplicit-modules", "-fcxx-modules",
      "-fpch", "-emit-pch", "-specs", "--specs", "-B", "--config", "-fprofile-list=", "-fxray-always-instrument=",
      "-fxray-never-instrument=", "-fxray-attr-list=", "-fopenmp-host-ir-file-path", "-fembed-offload-object",
      "-fthinlto-index", "-fdiagnostics-hotness-threshold=auto", "-fuse-ld", "-fbasic-block-sections=list",
      "-mstack-protector-guard-symbol", "--rtlib", "-resource-dir",
      // writes
      "-M", "-Wp,", "-save-temps", "-gsplit-dwarf", "-ftime-trace", "-fstack-usage", "--coverage", "-ftest-coverage",
      "-fprofile-arcs", "-fsave-optimization-record", "-foptimization-record", "-fopt-record", "-fdump", "-fproc-stat-report",
      "-fcrash-diagnostics", "-dependency-file", "-dependency-dot", "-fmodules-cache-path",
      "-gen-reproducer", "-fcallgraph-info", "-fcs-profile-generate"};
  static const char* exact[] = {"-H", "-fsyntax-only", "-###", "-v", "-print-search-dirs", "-load", "-plugin", "-add-plugin"};
  for (const char* p : prefixes) if (starts(a, p)) return p;
  for (const char* e : exact) if (a == e) return e;
  if (!a.empty() && a[0] == '@') return "@";
  static const char* suffixes[] = {".pch", ".gch", ".pcm", ".profdata", ".profraw", ".afdo", ".so", ".dylib", ".cfg", ".txt", ".list"};
  for (const char* s : suffixes) { size_t n = std::strlen(s); if (a.size() >= n && a.compare(a.size() - n, n, s) == 0) return s; }
  return nullptr;
}

// Assembler/preprocessor pass-through tokens (-Wa,a,b / -Xassembler x): listings, response files, dependency output.
bool bad_passthrough_token(const std::string& t) {
  return starts(t, "@") || starts(t, "-a") || starts(t, "--MD") || starts(t, "-MD") || starts(t, "--listing") ||
         starts(t, "-MF") || starts(t, "-o") || t.find("file") != std::string::npos;
}

}  // namespace

Shape classify(const std::vector<std::string>& argv) {
  Shape s;
  auto no = [&](const std::string& w) { s.ok = false; s.why = w; return s; };
  if (argv.size() < 2) return no("no arguments");
  if (argv.size() == 2) {
    static const char* info[] = {"--version", "-dumpversion", "-dumpfullversion", "-dumpmachine"};
    for (const char* q : info) if (argv[1] == q) { s.ok = true; s.mode = Mode::Info; s.looks_like_probe = true; return s; }
  }
  bool has_x = false; int ninputs = 0;
  std::string pending_bad;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "/dev/null" || a == "-") {
      if (++ninputs == 1) { s.input_index = static_cast<int>(i); s.stdin_input = (a == "-"); s.looks_like_probe = true; }
      continue;
    }
    Mode m = a == "-c" ? Mode::Compile : a == "-S" ? Mode::Assemble : a == "-E" ? Mode::Preprocess : Mode::None;
    if (m != Mode::None) {
      if (s.mode != Mode::None && s.mode != m) pending_bad = "several modes";
      s.mode = m; continue;
    }
    if (a == "-o") {
      if (i + 1 >= argv.size()) return no("-o without value");
      if (!s.output.empty()) pending_bad = "several -o";
      s.output = argv[i + 1]; s.output_index = static_cast<int>(i + 1); ++i; continue;
    }
    if (starts(a, "-o") && a.size() > 2 && !starts(a, "-obj")) {   // joined -oPATH: not used by probes; not supported
      if (pending_bad.empty()) pending_bad = "joined -o form " + a;
      continue;
    }
    if (a == "-x") { if (i + 1 >= argv.size()) return no("-x without value"); has_x = true; ++i; continue; }
    if (starts(a, "-x") && a.size() > 2) { has_x = true; continue; }
    if (const char* w = reads_or_writes(a)) { if (pending_bad.empty()) pending_bad = std::string("file-touching argument ") + a + " (" + w + ")"; }
    // the result depends on the HOST CPU, which no key component names
    if ((a == "-march=native" || a == "-mcpu=native" || a == "-mtune=native") && pending_bad.empty()) pending_bad = "host-dependent " + a;
    // an external assembler (located through the driver's prefix and PATH) is not pinned by the key
    if ((a == "-fno-integrated-as" || a == "-no-integrated-as") && pending_bad.empty()) pending_bad = "external assembler " + a;
    if (starts(a, "-Wa,") || starts(a, "-Xassembler")) {
      std::string rest = a == "-Xassembler" ? (i + 1 < argv.size() ? argv[i + 1] : std::string()) : a.substr(a.find(',') == std::string::npos ? a.size() : a.find(',') + 1);
      size_t p = 0;
      while (p <= rest.size()) {
        size_t c = rest.find(',', p); std::string tok = rest.substr(p, c == std::string::npos ? std::string::npos : c - p);
        if (bad_passthrough_token(tok) && pending_bad.empty()) pending_bad = "assembler option " + tok;
        if (c == std::string::npos) break; p = c + 1;
      }
    }
    if (takes_arg(a)) {
      if (i + 1 >= argv.size()) return no(a + " without value");
      const std::string& v = argv[i + 1];
      if ((a == "-Xclang" || a == "-mllvm" || a == "-Xpreprocessor") && pending_bad.empty()) {
        if (const char* w = reads_or_writes(v)) pending_bad = a + " " + v + " (" + w + ")";
        std::string lv = v; std::transform(lv.begin(), lv.end(), lv.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lv.find("file") != std::string::npos || lv.find("path") != std::string::npos || lv.find("list") != std::string::npos)
          pending_bad = a + " " + v + " (names a file)";
      }
      ++i; continue;
    }
    if (!a.empty() && a[0] == '-') continue;
    s.looks_like_probe = false;
    return no("positional argument " + a);   // a real source / object / library: not a probe
  }
  if (ninputs == 0) { s.looks_like_probe = false; return no("no /dev/null or stdin input"); }
  if (ninputs > 1) return no("several inputs");
  if (!pending_bad.empty()) return no(pending_bad);
  if (s.mode == Mode::None) return no("no -c/-S/-E");
  if (!has_x) return no("no -x language");
  if (s.mode != Mode::Preprocess && s.output.empty()) return no("no -o (default output name)");
  if (starts(s.output, "/dev/") && s.output != "/dev/null") return no("output to device " + s.output);
  s.ok = true;
  return s;
}

std::string config_identity(const std::string& resolved) {
  std::string out;
  auto scan_dir = [&](const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) { out += "|" + dir + ":absent"; return; }
    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) {
      const size_t n = std::strlen(e->d_name);
      if (n > 4 && std::strcmp(e->d_name + n - 4, ".cfg") == 0) names.emplace_back(e->d_name);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
      const direct::FStat f = direct::stat_path(dir + "/" + n);
      char buf[160];
      std::snprintf(buf, sizeof buf, ":%llu:%llu:%llu:%lld:%lld", (unsigned long long)f.dev, (unsigned long long)f.ino,
                    (unsigned long long)f.size, (long long)f.mtime_ns, (long long)f.ctime_ns);
      out += "|" + dir + "/" + n + buf;
    }
  };
  scan_dir(util::dirname(resolved));
  scan_dir("/etc/clang");
  return out;
}

std::vector<std::string> env() {
  std::vector<std::string> e = direct::relevant_env();
  static const char* extra[] = {"PATH",   // the driver locates its tools through PATH
                                "LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LC_CTYPE", "CLANG_NO_DEFAULT_CONFIG", "GCC_COLORS",
                                "DEVELOPER_DIR", "TOOLCHAINS"};   // macOS: xcrun shims select the real compiler by these
  for (const char* n : extra) if (const char* v = std::getenv(n)) e.emplace_back(std::string(n) + "=" + v);
  return e;
}

std::string key(const std::string& cid, const std::string& cfg, const std::vector<std::string>& argv, const Shape& s,
                const std::string& cwd, const std::vector<std::string>& envv, const std::string& input_sha) {
  std::string k = "opticc-probe-v1|C|"; k += cid; k += "|G|"; k += cfg;
  k += "|M|"; k += std::to_string(static_cast<int>(s.mode)); k += "|A|";
  for (size_t i = 0; i < argv.size(); ++i) {
    if (static_cast<int>(i) == s.output_index) {
      // the output path does not enter the key, only its kind (stdout, discarded, or a file)
      k += s.output == "-" ? "\x01STDOUT" : s.output == "/dev/null" ? "\x01NULL" : "\x01OUT";
    } else {
      k += argv[i];
    }
    k.push_back('\0');
  }
  k += "|W|"; k += cwd; k += "|E|";
  for (auto& e : envv) { k += e; k.push_back('\0'); }
  k += "|I|"; k += input_sha;
  return util::sha256_hex(k);
}

bool input_reads_files(const std::string& c) {
  static const char* words[] = {"include", "import", "incbin", "embed"};
  for (const char* w : words) if (c.find(w) != std::string::npos) return true;
  return false;
}

namespace {
struct W {
  std::string b;
  template <class T> void num(T v) { b.append(reinterpret_cast<const char*>(&v), sizeof v); }
  void str(const std::string& s) { num<uint32_t>(static_cast<uint32_t>(s.size())); b += s; }
};
struct R {
  const std::string& b; size_t i = 0; bool ok = true;
  explicit R(const std::string& s) : b(s) {}
  template <class T> T num() { T v{}; if (!ok || i + sizeof v > b.size()) { ok = false; return v; } std::memcpy(&v, b.data() + i, sizeof v); i += sizeof v; return v; }
  std::string str() { uint32_t n = num<uint32_t>(); if (!ok || i + n > b.size()) { ok = false; return {}; } std::string s = b.substr(i, n); i += n; return s; }
};
}  // namespace

std::string encode(const Result& r) {
  W w; w.b = "OPP1"; w.num<int32_t>(r.rc); w.str(r.out); w.str(r.err); w.num<uint8_t>(r.has_obj ? 1 : 0); w.str(r.obj);
  return w.b;
}

bool decode(const std::string& blob, Result* r) {
  if (blob.size() < 4 || blob.compare(0, 4, "OPP1") != 0) return false;
  R rd(blob); rd.i = 4;
  r->rc = rd.num<int32_t>(); r->out = rd.str(); r->err = rd.str(); r->has_obj = rd.num<uint8_t>() != 0; r->obj = rd.str();
  return rd.ok && rd.i == blob.size();
}

bool cacheable_failure(const std::string& err) {
  if (err.empty()) return false;
  std::string l = err; std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  static const char* env_causes[] = {"unable to open", "no such file", "permission denied", "no space", "read-only file system",
                                     "cannot open", "can't open", "too many open files", "resource temporarily", "killed",
                                     "out of memory", "cannot allocate", "stack dump", "please submit", "unable to execute",
                                     "error: unable to", "input/output error", "crash", "bus error", "segmentation fault",
                                     "interrupted", "file exists", "disk quota", "is a directory", "not a directory",
                                     "failed to", "error opening", "error writing", "error reading", "cannot create",
                                     "could not", "timed out", "broken pipe", "terminated", "signal"};
  for (const char* c : env_causes) if (l.find(c) != std::string::npos) return false;
  return true;
}

std::string entry_path(const std::string& dir, const std::string& pk) { return dir + "/p/" + pk.substr(0, 2) + "/" + pk; }

std::string lang_of(const std::vector<std::string>& argv) {
  for (size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == "-x" && i + 1 < argv.size()) return argv[i + 1];
    if (argv[i].size() > 2 && argv[i].compare(0, 2, "-x") == 0) return argv[i].substr(2);
  }
  return "c";
}

static std::string stat_str(const std::string& p) {
  const direct::FStat f = direct::stat_path(p);
  char b[160];
  std::snprintf(b, sizeof b, ":%d:%llu:%llu:%llu:%lld:%lld", f.exists ? 1 : 0, (unsigned long long)f.dev, (unsigned long long)f.ino,
                (unsigned long long)f.size, (long long)f.mtime_ns, (long long)f.ctime_ns);
  return p + b;
}

bool external_assembler(const std::string& resolved, const std::string& lang, const std::string& memo_dir) {
  // the driver's own plan (-###) for a -c of an empty input in this language: a job whose program is an assembler
  // ("as", "*-as", "gas", ...) means an external tool the key does not pin. Memoised per compiler identity + language.
  const std::string memo = memo_dir + "/" + util::sha256_hex("as|" + direct::compiler_identity(resolved) + "|" + lang).substr(0, 32);
  try { const std::string v = util::read_file(memo); if (v == "ext\n") return true; if (v == "int\n") return false; } catch (...) {}
  bool ext = true;   // unknown -> treated as external (not cached)
  try {
    const std::string t = util::make_temp_dir("opticc-pa-");
    util::RunResult rr = util::run_process({resolved, "-###", "-c", "-x", lang, "/dev/null", "-o", t + "/x.o"}, t + "/out", t + "/err", "/");
    std::string plan; try { plan = util::read_file(t + "/err"); } catch (...) {}
    try { plan += util::read_file(t + "/out"); } catch (...) {}
    util::remove_tree(t);
    if (rr.rc == 0) {
      ext = false;
      std::istringstream is(plan); std::string line;
      while (std::getline(is, line)) {
        size_t b = line.find_first_not_of(' '); if (b == std::string::npos || line[b] != '"') continue;
        size_t e = line.find('"', b + 1); if (e == std::string::npos) continue;
        std::string prog = line.substr(b + 1, e - b - 1); auto sl = prog.rfind('/'); if (sl != std::string::npos) prog = prog.substr(sl + 1);
        if (prog == "as" || prog == "gas" || (prog.size() > 3 && prog.compare(prog.size() - 3, 3, "-as") == 0)) { ext = true; break; }
      }
      // GCC prints its sub-programs without quotes: " /usr/lib/gcc/.../as ..." or "as ..."
      if (!ext && plan.find("collect2") == std::string::npos) {
        std::istringstream is2(plan); std::string l2;
        while (std::getline(is2, l2)) {
          size_t b = l2.find_first_not_of(' '); if (b == std::string::npos) continue;
          std::string w = l2.substr(b, l2.find(' ', b) == std::string::npos ? std::string::npos : l2.find(' ', b) - b);
          auto sl = w.rfind('/'); if (sl != std::string::npos) w = w.substr(sl + 1);
          if (w == "as" || (w.size() > 3 && w.compare(w.size() - 3, 3, "-as") == 0)) { ext = true; break; }
        }
      }
    }
  } catch (...) { ext = true; }
  try { util::mkdir_p(memo_dir); util::write_file(memo, ext ? "ext\n" : "int\n"); } catch (...) {}
  return ext;
}

std::string implicit_identity(const std::string& resolved, const std::string& lang, const std::string& memo_dir) {
  if (lang == "assembler") return "none";   // no preprocessor: nothing is read implicitly
  // memo: line 1 = the system include directories, then the implicitly read files (from `-M` of an empty input)
  const std::string memo = memo_dir + "/" + util::sha256_hex(direct::compiler_identity(resolved) + "|" + lang).substr(0, 32);
  std::vector<std::string> dirs, files;
  bool have = false;
  try {
    std::istringstream is(util::read_file(memo)); std::string line;
    if (std::getline(is, line) && line == "opticc-implicit-v1") {
      while (std::getline(is, line)) {
        if (line.compare(0, 2, "D ") == 0) dirs.push_back(line.substr(2));
        else if (line.compare(0, 2, "F ") == 0) files.push_back(line.substr(2));
      }
      have = !dirs.empty();
    }
  } catch (...) {}
  if (!have) {
    dirs = closure::system_include_dirs(resolved, lang == "c++" ? "c++" : "c", "/");
    try {
      const std::string t = util::make_temp_dir("opticc-pi-");
      util::RunResult rr = util::run_process({resolved, "-x", lang, "-M", "/dev/null"}, t + "/out", t + "/err", "/");
      std::string dep; try { dep = util::read_file(t + "/out"); } catch (...) {}
      util::remove_tree(t);
      if (rr.rc != 0) return "";   // unknown: the caller does not cache
      for (auto& f : closure::parse_depfile(dep)) if (f != "/dev/null") files.push_back(f);
    } catch (...) { return ""; }
    if (dirs.empty()) dirs.emplace_back("/usr/include");
    try {
      util::mkdir_p(memo_dir);
      std::string s = "opticc-implicit-v1\n";
      for (auto& d : dirs) s += "D " + d + "\n";
      for (auto& f : files) s += "F " + f + "\n";
      util::write_file(memo + ".tmp", s); std::rename((memo + ".tmp").c_str(), memo.c_str());
    } catch (...) {}
  }
  // the files read implicitly (stdc-predef.h with glibc) and the directories a new implicit file would appear in
  std::string id;
  for (auto& f : files) id += "|F" + stat_str(f);
  for (auto& d : dirs) id += "|D" + stat_str(d);
  return util::sha256_hex(id);
}

}  // namespace opticc::probe
