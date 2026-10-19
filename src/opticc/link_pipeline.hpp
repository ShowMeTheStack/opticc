// opticc: linker-domain pipeline on the coordinator.
//
// Given a final-link command, the coordinator (1) forms domains, (2) obtains each domain's condensed manifest
// from the worker that holds its objects, resolves the global symbol table from manifests alone (so
// undefined / multiply-defined symbols are known before any object bytes move) and computes the live boundary,
// (3) asks each worker for the prelinked domain object (proximity resolution), (4) rewrites the link command
// so the real linker consumes the domain objects.
//
// Ordering rule (byte-identity): the final link sees the domain objects in ascending domain order, where domains
// are numbered by the position of their first object in the original command; within a domain the original
// relative order is kept. A direct link of the ORIGINAL objects permuted into that same order is byte-identical to
// the domain link (tests/domains/test_domainlink.cpp); the original unpermuted link is a different (equally valid)
// layout. Object arguments are replaced in place: the domain objects are inserted where the first object stood,
// every other object argument is removed, all non-object arguments keep their positions.
//
// Domain formation: "placement" (default) = the node that compiled the object (no object moves;
// objects the coordinator did not compile form the client's own domain). "mincut" | "random" | "directory"
// partition the object reference graph (domains library) into K = #workers parts, assign part i to worker i, and
// ship objects that are not already on that worker (measured as extra bytes).
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "opticc/transport.hpp"

namespace opticc::coord {

struct LinkObject {
  std::string path;          // absolute path on the client
  std::string sha;           // sha256 of the bytes ("" = not yet hashed)
  uint64_t size = 0;
  int node = 0;              // node that holds it (0 = client only)
  double weight = 0;         // measured compile CPU (0 = unknown → bytes)
};

struct LinkOptions {
  std::string pipeline = "pipelined";    // sequential | pipelined
  std::string partition = "placement";   // placement | mincut | random | directory
  bool gc = true;                        // two-phase gc (forced off for -r links)
  bool apply_pcrel = true;               // proximity resolution (off = ld -r-like merge)
  std::vector<std::string> keep_globs;   // linker-script KEEP sections (OPTICC_DOMAIN_KEEP, comma-separated)
  int zstd_level = 3;
  std::string report_path;               // OPTICC_LINK_REPORT: write the JSON report here
  std::string tmp_dir;                   // where domain objects are written (default <cwd>/.opticc-link-<pid>)
  unsigned seed = 1;
  bool force = false;                    // OPTICC_LINK_FORCE=1: bypass the benefit gate (small tests)
  double min_mb = 0;                     // OPTICC_LINK_MIN_MB: minimum total input for domains (0 = default 128)
};

struct LinkOutcome {
  bool used_domains = false;
  std::vector<std::string> argv;         // rewritten (or original on fallback)
  nlohmann::json report;
  std::string error;                     // non-empty on fallback
};

using Lookup = std::function<LinkObject(const std::string& abs_path)>;
using Log = std::function<void(const nlohmann::json&)>;

// `conns[i]` is node i+1 (may be null when a node is down). `next_id` yields request ids unique on the connection.
LinkOutcome run_link_pipeline(const std::vector<std::string>& argv, const std::string& cwd, const LinkOptions& opt,
                              const std::vector<net::WorkerConn*>& conns, const Lookup& lookup, const Log& log,
                              const std::function<int64_t()>& next_id);

// Exposed for tests: expand thin archives / response files into object paths (absolute), returns false if an
// argument is an archive that cannot be expanded (regular archive) — such arguments are left in place.
bool expand_thin_archive(const std::string& path, std::vector<std::string>* members);
bool looks_like_link(const std::vector<std::string>& argv);

}  // namespace opticc::coord
