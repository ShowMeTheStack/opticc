// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/argscan.hpp"

#include <algorithm>

namespace opticc::args {

static bool ends_with(const std::string& s, const char* suf) {
  std::string t(suf); return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}
static std::string lang_of(const std::string& src) {
  if (ends_with(src, ".c")) return "c";
  if (ends_with(src, ".S") || ends_with(src, ".s") || ends_with(src, ".sx")) return "asm";
  if (ends_with(src, ".cc") || ends_with(src, ".cpp") || ends_with(src, ".cxx") || ends_with(src, ".C") || ends_with(src, ".c++") || ends_with(src, ".cp"))
    return "c++";
  if (ends_with(src, ".m")) return "objc";
  if (ends_with(src, ".mm")) return "objc++";
  return "";
}
static bool is_source(const std::string& a) { return !a.empty() && a[0] != '-' && !lang_of(a).empty(); }

std::string default_object_name(const std::string& source) {
  std::string b = source; auto slash = b.rfind('/'); if (slash != std::string::npos) b = b.substr(slash + 1);
  auto dot = b.rfind('.'); if (dot != std::string::npos) b = b.substr(0, dot);
  return b + ".o";
}
std::string preprocessed_suffix(const std::string& lang) { return lang == "c++" ? ".ii" : ".i"; }

static bool has_flag(const std::vector<std::string>& argv, const std::string& f) {
  for (auto& a : argv) if (a == f || (a.rfind("-Wp,", 0) == 0 && a.find(f + ",") != std::string::npos)) return true;
  return false;
}

// Flags that take a separate argument (so the next argv is not a source).
static bool takes_arg(const std::string& a) {
  static const char* k[] = {"-o", "-I", "-D", "-U", "-include", "-imacros", "-isystem", "-iquote", "-idirafter", "-iprefix",
                            "-MF", "-MT", "-MQ", "-x", "-Xclang", "-Xassembler", "-Xlinker", "-Xpreprocessor", "-target", "-arch",
                            "-isysroot", "--sysroot", "-L", "-l", "-u", "-z", "-T", "-wrapper", "-B", "-G"};
  for (auto s : k) if (a == s) return true;
  return false;
}

Scan scan(const std::vector<std::string>& argv) {
  Scan s;
  if (argv.empty()) { s.reason = "empty"; return s; }
  s.compiler = argv[0];
  bool has_c = false; int nsrc = 0; std::string xlang;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-c") { has_c = true; continue; }
    if (a == "-o") { if (i + 1 < argv.size()) { s.output = argv[i + 1]; s.output_index = static_cast<int>(i); ++i; } continue; }
    if (a == "-E" || a == "-S" || a == "-M" || a == "-MM") { s.reason = "not a -c compile (" + a + ")"; return s; }
    if (a == "-MD" || a == "-MMD") { s.has_md = true; continue; }
    if (a == "-MF") { s.has_mf = true; if (i + 1 < argv.size()) s.dep_file = argv[i + 1]; ++i; continue; }
    if (a == "-MT" || a == "-MQ") { s.has_mt = true; if (i + 1 < argv.size()) s.dep_target = argv[i + 1]; ++i; continue; }
    if (a.rfind("-MF", 0) == 0 && a.size() > 3) { s.has_mf = true; s.dep_file = a.substr(3); continue; }
    if ((a.rfind("-MT", 0) == 0 || a.rfind("-MQ", 0) == 0) && a.size() > 3) { s.has_mt = true; s.dep_target = a.substr(3); continue; }
    if (a.rfind("-Wp,", 0) == 0) {  // kbuild: -Wp,-MMD,<file>  (also -Wp,-MD,<file>)
      std::string rest = a.substr(4);
      size_t pos = 0;
      while (pos <= rest.size()) {
        size_t c = rest.find(',', pos); std::string tok = rest.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        if (tok == "-MD" || tok == "-MMD") {
          s.has_md = true;
          if (c != std::string::npos) { size_t c2 = rest.find(',', c + 1); s.dep_file = rest.substr(c + 1, c2 == std::string::npos ? std::string::npos : c2 - c - 1); s.has_mf = true; c = c2; }
        } else if (tok == "-MT" || tok == "-MQ") {
          if (c != std::string::npos) { size_t c2 = rest.find(',', c + 1); s.dep_target = rest.substr(c + 1, c2 == std::string::npos ? std::string::npos : c2 - c - 1); s.has_mt = true; c = c2; }
        }
        if (c == std::string::npos) break; pos = c + 1;
      }
      continue;
    }
    if (a == "-x") { if (i + 1 < argv.size()) xlang = argv[i + 1]; ++i; continue; }
    if (a.rfind("-fthinlto-index=", 0) == 0) { s.thinlto_index = a.substr(16); continue; }
    if (a == "-") { s.reason = "stdin input"; return s; }
    // Compiler-feature probes (kbuild cc-option / cc-disable-warning, autoconf): `-c -x c /dev/null -o tmp`.
    // They must run locally: a probe failing remotely for any incidental reason makes the build system drop a
    // flag silently (e.g. -Wno-address-of-packed-member in arch/x86/boot/compressed, after which the real compiles
    // fail with -Werror). distcc applies the same rule.
    if (a.rfind("/dev/", 0) == 0) { s.reason = "device path " + a + " (compiler probe)"; return s; }
    if (a[0] == '@') { s.reason = "response file"; return s; }
    if (a.rfind("-fplugin", 0) == 0 || a.rfind("-save-temps", 0) == 0 || a.rfind("-fprofile", 0) == 0 ||
        a.rfind("-fdump", 0) == 0 || a == "-ftime-report" || a.rfind("-fmodule", 0) == 0 || a.rfind("--coverage", 0) == 0 ||
        a == "-fsyntax-only" || a.rfind("-specs", 0) == 0 || a.rfind("-fdebug-prefix-map", 0) == 0 || a.rfind("-ffile-prefix-map", 0) == 0) {
      s.reason = "non-distributable flag " + a; return s;
    }
    if (takes_arg(a)) { ++i; continue; }
    if (a[0] == '-') continue;
    if (is_source(a) || !xlang.empty()) {
      if (nsrc++ == 0) { s.source = a; s.source_index = static_cast<int>(i); }
    } else {
      // an object/library input on a compile line -> link step
      s.reason = "non-source input " + a; return s;
    }
  }
  if (!has_c) { s.reason = "no -c"; return s; }
  if (nsrc != 1) { s.reason = nsrc == 0 ? "no source" : "multiple sources"; return s; }
  s.lang = xlang.empty() ? lang_of(s.source) : (xlang == "c" ? "c" : xlang == "c++" ? "c++" : xlang);
  // LLVM IR inputs (distributed ThinLTO backends) are distributable; header-push only (see Scan::thinlto_index)
  if (s.lang.rfind("assembler", 0) == 0) s.lang = "asm";   // -x assembler / -x assembler-with-cpp
  if (s.lang == "asm") {   // local only, but cacheable (direct path + client cache; see Scan::local_cacheable)
    if (s.output.empty()) s.output = default_object_name(s.source);
    s.local_cacheable = true; s.reason = "language asm not distributed"; return s;
  }
  if (s.lang != "c" && s.lang != "c++" && s.lang != "ir") { s.reason = "language " + s.lang + " not distributed"; return s; }
  if (s.output.empty()) s.output = default_object_name(s.source);
  s.distributable = true;
  return s;
}

std::string default_dep_name(const std::string& output) {
  std::string stem = output;
  auto dot = stem.rfind('.'), slash = stem.rfind('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) stem = stem.substr(0, dot);
  return stem + ".d";
}

IncludeDirs include_dirs(const std::vector<std::string>& argv) {
  IncludeDirs d;
  auto grab = [&](size_t& idx, const std::string& flag, std::vector<std::string>& into) -> bool {
    const std::string& a = argv[idx];
    if (a == flag) { if (idx + 1 < argv.size()) into.push_back(argv[idx + 1]); ++idx; return true; }
    if (a.rfind(flag, 0) == 0 && a.size() > flag.size()) { into.push_back(a.substr(flag.size())); return true; }
    return false;
  };
  for (size_t i = 1; i < argv.size(); ++i) {
    if (grab(i, "-iquote", d.quote) || grab(i, "-isystem", d.system) || grab(i, "-idirafter", d.after) ||
        grab(i, "-include", d.forced) || grab(i, "-imacros", d.forced)) continue;
    if (grab(i, "-I", d.bracket)) continue;
    if (takes_arg(argv[i])) ++i;
  }
  return d;
}

std::vector<std::string> mm_argv(const std::vector<std::string>& argv, const Scan& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (a == "-c" || a == "-MD" || a == "-MMD" || a == "-MP" || a == "-MG") continue;
    if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ") { ++i; continue; }
    if ((a.rfind("-MF", 0) == 0 || a.rfind("-MT", 0) == 0 || a.rfind("-MQ", 0) == 0) && a.size() > 3) continue;
    if (a.rfind("-Wp,", 0) == 0 && a.find("-M") != std::string::npos) continue;
    out.push_back(a);
  }
  (void)s;
  out.push_back("-MM");
  return out;
}

static bool is_abs(const std::string& p) { return !p.empty() && p[0] == '/'; }

std::vector<std::string> hp_remote_argv(const std::vector<std::string>& argv, const Scan& s, const std::string& root,
                                        const std::string& ofile, const std::string& depfile, const std::string& dep_target) {
  static const char* path_flags[] = {"-I", "-iquote", "-isystem", "-idirafter", "-include", "-imacros", "-iprefix", "-isysroot", "--sysroot"};
  auto rooted = [&](const std::string& p) { return is_abs(p) ? root + p : p; };
  std::vector<std::string> out;
  out.push_back(argv[0]);
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (static_cast<int>(i) == s.source_index) { out.push_back(rooted(a)); continue; }
    if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ") { ++i; continue; }
    if ((a.rfind("-MF", 0) == 0 || a.rfind("-MT", 0) == 0 || a.rfind("-MQ", 0) == 0) && a.size() > 3) continue;
    if (a.rfind("-Wp,", 0) == 0 && a.find("-M") != std::string::npos) continue;
    if (a == "-H") continue;
    if (a.rfind("-fthinlto-index=", 0) == 0) { out.push_back("-fthinlto-index=" + rooted(a.substr(16))); continue; }
    bool handled = false;
    for (const char* f : path_flags) {
      std::string flag(f);
      if (a == flag) { out.push_back(a); if (i + 1 < argv.size()) out.push_back(rooted(argv[i + 1])); ++i; handled = true; break; }
      if (a.rfind(flag, 0) == 0 && a.size() > flag.size() && (flag == "-I" || flag.size() > 2)) {
        std::string v = a.substr(flag.size());
        if (flag == "--sysroot" && !v.empty() && v[0] == '=') { out.push_back("--sysroot=" + rooted(v.substr(1))); }
        else out.push_back(flag + rooted(v));
        handled = true; break;
      }
    }
    if (handled) continue;
    out.push_back(a);
  }
  if (s.has_md) {
    // keep the client's choice of -MD (system headers listed) vs -MMD; the -Wp, form was dropped above
    bool md = has_flag(argv, "-MD"), plain = false;
    for (auto& a : argv) if (a == "-MD" || a == "-MMD") plain = true;
    if (!plain) out.push_back(md ? "-MD" : "-MMD");
    out.push_back("-MF"); out.push_back(depfile); out.push_back("-MT"); out.push_back(dep_target);
  }
  out.push_back("-ffile-prefix-map=" + root + "=");
  out.push_back("-o"); out.push_back(ofile);
  return out;
}

std::vector<std::string> preprocess_argv(const std::vector<std::string>& argv, const Scan& s, const std::string& ifile) {
  std::vector<std::string> out;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "-c") continue;
    if (argv[i] == "-o") { ++i; continue; }
    out.push_back(argv[i]);
  }
  if (s.has_md) {
    // gcc/clang derive the dep file (and clang the target) from -o; pin them to the real object.
    std::string stem = s.output;
    auto dot = stem.rfind('.'), slash = stem.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) stem = stem.substr(0, dot);
    if (!s.has_mf) { out.push_back("-MF"); out.push_back(stem + ".d"); }
    if (!s.has_mt) { out.push_back("-MT"); out.push_back(s.output); }
  }
  out.push_back("-E"); out.push_back("-o"); out.push_back(ifile);
  return out;
}

std::vector<std::string> remote_argv(const std::vector<std::string>& argv, const Scan& s,
                                     const std::string& ifile, const std::string& ofile) {
  std::vector<std::string> out;
  out.push_back(argv[0]);
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    if (static_cast<int>(i) == s.source_index) { out.push_back(ifile); continue; }
    if (a == "-o") { ++i; continue; }
    if (a == "-I" || a == "-D" || a == "-U" || a == "-include" || a == "-imacros" || a == "-isystem" || a == "-iquote" || a == "-idirafter" || a == "-iprefix" ||
        a == "-MF" || a == "-MT" || a == "-MQ" || a == "-isysroot") { ++i; continue; }
    if (a == "-MD" || a == "-MMD" || a == "-MP" || a == "-H" || a == "-nostdinc" || a == "-nostdinc++") continue;
    if (a.rfind("-I", 0) == 0 || a.rfind("-D", 0) == 0 || a.rfind("-U", 0) == 0 || a.rfind("-Wp,", 0) == 0 ||
        a.rfind("-MF", 0) == 0 || a.rfind("-MT", 0) == 0 || a.rfind("-MQ", 0) == 0 || a.rfind("-isystem", 0) == 0 ||
        a.rfind("-iquote", 0) == 0 || a.rfind("-idirafter", 0) == 0 || a.rfind("--sysroot", 0) == 0) continue;
    // preprocessor-only flags: on a .i they are "unused" and fatal under -Werror=unused-command-line-argument, which
    // kbuild sets (e.g. in drivers/firmware/efi/libstub, whose CFLAGS ignore KCFLAGS)
    if (a.rfind("-fmacro-prefix-map", 0) == 0 || a == "-undef" || a.rfind("-imultilib", 0) == 0) continue;
    if (a == "-x" && i + 1 < argv.size()) {
      const std::string& l = argv[i + 1];
      out.push_back("-x");
      out.push_back(l == "c" ? "cpp-output" : l == "c++" ? "c++-cpp-output" : l == "objective-c" ? "objective-c-cpp-output" : l);
      ++i; continue;
    }
    out.push_back(a);
  }
  out.push_back("-o"); out.push_back(ofile);
  return out;
}

}  // namespace opticc::args
