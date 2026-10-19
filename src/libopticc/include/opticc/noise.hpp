// opticc: connection security.
//
// Default channel: Noise_IK_25519_ChaChaPoly_BLAKE2b (Noise Protocol Framework
// rev. 34, pattern IK, HASHLEN = 64, BLOCKLEN = 128) on libsodium primitives:
//   DH    X25519                     crypto_scalarmult_curve25519
//   AEAD  ChaCha20-Poly1305 (IETF)   crypto_aead_chacha20poly1305_ietf_*
//   HASH  BLAKE2b-512                crypto_generichash_blake2b (unkeyed); HMAC/HKDF per the Noise spec
// BLAKE2s is not available in libsodium, hence BLAKE2b; this only changes the
// protocol name string, not the security argument.
//
// IK: the initiator (coordinator) knows the responder's (worker's) static public
// key in advance and sends its own static key encrypted in the first message, so
// nothing identifying ever crosses the wire in the clear and there is no
// negotiation an attacker could downgrade. Both static keys are authenticated:
// the responder checks the initiator's key against `authorized_clients`.
//
//   -> e, es, s, ss   (+ empty payload)
//   <- e, ee, se      (+ empty payload)
//   Split() -> c_i2r, c_r2i
//
// Handshake messages are sent as u16 big-endian length + bytes. Transport
// records are u16 big-endian ciphertext length + AEAD(plaintext <= 65519 bytes,
// nonce = 32 zero bits || little-endian u64 counter, ad = empty). Each direction
// rekeys (Noise Rekey()) after 1 GiB of plaintext or 2^32 records, whichever
// comes first; both ends count identically so no signalling is needed.
//
// Prologue "opticc-noise-v1" is mixed into the handshake hash by both sides.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace opticc::net { class Socket; class Channel; }

namespace opticc::noise {

constexpr const char* kProtocolName = "Noise_IK_25519_ChaChaPoly_BLAKE2b";
constexpr const char* kPrologue = "opticc-noise-v1";
constexpr size_t kKeyLen = 32;
constexpr size_t kMaxRecordPlain = 65535 - 16;   // Noise message limit minus the tag
constexpr uint64_t kRekeyBytes = 1ULL << 30;      // 1 GiB per direction
constexpr uint64_t kRekeyRecords = 1ULL << 32;

using Key = std::array<uint8_t, kKeyLen>;

struct KeyPair {
  Key pub{}, priv{};
  static KeyPair generate();
  // base64 (standard alphabet, padded)
  std::string pub_b64() const;
  std::string priv_b64() const;
};
std::string b64(const uint8_t* p, size_t n);
bool from_b64(const std::string& s, uint8_t* out, size_t n);   // exact length required
bool parse_pub(const std::string& b64s, Key* out);
// SSH-style fingerprint: "SHA256:" + base64(SHA-256(pub)) without padding.
std::string fingerprint(const Key& pub);

// Load or create a key pair in `dir` as <stem>.key (0600, base64 private) and <stem>.pub.
// Returns true if the pair was newly created.
bool load_or_create_keypair(const std::string& dir, const std::string& stem, KeyPair* kp, bool create = true);

// --- raw cipher state (exposed for tests and the throughput benchmark) ---------
struct CipherState {
  Key k{}; bool has_key = false; uint64_t n = 0;
  uint64_t records = 0, bytes = 0;   // since last rekey
  void init(const Key& key) { k = key; has_key = true; n = 0; records = 0; bytes = 0; }
  std::string encrypt_with_ad(const std::string& ad, const std::string& pt);
  std::string decrypt_with_ad(const std::string& ad, const std::string& ct);   // throws on failure
  void rekey();
};

// Handshake. `authorize` (responder side) sees the initiator's static key before
// the second message is sent; returning false aborts the handshake.
struct HandshakeResult {
  CipherState send, recv;
  Key remote_static{};
  std::array<uint8_t, 64> handshake_hash{};
};
// Returns the first message to send.
class Initiator {
 public:
  Initiator(const KeyPair& local, const Key& remote_static);
  std::string write_message1();
  HandshakeResult read_message2(const std::string& msg2);   // throws on failure
  // Tests only: use this ephemeral key pair instead of a random one (known-answer vectors).
  void set_test_ephemeral(const KeyPair& e);
 private:
  struct Impl; std::shared_ptr<Impl> p_;
};
class Responder {
 public:
  explicit Responder(const KeyPair& local);
  // Processes message 1; returns the initiator's static key (for authorization).
  Key read_message1(const std::string& msg1);               // throws on failure
  std::string write_message2();
  HandshakeResult finish();
  void set_test_ephemeral(const KeyPair& e);                // tests only
 private:
  struct Impl; std::shared_ptr<Impl> p_;
};

// Socket-level helpers: run the handshake over `s` and return the encrypted channel.
std::unique_ptr<net::Channel> initiate(const net::Socket& s, const KeyPair& local, const Key& remote_static);
// `prefix` = bytes of message 1 already consumed by the caller's protocol sniffing (0..2 bytes).
std::unique_ptr<net::Channel> respond(const net::Socket& s, const KeyPair& local, const std::string& prefix,
                                      const std::function<bool(const Key&)>& authorize, Key* initiator_key = nullptr);

// Encrypt+decrypt `bytes` in 64 KiB records with a throwaway key; returns bytes per second
// (both operations counted once, i.e. the client's cost per byte transferred).
double benchmark_bytes_per_s(size_t bytes = 16u << 20);

// --- plain channel helpers --------------------------------------------------------------
constexpr const char* kPlainMagic = "OPTICC-PLAIN\n";
constexpr const char* kPubkeyQuery = "OPTICC-PUBKEY?\n";
// Pre-shared key for plain mode: SHA-256 of the file contents (any length >= 16 bytes).
bool load_psk(const std::string& path, Key* out);
// HMAC-SHA-256 challenge/response (crypto_auth): 32-byte challenge -> 32-byte tag.
std::string psk_tag(const Key& psk, const std::string& challenge);
bool psk_verify(const Key& psk, const std::string& challenge, const std::string& tag);

// --- policy ---------------------------------------------------------------------------
enum class SecMode { Auto, Noise, Plain };
SecMode parse_sec_mode(const std::string& s);   // "auto" | "noise" | "plain" (default Auto)
struct SecDecision {
  enum Kind { Noise, Plain, Tofu, Refuse } kind = Refuse;
  std::string reason;
};
// key_known: worker static key available (host spec or known_workers); spec_plain: host spec asked for plain;
// tofu: OPTICC_TOFU=1; addr_private: destination is loopback/RFC1918/link-local/CGNAT or on a wg*/tun*/utun* subnet.
SecDecision decide(SecMode mode, bool key_known, bool spec_plain, bool tofu, bool addr_private);

}  // namespace opticc::noise
