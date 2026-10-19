// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/closure.hpp"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>

#include "opticc/util.hpp"

namespace opticc::closure {
namespace fs = std::filesystem;

std::string absolutize(const std::string& path, const std::string& cwd) {
  if (path.empty()) return path;
  if (path[0] == '/') return fs::path(path).lexically_normal().string();
  return (fs::path(cwd) / path).lexically_normal().string();
}

std::vector<std::string> parse_depfile(const std::string& text) {
  // join continuation lines, then tokenise on unescaped whitespace; the token
  // ending in ':' (first one per rule) is a target.
  std::string t;
  t.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n') { t.push_back(' '); ++i; continue; }
    if (text[i] == '\\' && i + 2 < text.size() && text[i + 1] == '\r' && text[i + 2] == '\n') { t.push_back(' '); i += 2; continue; }
    t.push_back(text[i]);
  }
  std::vector<std::string> out; std::set<std::string> seen;
  std::istringstream lines(t); std::string line;
  while (std::getline(lines, line)) {
    std::vector<std::string> toks; std::string cur; bool target_seen = false;
    for (size_t i = 0; i < line.size(); ++i) {
      char c = line[i];
      if (c == '\\' && i + 1 < line.size() && (line[i + 1] == ' ' || line[i + 1] == '#' || line[i + 1] == '\\')) { cur.push_back(line[i + 1]); ++i; continue; }
      if (c == '$' && i + 1 < line.size() && line[i + 1] == '$') { cur.push_back('$'); ++i; continue; }
      if (c == ' ' || c == '\t') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } continue; }
      cur.push_back(c);
    }
    if (!cur.empty()) toks.push_back(cur);
    for (auto& tok : toks) {
      if (!target_seen) {
        if (tok.back() == ':') { target_seen = true; continue; }
        // "target :" with a space, or a stray token before the colon
        continue;
      }
      if (tok == ":") continue;
      if (tok.back() == ':') { tok.pop_back(); if (tok.empty()) continue; }   // "a.o: b.h c.h:" phony rules (-MP)
      if (seen.insert(tok).second) out.push_back(tok);
    }
  }
  return out;
}

std::vector<std::string> parse_kbuild_cmd(const std::string& text) {
  std::vector<std::string> out; std::set<std::string> seen;
  auto add = [&](std::string tok) {
    while (!tok.empty() && (tok.back() == '\\' || tok.back() == ' ' || tok.back() == '\t' || tok.back() == '\r')) tok.pop_back();
    size_t b = tok.find_first_not_of(" \t"); if (b == std::string::npos) return; tok = tok.substr(b);
    if (tok.empty() || tok[0] == '$') return;              // $(wildcard include/config/...) and other make expressions
    if (seen.insert(tok).second) out.push_back(tok);
  };
  std::istringstream is(text); std::string line; bool in_deps = false; std::string source;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("source_", 0) == 0) {
      auto p = line.find(":="); if (p != std::string::npos) { source = line.substr(p + 2); size_t b = source.find_first_not_of(" \t"); source = b == std::string::npos ? "" : source.substr(b); }
      continue;
    }
    if (line.rfind("deps_", 0) == 0) { in_deps = true; auto p = line.find(":="); if (p != std::string::npos) { std::string rest = line.substr(p + 2); add(rest); } continue; }
    if (!in_deps) continue;
    if (line.empty() || line.find(":=") != std::string::npos || line.find(": $(") != std::string::npos || line.rfind("$(deps_", 0) == 0) { in_deps = false; continue; }
    // one dependency per continuation line (may also hold several tokens); drop make expressions
    // such as "$(wildcard include/config/FOO)" as a unit before tokenising
    std::string clean;
    for (size_t i = 0; i < line.size();) {
      if (line[i] == '$' && i + 1 < line.size() && line[i + 1] == '(') {
        int depth = 0; size_t j = i;
        for (; j < line.size(); ++j) { if (line[j] == '(') ++depth; else if (line[j] == ')' && --depth == 0) { ++j; break; } }
        i = j; continue;
      }
      clean.push_back(line[i++]);
    }
    std::istringstream ls(clean); std::string tok;
    while (ls >> tok) add(tok);
  }
  if (!source.empty()) { if (!seen.count(source)) out.insert(out.begin(), source); }
  return out;
}

std::string kbuild_cmd_path(const std::string& out) {
  fs::path p(out); if (p.filename().empty()) return "";
  return (p.parent_path() / ("." + p.filename().string() + ".cmd")).string();
}

std::vector<std::string> system_include_dirs(const std::string& compiler, const std::string& lang, const std::string& cwd) {
  std::vector<std::string> dirs;
  try {
    std::string dir = util::make_temp_dir("opticc-sysinc-");
    std::vector<std::string> argv{compiler, "-E", "-v", "-x", lang == "c++" ? "c++" : "c", "/dev/null"};
    util::run_process(argv, dir + "/out", dir + "/err", cwd);
    std::string err = util::file_exists(dir + "/err") ? util::read_file(dir + "/err") : "";
    util::remove_tree(dir);
    std::istringstream is(err); std::string line; bool in = false;
    while (std::getline(is, line)) {
      if (line.find("search starts here:") != std::string::npos) { in = true; continue; }
      if (line.find("End of search list.") != std::string::npos) break;
      if (!in) continue;
      size_t b = line.find_first_not_of(' ');
      if (b == std::string::npos) continue;
      std::string d = line.substr(b);
      auto fw = d.find(" (framework directory)"); if (fw != std::string::npos) d = d.substr(0, fw);
      std::error_code ec; auto canon = fs::weakly_canonical(absolutize(d, cwd), ec);
      dirs.push_back(ec ? absolutize(d, cwd) : canon.string());
    }
  } catch (const std::exception&) {}
  return dirs;
}

std::string resolve_include(const std::string& name, const std::string& includer, const args::IncludeDirs& dirs,
                            const std::vector<std::string>& system_dirs, const std::string& cwd) {
  std::vector<std::string> search;
  if (!name.empty() && name[0] == '/') return util::file_exists(name) ? name : "";
  if (!includer.empty()) search.push_back(util::dirname(includer));
  for (auto& d : dirs.quote) search.push_back(absolutize(d, cwd));
  for (auto& d : dirs.bracket) search.push_back(absolutize(d, cwd));
  for (auto& d : dirs.system) search.push_back(absolutize(d, cwd));
  for (auto& d : system_dirs) search.push_back(d);
  for (auto& d : dirs.after) search.push_back(absolutize(d, cwd));
  for (auto& d : search) {
    std::string cand = (fs::path(d) / name).lexically_normal().string();
    std::error_code ec;
    if (fs::is_regular_file(cand, ec)) return cand;
  }
  return "";
}

bool parse_missing_include(const std::string& text_in, std::string* name, std::string* includer) {
  static const std::regex clang_re(R"(^(.*?):\d+:\d+: fatal error: '([^']+)' file not found)");
  static const std::regex gcc_re(R"(^(.*?):\d+:\d+: fatal error: ([^:]+): No such file or directory)");
  // Builds that pass -fdiagnostics-color / -fcolor-diagnostics (CMake >= 3.24 with Ninja does by default) put ANSI
  // escapes inside the line; strip CSI sequences before matching, or the MISSING round never fires.
  std::string text; text.reserve(text_in.size());
  for (size_t i = 0; i < text_in.size(); ++i) {
    if (text_in[i] == '\x1b' && i + 1 < text_in.size() && text_in[i + 1] == '[') {
      size_t k = i + 2; while (k < text_in.size() && ((text_in[k] >= '0' && text_in[k] <= '9') || text_in[k] == ';')) ++k;
      if (k < text_in.size()) { i = k; continue; }   // skip ESC [ params final-byte
    }
    text.push_back(text_in[i]);
  }
  std::istringstream is(text); std::string line; std::smatch m;
  while (std::getline(is, line)) {
    if (std::regex_search(line, m, clang_re) || std::regex_search(line, m, gcc_re)) {
      *includer = m[1].str(); *name = m[2].str();
      return true;
    }
  }
  return false;
}

}  // namespace opticc::closure
