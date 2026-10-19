// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/transport.hpp"
#include <cstddef>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include <cstdlib>

#include "opticc/noise.hpp"
#include "opticc/util.hpp"

namespace opticc::net {

void PlainChannel::send(const Socket& s, const char* p, size_t n) { s.send_all(p, n); }
void PlainChannel::recv(const Socket& s, char* p, size_t n) { s.recv_all(p, n); }

uint32_t Socket::default_max_frame() {
  static const uint32_t v = [] {
    const char* e = std::getenv("OPTICC_MAX_FRAME_MB");
    long long mb = e ? std::atoll(e) : 0;
    if (mb <= 0 || mb > 2047) mb = 1024;   // 1 GiB (link-domain PRELINK frames carry many objects)
    return static_cast<uint32_t>(mb) << 20;
  }();
  return v;
}

static std::runtime_error sys_err(const std::string& what) { return std::runtime_error(what + ": " + std::strerror(errno)); }

// TCP keepalive on every worker connection, so a peer that vanished (host down, cable pulled, NAT
// state lost) is detected in about 2 minutes instead of never: the blocked read fails, the connection is marked lost and
// its jobs take the error path. Idle 60 s, then 4 probes 15 s apart.
static void set_keepalive(int fd) {
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#if defined(__linux__)
  int idle = 60, intvl = 15, cnt = 4;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#elif defined(__APPLE__)
  int idle = 60; setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle);
#endif
}

void Socket::set_recv_timeout(double s) const {
  struct timeval tv{};
  if (s > 0) { tv.tv_sec = static_cast<time_t>(s); tv.tv_usec = static_cast<suseconds_t>((s - static_cast<double>(tv.tv_sec)) * 1e6); }
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

Socket Socket::connect_tcp(const std::string& host, int port, double timeout_s) {
  addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
    throw std::runtime_error("cannot resolve " + host);
  int fd = -1; std::string last = "connect failed";
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      rc = ::poll(&p, 1, static_cast<int>(timeout_s * 1000));
      int err = 0; socklen_t len = sizeof err;
      if (rc > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) rc = 0;
      else { rc = -1; last = rc == 0 ? "connect timeout" : std::string("connect: ") + std::strerror(err ? err : errno); }
    } else if (rc < 0) last = std::string("connect: ") + std::strerror(errno);
    if (rc == 0) { fcntl(fd, F_SETFL, fl); break; }
    ::close(fd); fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) throw std::runtime_error(last + " (" + host + ":" + std::to_string(port) + ")");
  int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  set_keepalive(fd);
  return Socket(fd);
}

Socket Socket::listen_tcp(const std::string& bind_ip, int port, int backlog) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw sys_err("socket");
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, bind_ip.c_str(), &a.sin_addr) != 1) { ::close(fd); throw std::runtime_error("bad bind ip " + bind_ip); }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); throw sys_err("bind"); }
  if (::listen(fd, backlog) < 0) { ::close(fd); throw sys_err("listen"); }
  return Socket(fd);
}

Socket Socket::connect_unix(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) throw sys_err("socket");
  sockaddr_un a{}; a.sun_family = AF_UNIX;
  if (path.size() >= sizeof a.sun_path) { ::close(fd); throw std::runtime_error("unix path too long"); }
  std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { int e = errno; ::close(fd); errno = e; throw sys_err("connect " + path); }
  return Socket(fd);
}
Socket Socket::listen_unix(const std::string& path) {
  ::unlink(path.c_str());
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) throw sys_err("socket");
  sockaddr_un a{}; a.sun_family = AF_UNIX;
  if (path.size() >= sizeof a.sun_path) { ::close(fd); throw std::runtime_error("unix path too long"); }
  std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) { ::close(fd); throw sys_err("bind " + path); }
  chmod(path.c_str(), 0600);
  if (::listen(fd, 128) < 0) { ::close(fd); throw sys_err("listen"); }
  return Socket(fd);
}

Socket Socket::accept(uint32_t* peer_ipv4) const {
  sockaddr_storage ss{}; socklen_t len = sizeof ss;
  int c;
  do { c = ::accept(fd_, reinterpret_cast<sockaddr*>(&ss), &len); } while (c < 0 && errno == EINTR);
  if (c < 0) throw sys_err("accept");
  if (peer_ipv4) {
    *peer_ipv4 = 0;
    if (ss.ss_family == AF_INET) *peer_ipv4 = ntohl(reinterpret_cast<sockaddr_in*>(&ss)->sin_addr.s_addr);
  }
  int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  set_keepalive(c);
  return Socket(c);
}
int Socket::local_port() const {
  sockaddr_in a{}; socklen_t len = sizeof a;
  if (getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) < 0) return -1;
  return ntohs(a.sin_port);
}

void Socket::send_all(const char* p, size_t n) const {
  while (n) {
#ifdef MSG_NOSIGNAL
    ssize_t k = ::send(fd_, p, n, MSG_NOSIGNAL);
#else
    ssize_t k = ::send(fd_, p, n, 0);
#endif
    if (k < 0) { if (errno == EINTR) continue; throw sys_err("send"); }
    p += k; n -= static_cast<size_t>(k);
  }
}
void Socket::recv_all(char* p, size_t n) const {
  // drain any bytes buffered by recv_line first
  if (!rbuf_.empty()) {
    size_t k = std::min(n, rbuf_.size());
    std::memcpy(p, rbuf_.data(), k); rbuf_.erase(0, k); p += k; n -= k;
  }
  while (n) {
    ssize_t k = ::recv(fd_, p, n, 0);
    if (k < 0) { if (errno == EINTR) continue; throw sys_err("recv"); }
    if (k == 0) throw std::runtime_error("connection closed");
    p += k; n -= static_cast<size_t>(k);
  }
}
bool Socket::recv_line(std::string* line) const {
  for (;;) {
    auto nl = rbuf_.find('\n');
    if (nl != std::string::npos) { *line = rbuf_.substr(0, nl); rbuf_.erase(0, nl + 1); return true; }
    char buf[65536];
    ssize_t k = ::recv(fd_, buf, sizeof buf, 0);
    if (k < 0) { if (errno == EINTR) continue; throw sys_err("recv"); }
    if (k == 0) { if (rbuf_.empty()) return false; *line = rbuf_; rbuf_.clear(); return true; }
    rbuf_.append(buf, static_cast<size_t>(k));
  }
}
void Socket::write_frame(const proto::Frame& f) const {
  std::string hdr; proto::put_u32(hdr, static_cast<uint32_t>(f.payload.size() + 1)); hdr.push_back(static_cast<char>(f.type));
  if (chan_) {
    // one channel write per frame keeps small frames in one encrypted record
    if (f.payload.size() <= 65536) { hdr += f.payload; chan_->send(*this, hdr.data(), hdr.size()); }
    else { chan_->send(*this, hdr.data(), hdr.size()); chan_->send(*this, f.payload.data(), f.payload.size()); }
    return;
  }
  send_all(hdr.data(), hdr.size());
  send_all(f.payload.data(), f.payload.size());
}
proto::Frame Socket::read_frame() const {
  auto rd = [&](char* p, size_t n) { if (chan_) chan_->recv(*this, p, n); else recv_all(p, n); };
  char h[4]; rd(h, 4);
  uint32_t n = proto::get_u32(h);
  if (n < 1 || n > max_frame_) throw std::runtime_error("bad frame length (" + std::to_string(n) + " > limit " + std::to_string(max_frame_) + ")");
  proto::Frame f; char t; rd(&t, 1); f.type = static_cast<proto::Type>(t);
  f.payload.resize(n - 1);
  if (n > 1) rd(f.payload.data(), n - 1);
  return f;
}
uint32_t Socket::peer_ipv4() const {
  sockaddr_storage ss{}; socklen_t len = sizeof ss;
  if (getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &len) < 0 || ss.ss_family != AF_INET) return 0;
  return ntohl(reinterpret_cast<sockaddr_in*>(&ss)->sin_addr.s_addr);
}

std::string fetch_worker_pubkey(const std::string& host, int port, double timeout_s) {
  Socket s = Socket::connect_tcp(host, port, timeout_s);
  s.send_str(noise::kPubkeyQuery);
  std::string line;
  if (!s.recv_line(&line) || line.empty()) throw std::runtime_error("worker did not answer the key query (older opticcd?)");
  noise::Key k; if (!noise::parse_pub(line, &k)) throw std::runtime_error("worker returned a malformed key");
  return line;
}
void Socket::close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
void Socket::shutdown_both() const { if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR); }

// ---- WorkerConn -----------------------------------------------------------
WorkerConn::WorkerConn(std::string host, int port, std::string token, Security sec)
    : host_(std::move(host)), port_(port), token_(std::move(token)), sec_(sec) {}

WorkerConn::~WorkerConn() {
  alive_ = false;
  if (sock_.valid()) { try { std::lock_guard<std::mutex> g(wmu_); sock_.write_frame({proto::Type::Bye, {}}); } catch (...) {} sock_.shutdown_both(); }
  if (demux_.joinable()) demux_.join();
  fail_all("shutdown");
}

void WorkerConn::connect(double timeout_s) {
  sock_ = Socket::connect_tcp(host_, port_, timeout_s);
  if (sec_.kind == Security::Noise) {
    if (!sec_.local) throw std::runtime_error("noise: no client key");
    sock_.set_channel(noise::initiate(sock_, *sec_.local, sec_.remote_pub));
  } else {
    // plain: magic, then the worker's PSK flag (0 = none, 1 = 32-byte challenge follows)
    sock_.send_str(noise::kPlainMagic);
    char flag = 0; sock_.recv_all(&flag, 1);
    if (flag == 1) {
      std::string ch(32, '\0'); sock_.recv_all(ch.data(), 32);
      if (!sec_.have_psk) throw std::runtime_error("worker requires a pre-shared key for plain mode (OPTICC_PSK_FILE)");
      sock_.send_str(noise::psk_tag(sec_.psk, ch));
    } else if (flag != 0) throw std::runtime_error("worker refused plain connection (start it with --accept plain)");
    sock_.set_channel(std::make_unique<PlainChannel>());
  }
  proto::json hello{{"token", token_}, {"version", proto::kVersion}, {"client", util::hostname()}};
  sock_.write_frame({proto::Type::Hello, hello.dump()});
  proto::Frame ack = sock_.read_frame();
  if (ack.type != proto::Type::HelloAck) throw std::runtime_error("worker rejected HELLO");
  hello_ack_ = proto::json::parse(ack.payload);
  if (hello_ack_.value("error", std::string()).size()) throw std::runtime_error("worker: " + hello_ack_["error"].get<std::string>());
  alive_ = true;
}

double WorkerConn::ping_rtt_s() {
  double t0 = util::now_s();
  sock_.write_frame({proto::Type::Ping, {}});
  proto::Frame f = sock_.read_frame();
  if (f.type != proto::Type::Pong) throw std::runtime_error("expected PONG");
  return util::now_s() - t0;
}

// Two one-directional probes: upload N bytes (tiny reply) and request N bytes
// (tiny request). Each costs one RTT plus the transfer, so bps = 8N/(t - rtt).
std::pair<double, double> WorkerConn::bandwidth_probe(size_t up_bytes, size_t down_bytes) {
  double rtt = ping_rtt_s();
  auto one = [&](size_t up, size_t down) {
    proto::Frame f{proto::Type::Bw, proto::pack(proto::json{{"down_bytes", down}}, std::string(up, 'x'))};
    double t0 = util::now_s();
    sock_.write_frame(f);
    proto::Frame ack = sock_.read_frame();
    double t = util::now_s() - t0;
    if (ack.type != proto::Type::BwAck) throw std::runtime_error("expected BW_ACK");
    return std::max(t - rtt, 1e-5);
  };
  double tu = one(up_bytes, 0), td = one(0, down_bytes);
  return {static_cast<double>(up_bytes) * 8.0 / tu, static_cast<double>(down_bytes) * 8.0 / td};
}

std::string WorkerConn::local_addr() const {
  sockaddr_storage ss{}; socklen_t len = sizeof ss;
  if (::getsockname(sock_.fd(), reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "";
  char buf[INET6_ADDRSTRLEN] = {0};
  if (ss.ss_family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&ss)->sin_addr, buf, sizeof buf);
  else if (ss.ss_family == AF_INET6) inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr, buf, sizeof buf);
  else return "local";
  return buf;
}

bool WorkerConn::tcp_delivery_rate(double* bps) const {
#if defined(__linux__)
  // Read struct tcp_info by its stable kernel ABI layout (glibc's netinet/tcp.h may lack the newer fields):
  // byte 7 bit 0 = tcpi_delivery_rate_app_limited, u64 tcpi_delivery_rate (bytes/s) at offset 160 (Linux >= 4.9).
  unsigned char buf[256] = {0}; socklen_t len = sizeof buf;
  if (::getsockopt(sock_.fd(), IPPROTO_TCP, 11 /* TCP_INFO */, buf, &len) != 0 || len < 168) return false;
  if (buf[7] & 1) return false;                       // app-limited sample: under-states the link
  uint64_t rate = 0; std::memcpy(&rate, buf + 160, sizeof rate);
  if (rate == 0) return false;
  *bps = static_cast<double>(rate) * 8.0;
  return true;
#else
  (void)bps; return false;
#endif
}

void WorkerConn::send_locked(const proto::Frame& f) {
  std::lock_guard<std::mutex> g(wmu_);
  sock_.write_frame(f);
  bytes_sent_ += f.payload.size() + 5;
}

std::future<proto::CompileResult> WorkerConn::compile(const proto::CompileRequest& req, int zstd_level,
                                                      uint64_t* sent_zstd_bytes) {
  std::promise<proto::CompileResult> pr; auto fut = pr.get_future();
  if (!alive_) { pr.set_exception(std::make_exception_ptr(std::runtime_error("worker not connected"))); return fut; }
  { std::lock_guard<std::mutex> g(pmu_); pending_.emplace(req.id, std::move(pr)); }
  try {
    uint64_t z = 0;
    proto::Frame f = proto::encode_compile(req, zstd_level, &z);
    if (sent_zstd_bytes) *sent_zstd_bytes = z;
    send_locked(f);
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> g(pmu_);
    auto it = pending_.find(req.id);
    if (it != pending_.end()) { it->second.set_exception(std::make_exception_ptr(std::runtime_error(e.what()))); pending_.erase(it); }
    alive_ = false;
  }
  return fut;
}

void WorkerConn::cancel(int64_t id) {
  if (!alive_) return;
  try { send_locked({proto::Type::Cancel, proto::json{{"id", id}}.dump()}); } catch (...) { alive_ = false; }
}

void WorkerConn::abandon(int64_t id) {
  std::lock_guard<std::mutex> g(pmu_);
  auto it = pending_.find(id);
  if (it != pending_.end()) { try { it->second.set_exception(std::make_exception_ptr(std::runtime_error("abandoned"))); } catch (...) {} pending_.erase(it); }
  hp_pending_.erase(id);
}

double WorkerConn::attempt_timeout_s() {
  static const double v = [] { double t = std::atof(util::getenv_or("OPTICC_ATTEMPT_TIMEOUT_S", "3600").c_str()); return t > 0 ? t : 3600.0; }();
  return v;
}

void WorkerConn::fail_all(const std::string& why) {
  std::lock_guard<std::mutex> g(pmu_);
  for (auto& [id, p] : pending_) { try { p.set_exception(std::make_exception_ptr(std::runtime_error(why))); } catch (...) {} }
  pending_.clear();
  for (auto& [id, s] : hp_pending_) { std::lock_guard<std::mutex> l(s->m); s->error = why; s->done = true; s->cv.notify_all(); }
  hp_pending_.clear();
}

bool WorkerConn::HpSession::wait_event(proto::Frame* ev) {
  std::unique_lock<std::mutex> l(m);
  cv.wait(l, [&] { return done || !events.empty(); });
  if (!events.empty()) { *ev = std::move(events.front()); events.pop_front(); return true; }
  return false;
}
bool WorkerConn::HpSession::wait_event_for(proto::Frame* ev, double timeout_s, bool* timed_out) {
  std::unique_lock<std::mutex> l(m);
  *timed_out = !cv.wait_for(l, std::chrono::duration<double>(std::max(0.0, timeout_s)), [&] { return done || !events.empty(); });
  if (!events.empty()) { *ev = std::move(events.front()); events.pop_front(); return true; }
  return false;
}

std::shared_ptr<WorkerConn::HpSession> WorkerConn::request_session(const proto::Frame& f, int64_t id, uint64_t* sent_bytes) {
  auto s = std::make_shared<HpSession>();
  { std::lock_guard<std::mutex> g(pmu_); hp_pending_[id] = s; }
  try { send_locked(f); if (sent_bytes) *sent_bytes = f.payload.size() + 5; }
  catch (...) { { std::lock_guard<std::mutex> g(pmu_); hp_pending_.erase(id); } throw; }
  return s;
}

std::shared_ptr<WorkerConn::HpSession> WorkerConn::compile_hp(const proto::HpRequest& req, int zstd_level, uint64_t* sent_bytes) {
  auto s = std::make_shared<HpSession>();
  if (!alive_) { s->error = "worker not connected"; s->done = true; return s; }
  { std::lock_guard<std::mutex> g(pmu_); hp_pending_[req.id] = s; }
  try {
    uint64_t z = 0;
    proto::Frame f = proto::encode_compile_hp(req, zstd_level, &z);
    if (sent_bytes) *sent_bytes = f.payload.size() + 5;
    send_locked(f);
  } catch (const std::exception& e) {
    { std::lock_guard<std::mutex> g(pmu_); hp_pending_.erase(req.id); }
    std::lock_guard<std::mutex> l(s->m); s->error = e.what(); s->done = true; s->cv.notify_all();
    alive_ = false;
  }
  return s;
}

void WorkerConn::send_blobs(const proto::BlobsMsg& m, int zstd_level, uint64_t* sent_bytes) {
  proto::Frame f = proto::encode_blobs(m, zstd_level);
  if (sent_bytes) *sent_bytes = f.payload.size() + 5;
  send_locked(f);
}

void WorkerConn::send_prefetch(const proto::BlobsMsg& m, int zstd_level, uint64_t* sent_bytes) {
  proto::Frame f = proto::encode_prefetch(m, zstd_level);
  if (sent_bytes) *sent_bytes = f.payload.size() + 5;
  send_locked(f);
}

void WorkerConn::send_cache_put(const proto::CachePutMsg& m, int zstd_level, uint64_t* sent_bytes) {
  proto::Frame f = proto::encode_cache_put(m, zstd_level, sent_bytes);
  send_locked(f);
}

void WorkerConn::send_frame(const proto::Frame& f) { send_locked(f); }

void WorkerConn::start_demux() {
  demux_ = std::thread([this] {
    try {
      while (alive_) {
        proto::Frame f = sock_.read_frame();
        bytes_recv_ += f.payload.size() + 5;
        if (f.type == proto::Type::Result) {
          proto::CompileResult r = proto::decode_result(f);
          std::promise<proto::CompileResult> p; bool found = false; std::shared_ptr<HpSession> hs;
          { std::lock_guard<std::mutex> g(pmu_);
            auto it = pending_.find(r.id); if (it != pending_.end()) { p = std::move(it->second); pending_.erase(it); found = true; }
            auto ht = hp_pending_.find(r.id); if (ht != hp_pending_.end()) { hs = ht->second; hp_pending_.erase(ht); } }
          if (found) p.set_value(std::move(r));
          else if (hs) { std::lock_guard<std::mutex> l(hs->m); hs->result = std::move(r); hs->done = true; hs->cv.notify_all(); }
        } else if (f.type == proto::Type::PrelinkResult || f.type == proto::Type::DirectReply) {   // DIRECT_REPLY matched by id like PRELINK_RESULT
          int64_t id = -1;
          try { proto::json h; std::string z; proto::unpack(f.payload, &h, &z); id = h.value("id", int64_t(-1)); } catch (...) {}
          std::shared_ptr<HpSession> hs;
          { std::lock_guard<std::mutex> g(pmu_); auto ht = hp_pending_.find(id); if (ht != hp_pending_.end()) { hs = ht->second; hp_pending_.erase(ht); } }
          if (hs) { std::lock_guard<std::mutex> l(hs->m); hs->raw = std::move(f); hs->has_raw = true; hs->done = true; hs->cv.notify_all(); }
        } else if (f.type == proto::Type::Need || f.type == proto::Type::Missing) {
          int64_t id = proto::json::parse(f.payload).value("id", int64_t(-1));
          std::shared_ptr<HpSession> hs;
          { std::lock_guard<std::mutex> g(pmu_); auto ht = hp_pending_.find(id); if (ht != hp_pending_.end()) hs = ht->second; }
          if (hs) { std::lock_guard<std::mutex> l(hs->m); hs->events.push_back(std::move(f)); hs->cv.notify_all(); }
        } else if (f.type == proto::Type::Pong) {
          // stray pong (ping issued concurrently) — ignore
        } else if (f.type == proto::Type::Bye) {
          break;
        }
      }
    } catch (const std::exception&) {
    }
    alive_ = false;
    fail_all("worker connection lost: " + host_ + ":" + std::to_string(port_));
  });
}

}  // namespace opticc::net
