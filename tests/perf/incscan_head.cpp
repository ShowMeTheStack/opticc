// SPDX-License-Identifier: GPL-2.0-only
#include "incscan_head.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <filesystem>
#include <set>

#include "opticc/closure.hpp"
#include "opticc/util.hpp"

namespace opticc::incscan_head {
namespace fs = std::filesystem;

FileScan parse_text(const std::string& text) {
  FileScan fsn;
  size_t i = 0, n = text.size();
  bool in_block = false;
  while (i < n) {
    // one logical line (backslash continuations joined)
    std::string line;
    while (i < n) {
      char c = text[i];
      if (c == '\\' && i + 1 < n && text[i + 1] == '\n') { i += 2; continue; }
      if (c == '\\' && i + 2 < n && text[i + 1] == '\r' && text[i + 2] == '\n') { i += 3; continue; }
      if (c == '\n') { ++i; break; }
      line.push_back(c); ++i;
    }
    // strip block comments (state carried across lines) and line comments
    std::string clean; clean.reserve(line.size());
    for (size_t k = 0; k < line.size(); ++k) {
      if (in_block) { if (line[k] == '*' && k + 1 < line.size() && line[k + 1] == '/') { in_block = false; ++k; } continue; }
      if (line[k] == '/' && k + 1 < line.size() && line[k + 1] == '*') { in_block = true; ++k; continue; }
      if (line[k] == '/' && k + 1 < line.size() && line[k + 1] == '/') break;
      clean.push_back(line[k]);
    }
    size_t p = clean.find_first_not_of(" \t\r");
    if (p == std::string::npos || clean[p] != '#') continue;
    p = clean.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) continue;
    bool next = false;
    if (clean.compare(p, 12, "include_next") == 0) { next = true; p += 12; }
    else if (clean.compare(p, 7, "include") == 0 && !(p + 7 < clean.size() && (std::isalnum(static_cast<unsigned char>(clean[p + 7])) || clean[p + 7] == '_'))) p += 7;
    else if (clean.compare(p, 6, "import") == 0) p += 6;
    else continue;
    p = clean.find_first_not_of(" \t", p);
    if (p == std::string::npos) { fsn.computed = true; continue; }
    char open = clean[p]; char close = open == '"' ? '"' : open == '<' ? '>' : 0;
    if (!close) { fsn.computed = true; continue; }          // #include MACRO
    size_t e = clean.find(close, p + 1);
    if (e == std::string::npos) { fsn.computed = true; continue; }
    Directive d; d.angled = open == '<'; d.next = next; d.name = clean.substr(p + 1, e - p - 1);
    if (!d.name.empty()) fsn.incs.push_back(d);
  }
  return fsn;
}

const FileScan& Scanner::scan_file(const std::string& abs) {
  struct stat st{};
  int64_t mt = 0; uint64_t sz = 0;
  if (::stat(abs.c_str(), &st) == 0) {
#ifdef __APPLE__
    mt = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    mt = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    sz = static_cast<uint64_t>(st.st_size);
  }
  {
    std::lock_guard<std::mutex> g(mu_);
    auto it = cache_.find(abs);
    if (it != cache_.end() && it->second.mtime_ns == mt && it->second.size == sz) return it->second;
  }
  FileScan fsn;
  try { fsn = parse_text(util::read_file(abs)); } catch (...) {}
  fsn.mtime_ns = mt; fsn.size = sz;
  std::lock_guard<std::mutex> g(mu_);
  return cache_[abs] = fsn;
}

std::vector<std::string> Scanner::closure(const std::string& src_abs, const args::IncludeDirs& dirs,
                                          const std::vector<std::string>& system_dirs, const std::string& cwd,
                                          bool* computed, int* files_scanned) {
  std::vector<std::string> bracket_search, quote_extra;
  for (auto& d : dirs.quote) quote_extra.push_back(closure::absolutize(d, cwd));
  for (auto& d : dirs.bracket) bracket_search.push_back(closure::absolutize(d, cwd));
  for (auto& d : dirs.system) bracket_search.push_back(closure::absolutize(d, cwd));
  for (auto& d : system_dirs) bracket_search.push_back(d);
  for (auto& d : dirs.after) bracket_search.push_back(closure::absolutize(d, cwd));
  auto in_system = [&](const std::string& abs) { for (auto& d : system_dirs) if (util::path_within(abs, d)) return true; return false; };
  auto resolve = [&](const Directive& d, const std::string& includer) -> std::string {
    if (!d.name.empty() && d.name[0] == '/') return util::file_exists(d.name) ? d.name : "";
    std::vector<std::string> search;
    if (!d.angled && !d.next) { search.push_back(util::dirname(includer)); search.insert(search.end(), quote_extra.begin(), quote_extra.end()); }
    search.insert(search.end(), bracket_search.begin(), bracket_search.end());
    for (auto& dir : search) {
      std::string cand = (fs::path(dir) / d.name).lexically_normal().string();
      std::error_code ec;
      if (fs::is_regular_file(cand, ec)) return cand;
    }
    return "";
  };
  std::vector<std::string> out; std::set<std::string> seen{src_abs};
  std::vector<std::string> stack{src_abs};
  for (auto& f : dirs.forced) {
    Directive d; d.name = f; std::string r = resolve(d, cwd + "/.");
    if (!r.empty() && seen.insert(r).second) { if (!in_system(r)) { out.push_back(r); stack.push_back(r); } }
  }
  bool comp = false; int scanned = 0;
  while (!stack.empty()) {
    std::string cur = stack.back(); stack.pop_back();
    const FileScan& fsn = scan_file(cur); ++scanned;
    if (fsn.computed) comp = true;
    for (auto& d : fsn.incs) {
      std::string r = resolve(d, cur);
      if (r.empty() || !seen.insert(r).second) continue;
      if (in_system(r)) continue;            // not shipped, not entered
      out.push_back(r); stack.push_back(r);
    }
  }
  if (computed) *computed = comp;
  if (files_scanned) *files_scanned = scanned;
  return out;
}

}  // namespace opticc::incscan_head
