#include <cctype>
// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/cachekey.hpp"
#include <cstring>
#include <initializer_list>
#include <future>
#include <sstream>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "opticc/util.hpp"

namespace opticc::cachekey {
namespace fs = std::filesystem;

std::string relativise(const std::string& path, const std::string& cwd) {
  if (cwd.empty() || path.empty()) return path;
  std::string c = cwd; if (c.back() != '/') c.push_back('/');
  if (path.rfind(c, 0) == 0) return path.substr(c.size());
  if (path == cwd) return ".";
  return path;
}

// An assembler file directive reads a file that no key and no dependency file lists.
// Assembler input: `.incbin` anywhere, `.include` as a directive. C/C++ (a header, or preprocessed text): `.incbin` or
// `.include` with an `asm`/`__asm`/`__asm__` token in the 2 KiB before it (inline assembly) — a string such as LLVM's
// DirectiveKindMap[".incbin"] alone does not count, so the large assembler-parser TUs stay cacheable.
static bool asm_token_before(const std::string& c, size_t p) {
  const size_t b = p > 2048 ? p - 2048 : 0;
  for (size_t q = c.find("asm", b); q != std::string::npos && q < p; q = c.find("asm", q + 1)) {
    size_t s0 = q; while (s0 > b && c[s0 - 1] == '_') --s0;
    const bool left = s0 == 0 || !(std::isalnum(static_cast<unsigned char>(c[s0 - 1])) || c[s0 - 1] == '_');
    size_t e = q + 3; while (e < c.size() && c[e] == '_') ++e;
    const bool right = e >= c.size() || !(std::isalnum(static_cast<unsigned char>(c[e])) || c[e] == '_');
    if (left && right) return true;
  }
  return false;
}
static bool include_directive_at_line(const std::string& c) {
  for (size_t p = c.find(".include"); p != std::string::npos; p = c.find(".include", p + 1)) {
    const size_t e = p + 8;
    if (e < c.size() && !std::isspace(static_cast<unsigned char>(c[e])) && c[e] != '"') continue;   // .included, .include_x
    size_t b = p; while (b > 0 && (c[b - 1] == ' ' || c[b - 1] == '\t')) --b;
    if (b == 0 || c[b - 1] == '\n' || c[b - 1] == ';' || c[b - 1] == '"') return true;
  }
  return false;
}
// C/C++ comments blanked (string and character literals kept): a directive named in a comment reads nothing (the kernel's
// nospec-branch.h says `asm(".include <asm/nospec-branch.h>")` in a comment, and nearly every x86 TU includes it)
static std::string blank_c_comments(const std::string& c) {
  std::string o = c;
  enum { N, STR, CHR, LINE, BLOCK } st = N;
  for (size_t i = 0; i < o.size(); ++i) {
    const char ch = c[i], nx = i + 1 < c.size() ? c[i + 1] : '\0';
    switch (st) {
      case N:
        if (ch == '"') st = STR;
        else if (ch == '\'') st = CHR;
        else if (ch == '/' && nx == '/') { st = LINE; o[i] = ' '; }
        else if (ch == '/' && nx == '*') { st = BLOCK; o[i] = ' '; o[i + 1] = ' '; ++i; }
        break;
      case STR: if (ch == '\\') ++i; else if (ch == '"' || ch == '\n') st = N; break;
      case CHR: if (ch == '\\') ++i; else if (ch == '\'' || ch == '\n') st = N; break;
      case LINE: if (ch == '\n') st = N; else o[i] = ' '; break;
      case BLOCK: if (ch == '*' && nx == '/') { o[i] = ' '; o[i + 1] = ' '; ++i; st = N; } else if (ch != '\n') o[i] = ' '; break;
    }
  }
  return o;
}

bool file_directive(const std::string& c0, bool assembler_input) {
  if (assembler_input) return c0.find(".incbin") != std::string::npos || include_directive_at_line(c0);
  if (c0.find(".incbin") == std::string::npos && c0.find(".include") == std::string::npos) return false;   // the common case: no scan
  const std::string c = blank_c_comments(c0);
  for (const char* d : {".incbin", ".include"})
    for (size_t p = c.find(d); p != std::string::npos; p = c.find(d, p + 1)) {
      const size_t e = p + std::strlen(d);
      if (e < c.size() && (std::isalnum(static_cast<unsigned char>(c[e])) || c[e] == '_')) continue;   // .included, .incbin_x
      if (asm_token_before(c, p)) return true;
    }
  return false;
}

bool argv_uses_time_macros(const std::vector<std::string>& argv) {
  for (size_t i = 1; i < argv.size(); ++i) {
    std::string def;
    if (argv[i] == "-D" && i + 1 < argv.size()) def = argv[i + 1];
    else if (argv[i].rfind("-D", 0) == 0) def = argv[i].substr(2);
    else continue;
    const auto eq = def.find('=');
    if (eq != std::string::npos && uses_time_macros(def.substr(eq + 1) + "\n")) return true;
  }
  return false;
}

bool dep_lists_system_headers(const std::vector<std::string>& argv) {
  bool full = false;
  for (auto& a : argv) {
    if (a == "-MD" || a == "-M") full = true;
    else if (a == "-MMD" || a == "-MM") return false;
    else if (a.rfind("-Wp,", 0) == 0) {
      size_t i = 4;
      while (i <= a.size()) {
        size_t c = a.find(',', i); std::string t = a.substr(i, c == std::string::npos ? std::string::npos : c - i);
        if (t == "-MD" || t == "-M") full = true; else if (t == "-MMD" || t == "-MM") return false;
        if (c == std::string::npos) break; i = c + 1;
      }
    }
  }
  return full;
}

bool debug_info(const std::vector<std::string>& argv) {
  bool dbg = false;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a.rfind("-g", 0) == 0 && a != "-g0" && a.rfind("-gno-", 0) != 0 && a != "-gz" && a.rfind("-gz=", 0) != 0) dbg = true;
    if (a == "-g0") dbg = false;
  }
  return dbg;
}

std::vector<std::string> normalise_argv(const std::vector<std::string>& argv, int source_index, const std::string& cwd) {
  static const char* drop_with_value[] = {"-o", "-MF", "-MT", "-MQ"};
  static const char* path_flags[] = {"-I", "-iquote", "-isystem", "-idirafter", "-include", "-imacros", "-iprefix", "-isysroot", "--sysroot"};
  std::vector<std::string> out;
  for (size_t i = 0; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (i == 0) { out.push_back(util::basename(a)); continue; }   // compiler name only; identity is the compiler id
    // the source as SPELLED (__FILE__, and the directory quoted includes resolve from)
    if (static_cast<int>(i) == source_index) { out.push_back("$SRC=" + a); continue; }
    bool skip = false;
    for (const char* f : drop_with_value) {
      std::string flag(f);
      if (a == flag) { ++i; skip = true; break; }
      if (a.rfind(flag, 0) == 0 && a.size() > flag.size() && flag != "-o") { skip = true; break; }
      if (flag == "-o" && a.rfind("-o", 0) == 0 && a.size() > 2 && a[2] != 'p' /* not -openmp etc. */ && a.find('=') == std::string::npos && a.find('-', 1) == std::string::npos) { skip = true; break; }
    }
    if (skip) continue;
    if (a == "-MD" || a == "-MMD" || a == "-MP" || a == "-H") continue;
    if (a.rfind("-Wp,", 0) == 0 && a.find("-M") != std::string::npos) continue;
    bool handled = false;
    for (const char* f : path_flags) {
      std::string flag(f);
      if (a == flag) { out.push_back(a); if (i + 1 < argv.size()) out.push_back(argv[i + 1]); ++i; handled = true; break; }
      if (a.rfind(flag, 0) == 0 && a.size() > flag.size() && (flag == "-I" || flag.size() > 2)) { out.push_back(a); handled = true; break; }
    }
    if (handled) continue;
    out.push_back(a);   // prefix maps verbatim as well
  }
  // with debug info the object records the compile directory (DW_AT_comp_dir): two checkouts of one
  // layout must not share objects (ccache: hash_dir). Without debug info cwd stays out (identical layouts share).
  if (debug_info(argv)) out.push_back("$CWD=" + cwd);
  return out;
}

std::string resolve_compiler(const std::string& compiler) {
  if (compiler.find('/') != std::string::npos) { std::error_code ec; auto p = fs::weakly_canonical(compiler, ec); return ec ? compiler : p.string(); }
  const char* path = std::getenv("PATH");
  std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
  size_t pos = 0;
  while (pos <= p.size()) {
    size_t c = p.find(':', pos); if (c == std::string::npos) c = p.size();
    std::string dir = p.substr(pos, c - pos); pos = c + 1;
    if (dir.empty()) continue;
    std::string cand = dir + "/" + compiler;
    if (access(cand.c_str(), X_OK) == 0) { std::error_code ec; auto r = fs::weakly_canonical(cand, ec); return ec ? cand : r.string(); }
  }
  return compiler;
}

// What the driver runs besides itself. gcc's cc1/cc1plus and the assembler (found
// through PATH) and the shared libraries the driver loads (Debian's clang: libLLVM, libclang-cpp) change the object
// without changing the driver's bytes or its --version line. Their CONTENTS are hashed (the identity is compared across
// machines); a name the driver cannot resolve ("cc1" for clang, which compiles in-process) adds nothing.
static std::string resolve_in_path(const std::string& name) {
  if (name.empty()) return {};
  if (name.find('/') != std::string::npos) return util::file_exists(name) ? name : std::string();
  const char* path = std::getenv("PATH");
  std::string p = path ? path : "/usr/bin:/bin";
  size_t i = 0;
  while (i <= p.size()) {
    size_t j = p.find(':', i); if (j == std::string::npos) j = p.size();
    std::string c = p.substr(i, j - i) + "/" + name;
    if (!p.substr(i, j - i).empty() && ::access(c.c_str(), X_OK) == 0) return c;
    i = j + 1;
  }
  return {};
}

// output of `cmd`; *ok = it ran and exited with a status in `accept` (spawn failures, signals, other statuses: false)
static std::string run_capture(const std::vector<std::string>& cmd, std::initializer_list<int> accept, bool* ok) {
  std::string out; *ok = false;
  try {
    std::string tmp = util::make_temp_dir("opticc-cid-");
    util::RunResult rr = util::run_process(cmd, tmp + "/out", tmp + "/err", "/", nullptr);
    bool good = !rr.signalled; if (good) { good = false; for (int a : accept) if (rr.rc == a) good = true; }
    if (good && util::file_exists(tmp + "/out")) { out = util::read_file(tmp + "/out"); *ok = true; }
    util::remove_tree(tmp);
  } catch (...) { *ok = false; }
  return out;
}

std::string toolchain_parts(const std::string& real, bool* ok) {
  bool all = true;
  std::string s;
  auto add_file = [&](const std::string& tag, const std::string& path) {
    if (path.empty()) return;
    std::string h;
    try { h = util::sha256_hex(util::read_file(path)); } catch (...) { all = false; return; }
    s += "|" + tag + ":" + util::basename(path) + ":" + h;
  };
  for (const char* prog : {"cc1", "cc1plus", "as"}) {
    bool r = false;
    std::string o = run_capture({real, std::string("-print-prog-name=") + prog}, {0}, &r);
    if (!r) { all = false; continue; }   // gcc and clang always answer: a failure is not an answer
    while (!o.empty() && (o.back() == '\n' || o.back() == '\r' || o.back() == ' ')) o.pop_back();
    if (o.empty() || o == prog) { if (std::string(prog) == "as") add_file("as", resolve_in_path("as")); continue; }   // not a separate program
    add_file(prog, resolve_in_path(o));
  }
  std::vector<std::string> libs;
#ifdef __APPLE__
  bool r = false;
  const std::string ld = run_capture({"/usr/bin/otool", "-L", real}, {0}, &r);
  if (!r) all = false;
  std::istringstream ls(ld); std::string line; bool first = true;
  while (std::getline(ls, line)) {
    if (first) { first = false; continue; }   // "<binary>:"
    size_t a = line.find_first_not_of(" \t"), b = line.find(" (compatibility");
    if (a == std::string::npos || b == std::string::npos || b <= a) continue;
    std::string p = line.substr(a, b - a);
    if (p.rfind("/usr/lib/", 0) == 0 || p.rfind("/System/", 0) == 0) continue;   // the OS's libraries (dyld shared cache)
    libs.push_back(p);
  }
#else
  bool r = false;   // ldd exits 1 for a static binary or a script ("not a dynamic executable"): an answer, no libraries
  const std::string ld = run_capture({"ldd", real}, {0, 1}, &r);
  if (!r) all = false;
  std::istringstream ls(ld); std::string line;
  while (std::getline(ls, line)) {
    if (line.find("not found") != std::string::npos) { all = false; continue; }
    size_t arrow = line.find("=> ");
    std::string p;
    if (arrow != std::string::npos) { size_t e = line.find(" (", arrow); p = line.substr(arrow + 3, e == std::string::npos ? std::string::npos : e - arrow - 3); }
    else { size_t a = line.find_first_not_of(" \t"); size_t e = line.find(" (", a); if (a != std::string::npos && line[a] == '/') p = line.substr(a, e == std::string::npos ? std::string::npos : e - a); }
    if (!p.empty() && p[0] == '/') libs.push_back(p);
  }
#endif
  std::sort(libs.begin(), libs.end()); libs.erase(std::unique(libs.begin(), libs.end()), libs.end());
  for (auto& l : libs) add_file("lib", l);
  if (ok) *ok = all;
  return s;
}

static std::string compute_compiler_id(const std::string& real, bool have) {
  std::string ident = "name:" + util::basename(real);
  if (!have) return {};
  try { ident += "|bin:" + util::sha256_hex(util::read_file(real)); } catch (...) { return {}; }
  // first line of --version; retried (macOS xcrun shims fail transiently under load)
  std::string ver;
  for (int attempt = 0; attempt < 3 && ver.empty(); ++attempt) {
    bool r = false;
    ver = run_capture({real, "--version"}, {0}, &r);
    if (!r) ver.clear();
    auto nl = ver.find('\n'); if (nl != std::string::npos) ver = ver.substr(0, nl);
    if (ver.empty() && attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (ver.empty()) return {};
  ident += "|ver:" + ver;
  bool parts_ok = false;
  ident += toolchain_parts(real, &parts_ok);
  if (!parts_ok) return {};
  return util::sha256_hex(ident);
}

// Computed ONCE per process and compiler (stat key): the first caller computes,
// concurrent callers wait for it (so a first wave of jobs does not hash the toolchain once each). Any failed helper
// query gives "" — never cached (the next call retries), and every caller treats "" as "no cache for this job" (a
// failed query must not become a different, cached identity that silently disables sharing for the process).
std::string compiler_id(const std::string& compiler) {
  static std::mutex mu;
  static std::unordered_map<std::string, std::shared_future<std::string>> cache;   // key: resolved path | mtime | size
  std::string real = resolve_compiler(compiler);
  struct stat st{}; bool have = ::stat(real.c_str(), &st) == 0;
  std::string key = real + "|" + (have ? std::to_string(static_cast<long long>(st.st_mtime)) + "|" + std::to_string(static_cast<long long>(st.st_size)) : "?");
  std::promise<std::string> prom; std::shared_future<std::string> fut; bool mine = false;
  {
    std::lock_guard<std::mutex> g(mu);
    auto it = cache.find(key);
    if (it != cache.end()) fut = it->second;
    else { fut = prom.get_future().share(); cache.emplace(key, fut); mine = true; }
  }
  if (!mine) return fut.get();
  std::string id;
  try { id = compute_compiler_id(real, have); } catch (...) { id.clear(); }
  prom.set_value(id);
  if (id.empty()) { std::lock_guard<std::mutex> g(mu); auto it = cache.find(key); if (it != cache.end()) cache.erase(it); }
  return id;
}

static std::string join(const std::vector<std::string>& v) { std::string s; for (auto& x : v) { s += x; s.push_back('\0'); } return s; }

std::string manifest_key(const std::string& cid, const std::vector<std::string>& norm_argv, const std::string& src_sha) {
  return util::sha256_hex(std::string("opticc-cache-v1|M|") + cid + "|" + join(norm_argv) + "|" + src_sha);
}

std::string full_key(const std::string& mkey, const std::vector<std::pair<std::string, std::string>>& view) {
  std::vector<std::pair<std::string, std::string>> v = view; std::sort(v.begin(), v.end());
  std::string s = "opticc-cache-v1|F|" + mkey;
  for (auto& [p, h] : v) { s.push_back('\0'); s += p; s.push_back('\1'); s += h; }
  return util::sha256_hex(s);
}

std::string object_key(const std::string& mkey, Closure closure, const std::string& cwd) {
  for (auto& [p, s] : closure) p = relativise(p, cwd);
  std::sort(closure.begin(), closure.end());
  std::string s = std::string("opticc-cache-v1|O|") + mkey;
  for (auto& [p, h] : closure) { s.push_back('\0'); s += p; s += "="; s += h; }
  return util::sha256_hex(s);
}

std::string classic_key(const std::string& cid, const std::vector<std::string>& norm_argv, const std::string& pre_sha) {
  return util::sha256_hex(std::string("opticc-cache-v1|C|") + cid + "|" + join(norm_argv) + "|" + pre_sha);
}


std::string content_key_sha(const std::string& in, bool* normalised) {
  auto raw = [&]() { if (normalised) *normalised = false; return util::sha256_hex(in); };
  std::string out; out.reserve(in.size());
  const size_t n = in.size();
  size_t i = 0;
  auto idch = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; };
  // A comment whose CONTENT can change a diagnostic (and, under -Werror, the exit status)
  // must not be ignored: '/*' inside a block comment (-Wcomment, in -Wall), fall-through markers (GCC
  // -Wimplicit-fallthrough reads comments), and any non-ASCII byte (bidirectional-control and invalid-UTF-8
  // warnings in comments). Such a file falls back to the raw hash. Doxygen comments (-Wdocumentation) are handled by
  // keeps_comments(): the flag is visible there.
  auto risky_comment = [](const std::string& c) {
    if (c.find("/*", 2) != std::string::npos && c.size() > 2 && c[0] == '/' && c[1] == '*') return true;
    std::string l; l.reserve(c.size());
    for (char x : c) { if (static_cast<unsigned char>(x) >= 0x80) return true; l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(x)))); }
    for (size_t k = l.find("fall"); k != std::string::npos; k = l.find("fall", k + 1)) {
      size_t j = k + 4; if (j < l.size() && l[j] == 's') ++j;
      while (j < l.size() && (l[j] == ' ' || l[j] == '-' || l[j] == '_' || l[j] == '\t')) ++j;
      if (l.compare(j, 3, "thr") == 0) return true;
    }
    return false;
  };
  while (i < n) {
    char ch = in[i];
    // trigraphs (??/ etc.) are rare in practice; not modelled -> raw hash
    if (ch == '?' && i + 2 < n && in[i + 1] == '?') return raw();
    // an identifier is one token: copied verbatim. A raw-string prefix (R, u8R, uR, UR, LR) directly followed by '"'
    // starts a raw string literal, whose content may contain anything -> raw hash.
    if ((std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || static_cast<unsigned char>(ch) >= 0x80)) {
      size_t j = i; while (j < n && idch(in[j])) ++j;
      const std::string id = in.substr(i, j - i);
      if (j < n && in[j] == '"' && (id == "R" || id == "u8R" || id == "uR" || id == "UR" || id == "LR")) return raw();
      out.append(id); i = j; continue;
    }
    // a pp-number is one token: digits, letters, '_', '.', exponent signs, and C++14 digit separators ('), copied
    // verbatim, so 1'000 never opens a character literal (e.g. "int a = 1'000; ... "x'y"")
    if (std::isdigit(static_cast<unsigned char>(ch)) || (ch == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(in[i + 1])))) {
      size_t j = i + 1;
      while (j < n) {
        const char c = in[j];
        if ((c == '+' || c == '-') && (in[j - 1] == 'e' || in[j - 1] == 'E' || in[j - 1] == 'p' || in[j - 1] == 'P')) { ++j; continue; }
        if (c == '\'' && j + 1 < n && idch(in[j + 1])) { j += 2; continue; }
        if (idch(c) || c == '.') { ++j; continue; }
        break;
      }
      out.append(in, i, j - i); i = j; continue;
    }
    if (ch == '"' || ch == '\'') {   // string / char literal: copy verbatim, honouring escapes
      char q = ch; out.push_back(ch); ++i;
      while (i < n && in[i] != q) {
        if (in[i] == '\n') return raw();                 // unterminated literal (or a '<' header name with quotes): be safe
        if (in[i] == '\\' && i + 1 < n) { out.push_back(in[i]); ++i; }
        out.push_back(in[i]); ++i;
      }
      if (i >= n) return raw();
      out.push_back(in[i]); ++i;
      continue;
    }
    if (ch == '<' ) {
      // #include <a//b.h> or <a/*b.h>: a header name is not a comment. Detect '#include'/'#import' lines conservatively:
      size_t ls = in.rfind('\n', i ? i - 1 : 0); ls = (ls == std::string::npos || i == 0) ? 0 : ls + 1;
      size_t k = ls; while (k < i && (in[k] == ' ' || in[k] == '\t')) ++k;
      if (k < i && in[k] == '#') {
        size_t e = in.find('\n', i); if (e == std::string::npos) e = n;
        std::string line = in.substr(i, e - i);
        size_t gt = line.find('>');
        if (gt != std::string::npos) { out.append(line, 0, gt + 1); i += gt + 1; continue; }
      }
    }
    if (ch == '/' && i + 1 < n && in[i + 1] == '/') {   // line comment
      size_t e = in.find('\n', i); if (e == std::string::npos) e = n;
      if (risky_comment(in.substr(i, e - i))) return raw();
      // a backslash before the newline continues the comment onto the next line: not modelled
      size_t last = e; while (last > i && (in[last - 1] == '\r')) --last;
      if (last > i && in[last - 1] == '\\') return raw();
      out.append(e - i, ' '); i = e; continue;
    }
    if (ch == '/' && i + 1 < n && in[i + 1] == '*') {   // block comment
      size_t e = in.find("*/", i + 2); if (e == std::string::npos) return raw();
      if (risky_comment(in.substr(i, e + 2 - i))) return raw();
      for (size_t k = i; k < e + 2; ++k) {
        if (in[k] == '\\' && k + 1 < n && in[k + 1] == '\n') return raw();   // line splice inside a comment
        out.push_back(in[k] == '\n' ? '\n' : ' ');
      }
      i = e + 2; continue;
    }
    if (ch == '\\' && i + 1 < n && in[i + 1] == '\n') {
      // a line splice outside comments/literals is fine (copied verbatim) unless it joins into a comment start
      if (i + 3 < n && in[i + 2] == '/' && (in[i + 3] == '/' || in[i + 3] == '*')) return raw();
    }
    out.push_back(ch); ++i;
  }
  // strip trailing blanks on every line (no tokens there; a comment shortened or lengthened at end of line leaves only
  // blanks). A line that then ends in a backslash would change its splice semantics: not modelled -> raw hash.
  {
    std::string t; t.reserve(out.size());
    size_t a = 0;
    while (a <= out.size()) {
      size_t e = out.find('\n', a); bool nl = e != std::string::npos; if (!nl) e = out.size();
      size_t z = e; while (z > a && (out[z - 1] == ' ' || out[z - 1] == '\t' || out[z - 1] == '\r')) --z;
      if (z < e && z > a && out[z - 1] == '\\') return raw();
      t.append(out, a, z - a); if (nl) t.push_back('\n');
      if (!nl) break;
      a = e + 1;
    }
    out.swap(t);
  }
  // drop whitespace-only lines at the end of the file (an appended comment leaves exactly that)
  size_t keep = out.size();
  while (keep > 0 && (out[keep - 1] == ' ' || out[keep - 1] == '\t' || out[keep - 1] == '\n' || out[keep - 1] == '\r')) --keep;
  // keep the final newline structure of the last real line identical: content up to the last token + one '\n'
  out.resize(keep); out.push_back('\n');
  if (normalised) *normalised = true;
  // The presence of the final newline is kept — a file without one is diagnosed (-Wnewline-eof,
  // -pedantic-errors in C), so with -Werror the exit status differs. An appended comment line ends in '\n': unaffected.
  const bool eol = in.empty() || in.back() == '\n';
  // N drops trailing whitespace, which GCC 15's -Wtrailing-whitespace (with -Werror) turns into
  // an exit status. The key keeps whether ANY physical line of the original ends in whitespace (a "\r\n" ending counts
  // as the newline); a comment edit that leaves a line ending in a token or a comment keeps the marker unchanged.
  bool tw = false;
  for (size_t k = 0; k <= n && !tw; ++k) {
    if (k < n && in[k] != '\n') continue;
    size_t e = k; if (e > 0 && e <= n && e - 1 < n && in[e - 1] == '\r' && k < n) --e;
    if (e > 0) { const char c = in[e - 1]; tw = c == ' ' || c == '\t' || c == '\f' || c == '\v'; }
  }
  return std::string("n") + (eol ? "" : "0") + (tw ? "w" : "") + ":" + util::sha256_hex(out);
}

bool keeps_comments(const std::vector<std::string>& argv) {
  // Comments kept in the preprocessed output (-C/-CC), or ANY debug info: DWARF 5 line tables carry an MD5 of each
  // source file (clang's default), so a comment edit changes the object — the raw hash must be used (see the
  // test content_key_equal_implies_identical_object). -g0 disables debug info.
  // Assembler compiles always use the raw hash: '#' and ';' start comments in assembly (cpp leaves them), and a
  // byte the C tokenizer treats as comment or whitespace can be significant to the assembler.
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-x" && i + 1 < argv.size() && argv[i + 1].rfind("assembler", 0) == 0) return true;
    if (a.empty() || a[0] == '-') continue;
    const std::string& prev = argv[i - 1];
    if (prev == "-o" || prev == "-MF" || prev == "-MT" || prev == "-MQ" || prev == "-x") continue;
    const auto dot = a.rfind('.');
    if (dot != std::string::npos && (a.compare(dot, std::string::npos, ".S") == 0 || a.compare(dot, std::string::npos, ".s") == 0 ||
                                     a.compare(dot, std::string::npos, ".sx") == 0)) return true;
  }
  for (auto& a : argv) {
    if (a == "-C" || a == "-CC" || a == "-Wp,-C" || a == "-Wp,-CC") return true;
    // UBSan/ASan handler data, sanitizer-coverage tables and OpenMP ident strings embed
    // the physical COLUMNS of the original source, which the comment-insensitive key (and -E) does not keep: raw hash
    if (a.rfind("-fsanitize=", 0) == 0 || a.rfind("-fsanitize-coverage", 0) == 0 || a.rfind("-fopenmp", 0) == 0) return true;
    // clang parses Doxygen comments for -Wdocumentation (also enabled by -Weverything): comment text can
    // then change diagnostics, so the raw hash is used
    if (a == "-Weverything" || (a.rfind("-Wdocumentation", 0) == 0) || a.rfind("-Werror=documentation", 0) == 0) return true;
  }
  return debug_info(argv);
}

// __DATE__ / __TIME__ / __TIMESTAMP__ used as identifiers outside comments and literals. A cheap
// substring test first (almost every file has none); then a scan that skips comments, string and character literals
// (a ' inside a pp-number is a C++14 digit separator, not a literal). Errs towards "yes": an unterminated block
// comment counts as a use, and line continuations are not modelled (the following line is scanned as code).
bool uses_time_macros(const std::string& t) {
  // A name formed by token pasting from two fragments (`__DA ## TE__`, CAT(__TIME, __)) is caught
  // through its fragments: an identifier of ≥ 4 characters that is a proper prefix starting "__" or a proper suffix
  // ending "__" of one of the three names. Every such fragment contains __DA, __TI, TE__, ME__ or MP__ (prefilter).
  static const char* const kNames[] = {"__DATE__", "__TIME__", "__TIMESTAMP__"};
  auto fragment = [](const std::string& id) {
    if (id.size() < 4) return false;
    for (const char* nm : kNames) {
      const std::string s(nm);
      if (id.size() >= s.size()) continue;
      if (id.compare(0, 2, "__") == 0 && s.compare(0, id.size(), id) == 0) return true;
      if (id.compare(id.size() - 2, 2, "__") == 0 && s.compare(s.size() - id.size(), id.size(), id) == 0) return true;
    }
    return false;
  };
  // A paste of up to 4 parts, `__ ## DATE ## __` or CAT3(__, DATE, __): consecutive identifiers
  // separated only by blanks, newlines, comments, commas or ## are concatenated (the last 2..4 of such a run) and tested
  // like one identifier. No substring prefilter (a split can leave any letters in any part); the scan is linear.
  // Residual: a name assembled through intermediate macros whose parts never stand next to each other in the text.
  std::vector<std::string> run; bool joinable = false;
  auto hit = [&](const std::string& id) { return id == "__DATE__" || id == "__TIME__" || id == "__TIMESTAMP__" || fragment(id); };
  const size_t n = t.size();
  auto idch = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; };
  size_t i = 0;
  while (i < n) {
    const char c = t[i];
    if (c == '/' && i + 1 < n && t[i + 1] == '/') { size_t e = t.find('\n', i); i = e == std::string::npos ? n : e; continue; }
    if (c == '/' && i + 1 < n && t[i + 1] == '*') { size_t e = t.find("*/", i + 2); if (e == std::string::npos) return true; i = e + 2; continue; }
    if (c == '"' || c == '\'') {
      joinable = false;
      size_t j = i + 1;
      while (j < n && t[j] != c && t[j] != '\n') { if (t[j] == '\\') ++j; ++j; }
      i = j + 1; continue;
    }
    if (idch(c)) {
      const bool num = std::isdigit(static_cast<unsigned char>(c));
      size_t j = i; while (j < n && (idch(t[j]) || (num && t[j] == '\''))) ++j;
      const std::string id = t.substr(i, j - i);
      if (hit(id)) return true;
      if (!num) {
        if (!joinable) run.clear();
        run.push_back(id); if (run.size() > 4) run.erase(run.begin());
        for (size_t k = 2; k <= run.size(); ++k) { std::string cat; for (size_t q = run.size() - k; q < run.size(); ++q) cat += run[q]; if (hit(cat)) return true; }
        joinable = true;
      } else joinable = false;
      i = j; continue;
    }
    if (c == '#' && i + 1 < n && t[i + 1] == '#') { i += 2; continue; }   // ## keeps a run joinable
    if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '\\')) joinable = false;
    ++i;
  }
  return false;
}

}  // namespace opticc::cachekey
