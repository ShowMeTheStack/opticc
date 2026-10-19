// opticc coordinator: one per build (per user). Owns the persistent worker
// connections and drives libopticc::Scheduler with wall-clock time exactly as
// the simulator drives it with simulated time.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <string>

namespace opticc::coord {

struct Options {
  std::string sock_path;        // unix socket for clients
  std::string hosts;            // "host:port[/cores][@speed] ..." (empty = everything local)
  std::string policy = "ect-window";
  std::string token;
  std::string summary_path;     // written at exit / SIGUSR1 (empty = none)
  std::string predictor = "affine";
  double idle_timeout_s = 120;  // exit after this long with nothing in flight
  int zstd_level = 3;
  bool zstd_explicit = false;   // OPTICC_ZSTD set -> fixed level; else per-node adaptive (classic uploads)
  int local_slots = -1;         // -1 = client cores
  bool verbose = false;
  // connection security
  std::string key_dir;          // client.key/client.pub/known_workers (default ~/.config/opticc)
  std::string security = "auto";// auto | noise | plain  (OPTICC_SECURITY)
  bool tofu = false;            // record unknown worker keys on first contact (OPTICC_TOFU=1)
  std::string psk_file;         // plain-mode pre-shared key (OPTICC_PSK_FILE)
  // verify mode: compile this fraction of remote jobs twice and compare objects (OPTICC_VERIFY)
  double verify_frac = 0;
  uint64_t verify_seed = 1;
  // binding: "late" (default) chooses the node after the client-side work; "early" at dispatch (OPTICC_BINDING)
  std::string binding = "late";
  // multiply predictions by a seeded log-uniform factor in [1/gamma, gamma] (OPTICC_PRED_GAMMA, OPTICC_SEED)
  double pred_gamma = 0;
  uint64_t seed = 1;
  // OPTICC_WINDOW = "slots" (plain core count) or a scale factor applied to N_k*
  std::string window;
  // linker domains: OPTICC_LINK_DOMAINS=1 enables the pipeline for link commands sent via `opticc --link`
  bool link_domains = false;
  std::string link_pipeline = "pipelined";    // OPTICC_LINK_PIPELINE sequential|pipelined
  std::string link_partition = "placement";   // OPTICC_DOMAIN_PARTITION placement|mincut|random|directory
  bool link_gc = true;                        // OPTICC_DOMAIN_GC
  bool link_pcrel = true;                     // OPTICC_DOMAIN_PCREL (proximity resolution)
  std::string link_keep;                      // OPTICC_DOMAIN_KEEP comma-separated KEEP globs
  std::string link_report;                    // OPTICC_LINK_REPORT: per-link JSON report path
  // header prefetch
  bool prefetch = true;                       // OPTICC_PREFETCH=0 disables piggyback + background warm-up
  double prefetch_frac = 0.3;                 // OPTICC_PREFETCH_FRAC: share of a node's measured uplink for background pushes
  double prefetch_budget_mb = 64;             // OPTICC_PREFETCH_BUDGET_MB: background bytes per node per coordinator session
  std::string prefetch_warm;                  // OPTICC_PREFETCH_WARM: popularity ranking file (default ~/.cache/opticc/popular.txt)
  int prefetch_min_remote = 8;                // OPTICC_PREFETCH_MIN_REMOTE: bulk background pushes start after this many remote dispatches
  double prefetch_warm_mb = 8;                // OPTICC_PREFETCH_WARM_MB: ungated warm-ranking pushes per node before that
};

int run(const Options& o);

}  // namespace opticc::coord
