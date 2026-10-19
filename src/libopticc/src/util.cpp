// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/util.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <copyfile.h>
#include <sys/clonefile.h>
#include <sys/sysctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

extern char** environ;

namespace opticc::util {
namespace fs = std::filesystem;

double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string read_file(const std::string& path) {
  // fstat + read() into a pre-sized string (an ifstream/ostringstream would copy every byte twice more;
  // this runs for every manifest, object and source on the wrapper's hot path)
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot read " + path);
  struct stat st{};
  std::string out;
  if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) out.resize(static_cast<size_t>(st.st_size));
  size_t have = 0;
  for (;;) {
    if (have == out.size()) out.resize(out.size() < 4096 ? 4096 : out.size() * 2);   // grew, or unknown size
    ssize_t n = ::read(fd, &out[have], out.size() - have);
    if (n < 0) { if (errno == EINTR) continue; ::close(fd); throw std::runtime_error("cannot read " + path); }
    if (n == 0) break;
    have += static_cast<size_t>(n);
  }
  ::close(fd);
  out.resize(have);
  return out;
}
void write_file(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw std::runtime_error("cannot write " + path);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!f) throw std::runtime_error("short write " + path);
  f.close();   // a failed flush on close (ENOSPC, EIO) is an error, not a silently short file
  if (!f) throw std::runtime_error("close failed " + path);
}
bool file_exists(const std::string& path) { std::error_code ec; return fs::exists(path, ec); }
uint64_t file_size(const std::string& path) { std::error_code ec; auto s = fs::file_size(path, ec); return ec ? 0 : s; }

std::string make_temp_dir(const std::string& prefix) {
  std::string base = getenv_or("TMPDIR", "/tmp");
  if (!base.empty() && base.back() == '/') base.pop_back();
  std::string tmpl = base + "/" + prefix + "XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end()); buf.push_back(0);
  if (!mkdtemp(buf.data())) throw std::runtime_error("mkdtemp failed: " + tmpl);
  return std::string(buf.data());
}
void remove_tree(const std::string& path) { std::error_code ec; fs::remove_all(path, ec); }
std::string basename(const std::string& path) { return fs::path(path).filename().string(); }
std::string dirname(const std::string& path) { auto p = fs::path(path).parent_path().string(); return p.empty() ? "." : p; }
std::string abspath(const std::string& path) { std::error_code ec; return fs::absolute(path, ec).lexically_normal().string(); }
std::string getenv_or(const char* name, const std::string& dflt) { const char* v = std::getenv(name); return v && *v ? v : dflt; }
int online_cores() { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? static_cast<int>(n) : 1; }

double total_memory_bytes() {
#ifdef __APPLE__
  int64_t mem = 0; size_t len = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) return static_cast<double>(mem);
  return 0;
#else
  long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGE_SIZE);
  return pages > 0 && psz > 0 ? static_cast<double>(pages) * static_cast<double>(psz) : 0;
#endif
}
std::string hostname() { char b[256] = {0}; gethostname(b, sizeof b - 1); return b; }

double cpu_bench_s() {
  // Fixed work: 2e6 iterations of a mixed integer/FP recurrence (~40-60 ms).
  double t0 = now_s();
  volatile uint64_t x = 88172645463325252ULL; double acc = 0;
  for (int i = 0; i < 2000000; ++i) {
    uint64_t v = x; v ^= v << 13; v ^= v >> 7; v ^= v << 17; x = v;
    acc += std::sqrt(static_cast<double>(v & 0xffff) + 1.0);
  }
  if (acc < 0) std::abort();  // keep the loop alive
  return now_s() - t0;
}

std::string compile_bench_source() {
  std::string s = "/* opticc compile benchmark: fixed, deterministic, no includes */\n"
                  "typedef unsigned long u64; typedef struct { u64 a, b; int c[8]; } rec;\n"
                  "static u64 mix(u64 x) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; }\n";
  for (int i = 0; i < 64; ++i) {
    std::string n = std::to_string(i);
    s += "u64 f" + n + "(rec* r, int n) { u64 acc = " + n + "u; int i;\n"
         "  for (i = 0; i < n; ++i) { switch ((i + " + n + ") & 7) {\n"
         "    case 0: acc += mix(r->a + i); break; case 1: acc ^= r->b * " + std::to_string(i * 7 + 3) + "u; break;\n"
         "    case 2: r->c[i & 7] += (int)(acc >> 3); break; case 3: acc = acc * 31u + r->c[(i + 1) & 7]; break;\n"
         "    case 4: if (acc & 1) r->a ^= acc; else r->b += acc; break; case 5: acc -= (u64)r->c[i & 7] * 17u; break;\n"
         "    default: acc = mix(acc + (u64)i * " + n + "u); } }\n"
         "  for (i = 0; i < 8; ++i) acc += (u64)r->c[i] << (i & 3);\n"
         "  return acc ^ (u64)n; }\n";
  }
  return s;
}

double compile_bench_s(const std::string& cc_in, int reps) {
  std::string cc = cc_in;
  if (cc.empty()) cc = getenv_or("OPTICC_BENCH_CC", "");
  if (cc.empty()) {
    const char* path = std::getenv("PATH"); std::string p = path ? path : "/usr/bin:/bin";
    for (const char* cand : {"clang", "cc", "gcc"}) {
      size_t pos = 0;
      while (pos <= p.size() && cc.empty()) {
        size_t c = p.find(':', pos); std::string dir = p.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        std::string full = (dir.empty() ? std::string(".") : dir) + "/" + cand;
        if (::access(full.c_str(), X_OK) == 0) cc = full;
        if (c == std::string::npos) break; pos = c + 1;
      }
      if (!cc.empty()) break;
    }
  }
  if (cc.empty()) return 0.0;
  double best = 0.0;
  std::string dir;
  try {
    dir = make_temp_dir("opticc-bench-");
    write_file(dir + "/bench.c", compile_bench_source());
    for (int r = 0; r < std::max(1, reps); ++r) {
      RunResult rr = run_process({cc, "-O2", "-c", dir + "/bench.c", "-o", dir + "/bench.o"}, dir + "/out", dir + "/err", dir);
      if (rr.rc != 0 || rr.cpu_s <= 0) { best = 0.0; break; }
      best = best > 0 ? std::min(best, rr.cpu_s) : rr.cpu_s;
    }
  } catch (...) { best = 0.0; }
  if (!dir.empty()) { try { remove_tree(dir); } catch (...) {} }
  return best;
}

RunResult run_process(const std::vector<std::string>& argv, const std::string& stdout_path,
                      const std::string& stderr_path, const std::string& cwd,
                      const std::function<void(pid_t)>& on_spawn,
                      bool inherit_stdout, bool inherit_stderr) {
  RunResult r;
  std::vector<char*> av; av.reserve(argv.size() + 1);
  for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  av.push_back(nullptr);
  posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
  if (!inherit_stdout) posix_spawn_file_actions_addopen(&fa, 1, stdout_path.empty() ? "/dev/null" : stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (!inherit_stderr) posix_spawn_file_actions_addopen(&fa, 2, stderr_path.empty() ? "/dev/null" : stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
#pragma clang diagnostic pop
#elif defined(__GLIBC__)
  if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
#endif
  double t0 = now_s();
  pid_t pid = 0;
  int err = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (err != 0) { r.rc = 127; r.wall_s = 0; return r; }
  if (on_spawn) on_spawn(pid);
  // Per-child accounting (wait4). A getrusage(RUSAGE_CHILDREN) delta would also count every OTHER child that
  // exited during this job; in a multi-threaded worker that inflates a compile's CPU time many times over and
  // poisons the scheduler's work predictor.
  int status = 0;
  struct rusage after{};
  while (wait4(pid, &status, 0, &after) < 0) { if (errno != EINTR) { r.rc = 127; return r; } }
  r.wall_s = now_s() - t0;
  auto tv = [](const timeval& v) { return static_cast<double>(v.tv_sec) + static_cast<double>(v.tv_usec) * 1e-6; };
  r.cpu_s = tv(after.ru_utime) + tv(after.ru_stime);
#ifdef __APPLE__
  r.max_rss_bytes = after.ru_maxrss;
#else
  r.max_rss_bytes = after.ru_maxrss * 1024;
#endif
  if (WIFEXITED(status)) r.rc = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) { r.rc = 128 + WTERMSIG(status); r.signalled = true; }
  else r.rc = 1;
  return r;
}

bool parse_cidr(const std::string& cidr, uint32_t* ip, uint32_t* mask) {
  std::string a = cidr; int bits = 32;
  auto slash = a.find('/');
  if (slash != std::string::npos) { bits = std::atoi(a.substr(slash + 1).c_str()); a = a.substr(0, slash); }
  if (bits < 0 || bits > 32) return false;
  in_addr in{};
  if (inet_pton(AF_INET, a.c_str(), &in) != 1) return false;
  *ip = ntohl(in.s_addr);
  *mask = bits == 0 ? 0u : (0xffffffffu << (32 - bits));
  *ip &= *mask;
  return true;
}
bool cidr_match(uint32_t addr, const std::vector<std::pair<uint32_t, uint32_t>>& allow) {
  for (auto& [ip, mask] : allow) if ((addr & mask) == ip) return true;
  return false;
}

// ---- SHA-256 (FIPS 180-4), compact and dependency-free ----------------------------
namespace {
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
const uint32_t K256[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
void sha256_block(uint32_t h[8], const unsigned char* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = (uint32_t(p[4*i]) << 24) | (uint32_t(p[4*i+1]) << 16) | (uint32_t(p[4*i+2]) << 8) | uint32_t(p[4*i+3]);
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3), s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K256[i] + w[i];
    uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22), maj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + maj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}
}  // namespace

std::string xxh64_hex(const std::string& data) { return xxh64_hex(data.data(), data.size()); }

std::string xxh64_hex(const char* bytes, size_t len) {   // XXH64 reference algorithm, seed 0
  static constexpr uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL, P3 = 1609587929392839161ULL,
                            P4 = 9650029242287828579ULL, P5 = 2870177450012600261ULL;
  auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
  auto rd64 = [](const unsigned char* p) { uint64_t v; std::memcpy(&v, p, 8); return v; };   // little-endian hosts (x86-64, arm64)
  auto rd32 = [](const unsigned char* p) { uint32_t v; std::memcpy(&v, p, 4); return static_cast<uint64_t>(v); };
  auto round = [&](uint64_t acc, uint64_t in) { acc += in * P2; acc = rotl(acc, 31); return acc * P1; };
  auto merge = [&](uint64_t acc, uint64_t v) { acc ^= round(0, v); return acc * P1 + P4; };
  const auto* p = reinterpret_cast<const unsigned char*>(bytes);
  const size_t n = len; const unsigned char* const end = p + n;
  uint64_t h;
  if (n >= 32) {
    uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
    const unsigned char* const lim = end - 32;
    do { v1 = round(v1, rd64(p)); v2 = round(v2, rd64(p + 8)); v3 = round(v3, rd64(p + 16)); v4 = round(v4, rd64(p + 24)); p += 32; } while (p <= lim);
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
  } else h = P5;
  h += static_cast<uint64_t>(n);
  while (p + 8 <= end) { h ^= round(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; }
  if (p + 4 <= end) { h ^= rd32(p) * P1; h = rotl(h, 23) * P2 + P3; p += 4; }
  while (p < end) { h ^= (*p) * P5; h = rotl(h, 11) * P1; ++p; }
  h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
  char buf[17]; std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return std::string(buf, 16);
}

bool copy_verified_atomic(const std::string& src, const std::string& dst, int64_t size, const std::string& xxh, std::string* why) {
  auto fail = [&](const char* w) { if (why) *why = w; return false; };
  const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return fail("object-missing");
  struct stat st{};
  if (::fstat(in, &st) != 0) { ::close(in); return fail("object-missing"); }
  if (size >= 0 && st.st_size != size) { ::close(in); return fail("bad-object-size"); }
  if (!xxh.empty()) {   // hash the very inode that is copied below (no user-space copy of the bytes)
    std::string h;
    if (st.st_size == 0) h = xxh64_hex(nullptr, 0);
    else {
      void* m = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, in, 0);
      if (m == MAP_FAILED) { ::close(in); return fail("object-unreadable"); }
      h = xxh64_hex(static_cast<const char*>(m), static_cast<size_t>(st.st_size));
      ::munmap(m, static_cast<size_t>(st.st_size));
    }
    if (h != xxh) { ::close(in); return fail("bad-object-checksum"); }
  }
  const std::string tmp = dst + ".opticc-" + std::to_string(getpid());
  ::unlink(tmp.c_str());
  bool ok = true;
#ifdef __APPLE__
  if (::fclonefileat(in, AT_FDCWD, tmp.c_str(), 0) != 0) {
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    ok = out >= 0 && ::fcopyfile(in, out, nullptr, COPYFILE_DATA) == 0;
    if (out >= 0 && ::close(out) != 0) ok = false;
  }
#else
  const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (out < 0) ok = false;
  off_t left = ok ? st.st_size : 0, off = 0;
  while (ok && left > 0) {
    ssize_t n = ::copy_file_range(in, &off, out, nullptr, static_cast<size_t>(left), 0);
    if (n <= 0) {   // not supported here: plain copy
      char buf[1 << 16]; ssize_t r;
      while (left > 0 && (r = ::pread(in, buf, sizeof buf, off)) > 0) { if (::write(out, buf, static_cast<size_t>(r)) != r) { ok = false; break; } left -= r; off += r; }
      break;
    }
    left -= n;
  }
  if (left != 0) ok = false;
  if (out >= 0 && ::close(out) != 0) ok = false;
#endif
  ::close(in);
  if (!ok || std::rename(tmp.c_str(), dst.c_str()) != 0) { ::unlink(tmp.c_str()); return fail("object-copy-failed"); }
  return true;
}

std::string sha256_hex(const std::string& data) {
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  size_t n = data.size(), i = 0;
  for (; i + 64 <= n; i += 64) sha256_block(h, p + i);
  unsigned char tail[128]; size_t rem = n - i; std::memcpy(tail, p + i, rem);
  tail[rem++] = 0x80;
  size_t padlen = rem <= 56 ? 64 : 128;
  std::memset(tail + rem, 0, padlen - rem);
  uint64_t bits = static_cast<uint64_t>(n) * 8;
  for (int k = 0; k < 8; ++k) tail[padlen - 1 - static_cast<size_t>(k)] = static_cast<unsigned char>(bits >> (8 * k));
  sha256_block(h, tail); if (padlen == 128) sha256_block(h, tail + 64);
  static const char* hex = "0123456789abcdef";
  std::string out; out.reserve(64);
  for (uint32_t v : h) for (int k = 7; k >= 0; --k) out.push_back(hex[(v >> (4 * k)) & 0xf]);
  return out;
}

void mkdir_p(const std::string& dir) {
  std::error_code ec; fs::create_directories(dir, ec);
  if (ec && !fs::is_directory(dir)) throw std::runtime_error("mkdir -p " + dir + ": " + ec.message());
}
// Client paths are untrusted. Normalise lexically with POSIX root clamping ("." and empty components
// dropped, ".." pops a component and stays at "/" when there is none), so the joined result can never leave `root`; a
// path that normalises to "/" itself, or contains a NUL, is refused. The worker never creates symlinks inside a job
// root (only directories and hard links / copies of regular CAS files), so lexical containment is physical containment.
std::string lexically_normal_abs(const std::string& path) {
  if (path.find('\0') != std::string::npos) throw std::runtime_error("path contains NUL");
  std::vector<std::string> parts; size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find('/', i); if (j == std::string::npos) j = path.size();
    std::string c = path.substr(i, j - i);
    if (c == "..") { if (!parts.empty()) parts.pop_back(); }
    else if (!c.empty() && c != ".") parts.push_back(c);
    i = j + 1;
  }
  std::string out;
  for (auto& c : parts) { out.push_back('/'); out += c; }
  return out.empty() ? std::string("/") : out;
}
std::string safe_file_stem(const std::string& name) {
  std::string stem = name;
  auto slash = stem.find_last_of('/'); if (slash != std::string::npos) stem = stem.substr(slash + 1);
  auto dot = stem.rfind('.'); if (dot != std::string::npos) stem = stem.substr(0, dot);
  if (stem.empty() || stem == "." || stem == ".." || stem.find('\0') != std::string::npos) stem = "input";
  return stem;
}
std::string under_root(const std::string& root, const std::string& abs_path) {
  std::string n = lexically_normal_abs(abs_path.empty() || abs_path[0] != '/' ? "/" + abs_path : abs_path);
  if (n == "/") throw std::runtime_error("refusing a client path that names the job root itself: '" + abs_path + "'");
  return root + n;
}
bool path_within(const std::string& path, const std::string& dir) {
  if (dir.empty()) return false;
  std::string d = dir; if (d.size() > 1 && d.back() == '/') d.pop_back();
  return path == d || (path.size() > d.size() && path.compare(0, d.size(), d) == 0 && path[d.size()] == '/');
}
std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
  return s;
}

uint32_t resolve_ipv4(const std::string& host) {
  in_addr a{};
  if (inet_pton(AF_INET, host.c_str(), &a) == 1) return ntohl(a.s_addr);
  addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return 0;
  uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(res);
  return ip;
}
bool is_private_ipv4(uint32_t a) {
  return (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) /*172.16/12*/ ||
         (a >> 16) == (192u << 8 | 168) || (a >> 16) == (169u << 8 | 254) || (a >> 22) == (100u << 2 | 1) /*100.64/10*/;
}
bool on_vpn_interface(uint32_t addr) {
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) != 0) return false;
  bool hit = false;
  for (ifaddrs* i = ifs; i && !hit; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_name) continue;
    std::string n = i->ifa_name;
    if (!(n.rfind("wg", 0) == 0 || n.rfind("tun", 0) == 0 || n.rfind("utun", 0) == 0)) continue;
    uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    uint32_t mask = i->ifa_netmask ? ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr) : 0xffffffffu;
    if ((ip & mask) == (addr & mask)) hit = true;
  }
  freeifaddrs(ifs);
  return hit;
}

}  // namespace opticc::util
