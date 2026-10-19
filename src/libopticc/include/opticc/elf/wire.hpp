// SPDX-License-Identifier: GPL-2.0-only
// Wire form of the condensed domain manifest and section-reference sets, as exchanged between opticcd and the
// coordinator over PRELINK/PRELINK_RESULT. SecRefs travel as [obj, sec] integer pairs local to the domain's object
// list (which both ends hold in the same order: the link order).
#pragma once
#include <set>

#include "nlohmann/json.hpp"
#include "opticc/elf/liveness.hpp"

namespace opticc::elf {

nlohmann::json secref_json(const SecRef& r);
SecRef secref_from(const nlohmann::json& j);
nlohmann::json secrefs_json(const std::set<SecRef>& s);
std::set<SecRef> secrefs_from(const nlohmann::json& j);
nlohmann::json domain_manifest_wire(const DomainManifest& m);
DomainManifest domain_manifest_from_wire(const nlohmann::json& j);
// Roots <-> json {"entry":[...], "keep":[...], "reserved":bool, "group_members":bool}
nlohmann::json roots_json(const Roots& r);
Roots roots_from(const nlohmann::json& j);

}  // namespace opticc::elf
