// opticc: validation of cached worker probes (rtt / bandwidth) across coordinator starts.
// SPDX-License-Identifier: GPL-2.0-only
//
// Cached entries (~/.cache/opticc/probe-cache.json) go stale when the network changes: an 80 ms / 100 Mbit link
// scheduled with the 1 ms / 1.2 Gbit values of an earlier fast link picks raw uploads and sizes windows for the
// wrong RTT, and a host moving from LAN to VPN has the same problem.
// An entry is therefore used only if it is fresh (TTL), was measured over the same local route (the cache key
// includes the local address the connection uses), and the RTT measured right now (2 pings, cheap) agrees within a
// factor `tol`; else the worker is re-probed before the first dispatch. Bandwidth drift after start-up is tracked live per node from
// the kernel's TCP delivery rate (WorkerConn::tcp_delivery_rate) and replaces the probe value.
#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace opticc::probecache {

enum class Verdict { Use, Reprobe };

// entry: {"rtt_s", "up", "down", "t"}; rtt_now_s: min of the pings just measured. Sub-millisecond differences are
// treated as jitter (|Δrtt| < 0.5 ms never invalidates).
inline Verdict validate(const nlohmann::json& entry, double now, double rtt_now_s, double ttl_s = 600.0, double tol = 1.5) {
  if (!entry.is_object()) return Verdict::Reprobe;
  double t = entry.value("t", 0.0), rtt = entry.value("rtt_s", 0.0), up = entry.value("up", 0.0), down = entry.value("down", 0.0);
  if (!(up > 0) || !(down > 0) || !(rtt > 0)) return Verdict::Reprobe;
  if (now - t > ttl_s || now < t) return Verdict::Reprobe;
  if (!(rtt_now_s > 0)) return Verdict::Reprobe;
  double hi = std::max(rtt, rtt_now_s), lo = std::min(rtt, rtt_now_s);
  if (hi - lo >= 0.0005 && hi > tol * lo) return Verdict::Reprobe;
  return Verdict::Use;
}

// Cache key: worker identity + the local route (local address of the connection).
inline std::string key(const std::string& fingerprint, const std::string& host, const std::string& local_addr) {
  return fingerprint + "@" + host + "|" + local_addr;
}

}  // namespace opticc::probecache
