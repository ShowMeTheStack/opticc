// opticc: small POSIX helpers shared by client, coordinator and worker.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opticc::util {

double now_s();                                   // monotonic seconds
std::string read_file(const std::string& path);   // throws on error
void write_file(const std::string& path, const std::string& data);  // throws
bool file_exists(const std::string& path);
uint64_t file_size(const std::string& path);      // 0 if missing
std::string make_temp_dir(const std::string& prefix);  // mkdtemp under $TMPDIR
void remove_tree(const std::string& path);
std::string basename(const std::string& path);
std::string dirname(const std::string& path);
std::string abspath(const std::string& path);     // relative to cwd
std::string getenv_or(const char* name, const std::string& dflt);
int online_cores();
double total_memory_bytes();
std::string hostname();

// Deterministic CPU micro-benchmark (~50 ms on a 2020-era core): returns the
// wall time of a fixed amount of integer/FP work. Speed factor of a worker is
// bench(client) / bench(worker).
double cpu_bench_s();
// Realistic speed calibration: CPU seconds `cc -O2 -c` takes on a fixed generated C translation
// unit (no includes, 64 independent functions mixing loops, switches, structs and arithmetic), best of `reps`. Returns 0 when
// the compiler is missing or fails (callers then fall back to cpu_bench_s()). `cc` empty: $OPTICC_BENCH_CC, else the
// first of clang, cc, gcc found in PATH. The microbenchmark alone can misrate efficiency cores by 2x or more.
double compile_bench_s(const std::string& cc = "", int reps = 2);
std::string compile_bench_source();

// Run argv[0] with argv (PATH search), stdout/stderr to the given files
// (or inherited), optional cwd. Returns exit code (or 128+signal), child CPU
// seconds and wall seconds; on_spawn (if set) is called with the pid as soon
// as the child exists (used by CANCEL).
struct RunResult { int rc = -1; double cpu_s = 0; double wall_s = 0; long max_rss_bytes = 0; bool signalled = false; };
RunResult run_process(const std::vector<std::string>& argv, const std::string& stdout_path,
                      const std::string& stderr_path, const std::string& cwd,
                      const std::function<void(pid_t)>& on_spawn = nullptr,
                      bool inherit_stdout = false, bool inherit_stderr = false);

// SHA-256 of a byte string as 64 lowercase hex chars (content addressing, header-push store).
std::string sha256_hex(const std::string& data);
// XXH64 (seed 0) as 16 hex digits — a fast integrity check of cached objects on every hit (media
// corruption, a torn write of the right size); not a security hash (the key hashes are SHA-256)
std::string xxh64_hex(const std::string& data);
std::string xxh64_hex(const char* bytes, size_t len);
// Copy `src` to `dst` atomically (temporary file in dst's directory + rename) WITHOUT moving the bytes
// through user space (copy_file_range on Linux, a clone on APFS), after verifying the source: its size must be `size`
// (if >= 0) and, if `xxh` is not empty, the XXH64 of its bytes (hashed from a read-only mapping of the same open file
// that is then copied). false: nothing written; *why says which check failed.
bool copy_verified_atomic(const std::string& src, const std::string& dst, int64_t size, const std::string& xxh, std::string* why = nullptr);
// mkdir -p; throws on failure.
void mkdir_p(const std::string& dir);
// Join a root directory and an (untrusted) absolute path, after lexical normalisation with POSIX root clamping, so the
// result is always strictly inside root. Throws on a NUL or on a path naming the root itself.
std::string under_root(const std::string& root, const std::string& abs_path);
// The base name of an untrusted file name without its extension, safe as a file name in a job directory ("input" when
// nothing usable remains).
std::string safe_file_stem(const std::string& name);
// "/a/./b/../c" -> "/a/c"; ".." at the top stays at "/". Throws on NUL.
std::string lexically_normal_abs(const std::string& path);
// True when `path` is lexically inside `dir` (or equal to it).
bool path_within(const std::string& path, const std::string& dir);
// Replace every occurrence of `from` in `s` with `to`.
std::string replace_all(std::string s, const std::string& from, const std::string& to);

// Parse "a.b.c.d/len" into (ip, mask); returns false on error.
bool parse_cidr(const std::string& cidr, uint32_t* ip, uint32_t* mask);
bool cidr_match(uint32_t addr, const std::vector<std::pair<uint32_t, uint32_t>>& allow);
// Resolve a host name to its first IPv4 address (host byte order); 0 on failure.
uint32_t resolve_ipv4(const std::string& host);
// Loopback, RFC1918, link-local or CGNAT (100.64/10) address.
bool is_private_ipv4(uint32_t addr);
// Address lies in the subnet of a local interface named wg*, tun* or utun* (VPN).
bool on_vpn_interface(uint32_t addr);
// Private or VPN: where plaintext is permitted with explicit opt-in.
inline bool plaintext_ok_address(uint32_t addr) { return is_private_ipv4(addr) || on_vpn_interface(addr); }

}  // namespace opticc::util
