// opticc: sockets and the coordinator-side worker connection.
// One persistent TCP connection per worker; requests are pipelined and a
// demux thread matches RESULT frames to pending promises by job id.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "opticc/protocol.hpp"

namespace opticc::noise { struct KeyPair; }

namespace opticc::net {

class Socket;

// Byte channel under the frame protocol: plain TCP or an
// encrypted Noise session. A channel owns no socket; it is handed the socket on
// every call so sockets stay movable. Callers serialise concurrent sends.
class Channel {
 public:
  virtual ~Channel() = default;
  virtual void send(const Socket& s, const char* p, size_t n) = 0;
  virtual void recv(const Socket& s, char* p, size_t n) = 0;
  virtual bool encrypted() const = 0;
  virtual std::string peer_key_b64() const { return ""; }
};
class PlainChannel : public Channel {
 public:
  void send(const Socket& s, const char* p, size_t n) override;
  void recv(const Socket& s, char* p, size_t n) override;
  bool encrypted() const override { return false; }
};

// Thin RAII socket. All operations throw std::runtime_error on failure.
class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  Socket(Socket&& o) noexcept : fd_(o.fd_), chan_(std::move(o.chan_)), rbuf_(std::move(o.rbuf_)), max_frame_(o.max_frame_) { o.fd_ = -1; }
  Socket& operator=(Socket&& o) noexcept { close(); fd_ = o.fd_; o.fd_ = -1; chan_ = std::move(o.chan_); rbuf_ = std::move(o.rbuf_); max_frame_ = o.max_frame_; return *this; }
  ~Socket() { close(); }
  Socket(const Socket&) = delete; Socket& operator=(const Socket&) = delete;

  static Socket connect_tcp(const std::string& host, int port, double timeout_s);
  static Socket listen_tcp(const std::string& bind_ip, int port, int backlog = 64);  // port 0 = ephemeral
  static Socket connect_unix(const std::string& path);
  static Socket listen_unix(const std::string& path);
  Socket accept(uint32_t* peer_ipv4 = nullptr) const;
  int local_port() const;

  void send_all(const char* p, size_t n) const;
  void recv_all(char* p, size_t n) const;        // throws on EOF
  void send_str(const std::string& s) const { send_all(s.data(), s.size()); }
  // Newline-delimited text (coordinator <-> client unix socket). Returns false on EOF.
  bool recv_line(std::string* line) const;
  // Frames go through the channel when one is set (set_channel), else raw.
  void write_frame(const proto::Frame& f) const;
  proto::Frame read_frame() const;               // throws on EOF / oversize
  // The largest frame read_frame accepts. A worker lowers it to kPreAuthFrame until the peer has
  // authenticated (Noise handshake or HELLO token), so an unauthenticated peer can never make it allocate more.
  void set_max_frame(uint32_t n) { max_frame_ = n; }
  uint32_t max_frame() const { return max_frame_; }
  static constexpr uint32_t kPreAuthFrame = 64u << 10;   // 64 KiB: a HELLO is < 1 KiB
  static uint32_t default_max_frame();                   // OPTICC_MAX_FRAME_MB, default 1024 (prelink frames carry objects)
  // A blocking read that waits longer than `s` seconds fails ("recv: Resource temporarily
  // unavailable"); 0 clears the limit. A worker bounds the unauthenticated handshake with it.
  void set_recv_timeout(double s) const;
  void set_channel(std::unique_ptr<Channel> c) { chan_ = std::move(c); }
  Channel* channel() const { return chan_.get(); }
  bool encrypted() const { return chan_ && chan_->encrypted(); }
  uint32_t peer_ipv4() const;                    // 0 if not IPv4
  int fd() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void close();
  void shutdown_both() const;

 private:
  int fd_ = -1;
  std::unique_ptr<Channel> chan_;
  mutable std::string rbuf_;                     // recv_line buffer
  uint32_t max_frame_ = default_max_frame();
};

// How the coordinator secures one worker connection (noise::decide output).
struct Security {
  enum Kind { Plain, Noise } kind = Noise;
  const noise::KeyPair* local = nullptr;         // our static key (noise)
  std::array<uint8_t, 32> remote_pub{};          // worker's static key (noise)
  bool have_psk = false; std::array<uint8_t, 32> psk{};   // plain-mode PSK (optional)
};
// Trust-on-first-use helper: ask a worker for its static public key (unauthenticated).
std::string fetch_worker_pubkey(const std::string& host, int port, double timeout_s = 5.0);

// Coordinator-side handle to one worker. Thread-safe.
class WorkerConn {
 public:
  WorkerConn(std::string host, int port, std::string token, Security sec = Security{});
  ~WorkerConn();
  // Connect + HELLO. Fills hello_ack. Throws on failure.
  void connect(double timeout_s = 5.0);
  bool connected() const { return alive_.load(); }
  const proto::json& hello_ack() const { return hello_ack_; }

  // Synchronous probes (only before pipelined traffic starts, or from the
  // owning thread — they use the same connection).
  double ping_rtt_s();
  // Two one-directional transfers; returns measured (up_bps, down_bps), RTT subtracted.
  std::pair<double, double> bandwidth_probe(size_t up_bytes, size_t down_bytes);
  // Local address of this connection (the route used to reach the worker; part of the probe-cache key)
  std::string local_addr() const;
  // The kernel's live TCP delivery-rate estimate (Linux TCP_INFO tcpi_delivery_rate, bits/s); false when
  // unavailable (non-Linux) or when the last sample was app-limited (it then under-states the link).
  bool tcp_delivery_rate(double* bps) const;

  // Pipelined compile: returns a future resolved by the demux thread.
  std::future<proto::CompileResult> compile(const proto::CompileRequest& req, int zstd_level,
                                            uint64_t* sent_zstd_bytes = nullptr);

  // Header-push session: the demux thread queues NEED/MISSING frames on
  // `events` and completes `result`; the owner answers with send_blobs().
  struct HpSession {
    std::mutex m; std::condition_variable cv;
    std::deque<proto::Frame> events;
    bool done = false; proto::CompileResult result; std::string error;
    proto::Frame raw; bool has_raw = false;      // for non-compile sessions (PRELINK): the reply frame itself
    // Blocks until an event or completion; returns false when done (result/error set).
    bool wait_event(proto::Frame* ev);
    // As wait_event, but returns false with *timed_out = true once `timeout_s` has passed with
    // neither an event nor completion (the owner then cancels and abandons the request).
    bool wait_event_for(proto::Frame* ev, double timeout_s, bool* timed_out);
  };
  // Generic pipelined request whose reply is a PRELINK_RESULT (or any frame) matched by `id`; NEED frames for the
  // same id are queued as events (the owner answers with send_blobs), exactly like header-push.
  std::shared_ptr<HpSession> request_session(const proto::Frame& f, int64_t id, uint64_t* sent_bytes = nullptr);
  std::shared_ptr<HpSession> compile_hp(const proto::HpRequest& req, int zstd_level, uint64_t* sent_bytes = nullptr);
  void send_blobs(const proto::BlobsMsg& m, int zstd_level, uint64_t* sent_bytes = nullptr);
  void send_prefetch(const proto::BlobsMsg& m, int zstd_level, uint64_t* sent_bytes = nullptr);   // prefetch, no reply
  void send_cache_put(const proto::CachePutMsg& m, int zstd_level, uint64_t* sent_bytes = nullptr);   // client upload (write-back), no reply
  void send_frame(const proto::Frame& f);   // any no-reply frame (DIRECT_PUT)
  void cancel(int64_t id);
  // Stop tracking request `id` (after its deadline passed): a late RESULT / NEED / MISSING for it is
  // dropped. Pair with cancel(id) so the worker kills the compile.
  void abandon(int64_t id);
  // How long one remote attempt may take from send to RESULT before the coordinator abandons it and
  // the job takes the error path (local fallback): OPTICC_ATTEMPT_TIMEOUT_S, default 3600 s (> the worker's own
  // compile timeout, so a worker that is merely slow reports first).
  static double attempt_timeout_s();
  // Fail every pending request (connection lost / shutting down).
  void fail_all(const std::string& why);
  void start_demux();
  const std::string& host() const { return host_; }
  int port() const { return port_; }
  bool encrypted() const { return sock_.encrypted(); }
  const Security& security() const { return sec_; }
  uint64_t bytes_sent() const { return bytes_sent_.load(); }
  uint64_t bytes_recv() const { return bytes_recv_.load(); }

 private:
  void send_locked(const proto::Frame& f);
  std::string host_; int port_; std::string token_;
  Security sec_;
  Socket sock_;
  std::mutex wmu_;
  std::mutex pmu_;
  std::unordered_map<int64_t, std::promise<proto::CompileResult>> pending_;
  std::unordered_map<int64_t, std::shared_ptr<HpSession>> hp_pending_;
  std::thread demux_;
  std::atomic<bool> alive_{false};
  std::atomic<uint64_t> bytes_sent_{0}, bytes_recv_{0};
  proto::json hello_ack_;
};

}  // namespace opticc::net
