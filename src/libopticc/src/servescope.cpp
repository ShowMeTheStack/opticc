// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/servescope.hpp"

#include <filesystem>
#include <system_error>

#include "opticc/argscan.hpp"
#include "opticc/closure.hpp"
#include "opticc/util.hpp"

namespace fs = std::filesystem;

namespace opticc::servescope {

std::string canonical_or_lexical(const std::string& p) {
  std::error_code ec;
  fs::path c = fs::canonical(p, ec);
  if (!ec) return c.string();
  return util::lexically_normal_abs(p.empty() || p[0] != '/' ? "/" + p : p);
}

static std::vector<std::string> components(const std::string& p) {
  std::vector<std::string> v; size_t i = 0;
  while (i <= p.size()) {
    size_t j = p.find('/', i); if (j == std::string::npos) j = p.size();
    if (j > i) v.push_back(p.substr(i, j - i));
    i = j + 1;
  }
  return v;
}
// deepest common ancestor of two absolute normal paths, by whole components ("/a/bc" and "/a/b" -> "/a")
std::string common_ancestor(const std::string& a, const std::string& b) {
  auto x = components(a), y = components(b);
  std::string r;
  for (size_t k = 0; k < x.size() && k < y.size() && x[k] == y[k]; ++k) r += "/" + x[k];
  return r.empty() ? std::string("/") : r;
}

Scope build(const std::vector<std::string>& argv, const std::string& cwd, const std::string& src,
            const std::string& includer_ok, const std::vector<std::string>& extra_deny) {
  Scope s;
  auto add = [&](const std::string& d) { if (d.empty()) return; std::string c = canonical_or_lexical(d); if (c != "/") s.allow.push_back(c); };
  const std::string ccwd = canonical_or_lexical(cwd);
  add(ccwd);
  const std::string src_abs = closure::absolutize(src, cwd);
  const std::string src_dir = canonical_or_lexical(util::dirname(src_abs));
  add(src_dir);
  args::IncludeDirs dirs = args::include_dirs(argv);
  std::vector<std::string> incs;
  for (auto* v : {&dirs.quote, &dirs.bracket, &dirs.system, &dirs.after})
    for (auto& d : *v) incs.push_back(canonical_or_lexical(closure::absolutize(d, cwd)));
  for (auto& f : dirs.forced) incs.push_back(canonical_or_lexical(util::dirname(closure::absolutize(f, cwd))));
  if (!includer_ok.empty()) incs.push_back(canonical_or_lexical(util::dirname(includer_ok)));
  for (auto& d : incs) add(d);
  // project roots: deepest common ancestor of the source directory and each other scope directory (never "/")
  std::vector<std::string> others = incs; others.push_back(ccwd);
  for (auto& d : others) { std::string r = common_ancestor(src_dir, d); if (r != "/") s.allow.push_back(r); }
  const std::string home = util::getenv_or("HOME", "");
  const std::string keydir = util::getenv_or("OPTICC_KEY_DIR", "");
  for (const std::string& d : {keydir, home.empty() ? std::string() : home + "/.config/opticc", home.empty() ? std::string() : home + "/.ssh",
                               home.empty() ? std::string() : home + "/.gnupg", std::string("/etc/ssh"), std::string("/proc"),
                               std::string("/sys"), std::string("/dev")})
    if (!d.empty()) s.deny.push_back(canonical_or_lexical(d));
  for (auto& d : extra_deny) if (!d.empty()) s.deny.push_back(canonical_or_lexical(d));
  return s;
}

bool allowed(const Scope& s, const std::string& path, std::string* why) {
  auto no = [&](const std::string& w) { if (why) *why = w; return false; };
  if (path.empty()) return no("empty path");
  std::error_code ec;
  fs::path c = fs::canonical(path, ec);
  if (ec) return no("does not exist");
  const std::string cp = c.string();
  if (!fs::is_regular_file(c, ec)) return no("not a regular file");
  if (cp == "/etc/shadow" || cp == "/etc/gshadow") return no("denied file");
  for (auto& d : s.deny) if (util::path_within(cp, d)) return no("inside a denied directory: " + d);
  for (auto& d : s.allow) if (util::path_within(cp, d)) return true;
  return no("outside the job's scope");
}

}  // namespace opticc::servescope
