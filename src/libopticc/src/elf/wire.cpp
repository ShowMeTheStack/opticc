// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/elf/wire.hpp"

namespace opticc::elf {
using nlohmann::json;

json secref_json(const SecRef& r) { return json::array({r.obj, r.sec}); }
SecRef secref_from(const json& j) { SecRef r; r.obj = j.at(0).get<int>(); r.sec = j.at(1).get<int>(); return r; }
json secrefs_json(const std::set<SecRef>& s) { json a = json::array(); for (auto& r : s) a.push_back(secref_json(r)); return a; }
std::set<SecRef> secrefs_from(const json& j) { std::set<SecRef> s; for (auto& e : j) s.insert(secref_from(e)); return s; }

json domain_manifest_wire(const DomainManifest& m) {
  json j; j["domain"] = m.domain; j["objs"] = m.objs;
  j["roots"] = secrefs_json(m.roots); j["entries"] = secrefs_json(m.entries);
  json arcs = json::array(); for (auto& [e, xs] : m.arcs) arcs.push_back({secref_json(e), secrefs_json(xs)}); j["arcs"] = arcs;
  json imp = json::array(); for (auto& [x, syms] : m.imports) imp.push_back({secref_json(x), syms}); j["imports"] = imp;
  json ss = json::array(); for (auto& [x, names] : m.start_stop) ss.push_back({secref_json(x), names}); j["start_stop"] = ss;
  json ex = json::object(); for (auto& [s, r] : m.exports) ex[s] = secref_json(r); j["exports"] = ex;
  json ns = json::object(); for (auto& [n, v] : m.named_sections) { json a = json::array(); for (auto& r : v) a.push_back(secref_json(r)); ns[n] = a; } j["named_sections"] = ns;
  return j;
}

DomainManifest domain_manifest_from_wire(const json& j) {
  DomainManifest m; m.domain = j.value("domain", 0); m.objs = j.value("objs", std::vector<int>{});
  m.roots = secrefs_from(j.value("roots", json::array())); m.entries = secrefs_from(j.value("entries", json::array()));
  for (auto& e : j.value("arcs", json::array())) m.arcs[secref_from(e.at(0))] = secrefs_from(e.at(1));
  for (auto& e : j.value("imports", json::array())) { std::set<std::string> s; for (auto& x : e.at(1)) s.insert(x.get<std::string>()); m.imports[secref_from(e.at(0))] = s; }
  for (auto& e : j.value("start_stop", json::array())) { std::set<std::string> s; for (auto& x : e.at(1)) s.insert(x.get<std::string>()); m.start_stop[secref_from(e.at(0))] = s; }
  // bind object fields to references before .items(): the proxy returned by items() refers to the json it was called on,
  // and value() returns a temporary (iterating it directly yields empty maps)
  static const json empty_obj = json::object();
  const json& ex = j.contains("exports") ? j["exports"] : empty_obj;
  for (auto& [s, r] : ex.items()) m.exports[s] = secref_from(r);
  const json& ns = j.contains("named_sections") ? j["named_sections"] : empty_obj;
  for (auto& [n, a] : ns.items()) { std::vector<SecRef> v; for (auto& r : a) v.push_back(secref_from(r)); m.named_sections[n] = v; }
  return m;
}

json roots_json(const Roots& r) { return json{{"entry", r.entry_symbols}, {"keep", r.keep_globs}, {"reserved", r.reserved_names}, {"group_members", r.group_members_together}}; }
Roots roots_from(const json& j) {
  Roots r; r.entry_symbols = j.value("entry", std::vector<std::string>{}); r.keep_globs = j.value("keep", std::vector<std::string>{});
  r.reserved_names = j.value("reserved", true); r.group_members_together = j.value("group_members", false); return r;
}

}  // namespace opticc::elf
