// SPDX-License-Identifier: GPL-2.0-only
#include "opticc/noise.hpp"

#include <sodium.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "opticc/transport.hpp"
#include "opticc/util.hpp"

namespace opticc::noise {

namespace {

constexpr size_t HASHLEN = 64, BLOCKLEN = 128;
using Hash = std::array<uint8_t, HASHLEN>;

void ensure_sodium() {
  static const int rc = sodium_init();
  if (rc < 0) throw std::runtime_error("libsodium initialisation failed");
}

Hash hash(const uint8_t* a, size_t na, const uint8_t* b = nullptr, size_t nb = 0) {
  Hash out{};
  crypto_generichash_blake2b_state st;
  crypto_generichash_blake2b_init(&st, nullptr, 0, HASHLEN);
  if (na) crypto_generichash_blake2b_update(&st, a, na);
  if (nb) crypto_generichash_blake2b_update(&st, b, nb);
  crypto_generichash_blake2b_final(&st, out.data(), HASHLEN);
  return out;
}

// HMAC-BLAKE2b per the Noise spec (HMAC construction over HASH, BLOCKLEN padding), not keyed BLAKE2.
Hash hmac(const uint8_t* key, size_t klen, const uint8_t* d1, size_t n1, const uint8_t* d2 = nullptr, size_t n2 = 0) {
  uint8_t k[BLOCKLEN] = {0};
  if (klen > BLOCKLEN) { Hash h = hash(key, klen); std::memcpy(k, h.data(), HASHLEN); }
  else std::memcpy(k, key, klen);
  uint8_t ipad[BLOCKLEN], opad[BLOCKLEN];
  for (size_t i = 0; i < BLOCKLEN; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
  crypto_generichash_blake2b_state st;
  Hash inner{};
  crypto_generichash_blake2b_init(&st, nullptr, 0, HASHLEN);
  crypto_generichash_blake2b_update(&st, ipad, BLOCKLEN);
  if (n1) crypto_generichash_blake2b_update(&st, d1, n1);
  if (n2) crypto_generichash_blake2b_update(&st, d2, n2);
  crypto_generichash_blake2b_final(&st, inner.data(), HASHLEN);
  Hash out{};
  crypto_generichash_blake2b_init(&st, nullptr, 0, HASHLEN);
  crypto_generichash_blake2b_update(&st, opad, BLOCKLEN);
  crypto_generichash_blake2b_update(&st, inner.data(), HASHLEN);
  crypto_generichash_blake2b_final(&st, out.data(), HASHLEN);
  sodium_memzero(k, sizeof k); sodium_memzero(ipad, sizeof ipad); sodium_memzero(opad, sizeof opad);
  return out;
}

// HKDF(ck, ikm, 2)
void hkdf2(const Hash& ck, const uint8_t* ikm, size_t n, Hash* o1, Hash* o2) {
  Hash temp = hmac(ck.data(), HASHLEN, ikm, n);
  uint8_t one = 1, two = 2;
  *o1 = hmac(temp.data(), HASHLEN, &one, 1);
  *o2 = hmac(temp.data(), HASHLEN, o1->data(), HASHLEN, &two, 1);
  sodium_memzero(temp.data(), HASHLEN);
}

Key dh(const Key& priv, const Key& pub) {
  Key out{};
  if (crypto_scalarmult_curve25519(out.data(), priv.data(), pub.data()) != 0)
    throw std::runtime_error("noise: X25519 failed (low-order point)");
  return out;
}

void nonce_bytes(uint64_t n, uint8_t out[12]) {
  std::memset(out, 0, 4);
  for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<uint8_t>(n >> (8 * i));
}

struct SymmetricState {
  Hash h{}, ck{};
  CipherState cs;
  void init() {
    std::string name = kProtocolName;
    if (name.size() <= HASHLEN) { h.fill(0); std::memcpy(h.data(), name.data(), name.size()); }
    else h = hash(reinterpret_cast<const uint8_t*>(name.data()), name.size());
    ck = h; cs = CipherState{};
  }
  void mix_hash(const uint8_t* d, size_t n) { h = hash(h.data(), HASHLEN, d, n); }
  void mix_key(const Key& ikm) {
    Hash tk; hkdf2(ck, ikm.data(), ikm.size(), &ck, &tk);
    Key k; std::memcpy(k.data(), tk.data(), kKeyLen);   // HASHLEN 64 -> truncate to 32
    cs.init(k);
    sodium_memzero(tk.data(), HASHLEN); sodium_memzero(k.data(), kKeyLen);
  }
  std::string encrypt_and_hash(const std::string& pt) {
    std::string ad(reinterpret_cast<const char*>(h.data()), HASHLEN);
    std::string ct = cs.has_key ? cs.encrypt_with_ad(ad, pt) : pt;
    mix_hash(reinterpret_cast<const uint8_t*>(ct.data()), ct.size());
    return ct;
  }
  std::string decrypt_and_hash(const std::string& ct) {
    std::string ad(reinterpret_cast<const char*>(h.data()), HASHLEN);
    std::string pt = cs.has_key ? cs.decrypt_with_ad(ad, ct) : ct;
    mix_hash(reinterpret_cast<const uint8_t*>(ct.data()), ct.size());
    return pt;
  }
  void split(CipherState* c1, CipherState* c2) {
    Hash t1, t2; hkdf2(ck, nullptr, 0, &t1, &t2);
    Key k1, k2; std::memcpy(k1.data(), t1.data(), kKeyLen); std::memcpy(k2.data(), t2.data(), kKeyLen);
    c1->init(k1); c2->init(k2);
    sodium_memzero(t1.data(), HASHLEN); sodium_memzero(t2.data(), HASHLEN);
  }
};

void put_u16(std::string& out, size_t v) { out.push_back(static_cast<char>((v >> 8) & 0xff)); out.push_back(static_cast<char>(v & 0xff)); }
size_t get_u16(const char* p) { return (static_cast<size_t>(static_cast<uint8_t>(p[0])) << 8) | static_cast<uint8_t>(p[1]); }

}  // namespace

// ---- CipherState --------------------------------------------------------------------
std::string CipherState::encrypt_with_ad(const std::string& ad, const std::string& pt) {
  if (!has_key) return pt;
  if (n == UINT64_MAX) throw std::runtime_error("noise: nonce exhausted");
  uint8_t nonce[12]; nonce_bytes(n, nonce);
  std::string ct(pt.size() + crypto_aead_chacha20poly1305_ietf_ABYTES, '\0');
  unsigned long long clen = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(ct.data()), &clen,
                                            reinterpret_cast<const unsigned char*>(pt.data()), pt.size(),
                                            reinterpret_cast<const unsigned char*>(ad.data()), ad.size(),
                                            nullptr, nonce, k.data());
  ct.resize(static_cast<size_t>(clen));
  ++n;
  return ct;
}
std::string CipherState::decrypt_with_ad(const std::string& ad, const std::string& ct) {
  if (!has_key) return ct;
  if (n == UINT64_MAX) throw std::runtime_error("noise: nonce exhausted");
  if (ct.size() < crypto_aead_chacha20poly1305_ietf_ABYTES) throw std::runtime_error("noise: short ciphertext");
  uint8_t nonce[12]; nonce_bytes(n, nonce);
  std::string pt(ct.size() - crypto_aead_chacha20poly1305_ietf_ABYTES, '\0');
  unsigned long long plen = 0;
  if (crypto_aead_chacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(pt.data()), &plen, nullptr,
                                                reinterpret_cast<const unsigned char*>(ct.data()), ct.size(),
                                                reinterpret_cast<const unsigned char*>(ad.data()), ad.size(),
                                                nonce, k.data()) != 0)
    throw std::runtime_error("noise: authentication failed");
  pt.resize(static_cast<size_t>(plen));
  ++n;
  return pt;
}
void CipherState::rekey() {   // Noise REKEY(k) = ENCRYPT(k, maxnonce, zerolen, zeros(32))[0:32]
  uint8_t nonce[12]; nonce_bytes(UINT64_MAX, nonce);
  uint8_t zeros[32] = {0}, out[32 + 16]; unsigned long long clen = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(out, &clen, zeros, 32, nullptr, 0, nullptr, nonce, k.data());
  std::memcpy(k.data(), out, 32);
  sodium_memzero(out, sizeof out);
  records = 0; bytes = 0;   // n is NOT reset by Rekey() (Noise §4.2); we keep counting
}

// ---- keys ----------------------------------------------------------------------------
KeyPair KeyPair::generate() {
  ensure_sodium();
  KeyPair kp; randombytes_buf(kp.priv.data(), kKeyLen);
  crypto_scalarmult_curve25519_base(kp.pub.data(), kp.priv.data());
  return kp;
}
std::string b64(const uint8_t* p, size_t n) {
  std::string out(sodium_base64_ENCODED_LEN(n, sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(out.data(), out.size(), p, n, sodium_base64_VARIANT_ORIGINAL);
  out.resize(std::strlen(out.c_str()));
  return out;
}
bool from_b64(const std::string& s, uint8_t* out, size_t n) {
  size_t got = 0;
  if (sodium_base642bin(out, n, s.data(), s.size(), " \t\r\n", &got, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) return false;
  return got == n;
}
bool parse_pub(const std::string& s, Key* out) { return from_b64(s, out->data(), kKeyLen); }
std::string KeyPair::pub_b64() const { return b64(pub.data(), kKeyLen); }
std::string KeyPair::priv_b64() const { return b64(priv.data(), kKeyLen); }
std::string fingerprint(const Key& pub) {
  uint8_t h[crypto_hash_sha256_BYTES]; crypto_hash_sha256(h, pub.data(), kKeyLen);
  std::string s = b64(h, sizeof h);
  while (!s.empty() && s.back() == '=') s.pop_back();
  return "SHA256:" + s;
}

bool load_or_create_keypair(const std::string& dir, const std::string& stem, KeyPair* kp, bool create) {
  ensure_sodium();
  std::string kf = dir + "/" + stem + ".key", pf = dir + "/" + stem + ".pub";
  if (util::file_exists(kf)) {
    std::string s = util::read_file(kf);
    if (!from_b64(s, kp->priv.data(), kKeyLen)) throw std::runtime_error("bad key file " + kf);
    crypto_scalarmult_curve25519_base(kp->pub.data(), kp->priv.data());
    if (!util::file_exists(pf)) util::write_file(pf, kp->pub_b64() + "\n");
    return false;
  }
  if (!create) throw std::runtime_error("no key at " + kf);
  util::mkdir_p(dir);
  ::chmod(dir.c_str(), 0700);
  *kp = KeyPair::generate();
  { std::ofstream f(kf, std::ios::binary | std::ios::trunc); if (!f) throw std::runtime_error("cannot write " + kf); f << kp->priv_b64() << "\n"; }
  ::chmod(kf.c_str(), 0600);
  util::write_file(pf, kp->pub_b64() + "\n");
  return true;
}

// ---- handshake -----------------------------------------------------------------------
struct Initiator::Impl { SymmetricState ss; KeyPair s, e; Key rs{}, re{}; bool sent = false; bool fixed_e = false; };
void Initiator::set_test_ephemeral(const KeyPair& e) { p_->e = e; p_->fixed_e = true; }
Initiator::Initiator(const KeyPair& local, const Key& remote_static) : p_(std::make_shared<Impl>()) {
  ensure_sodium();
  p_->s = local; p_->rs = remote_static;
  p_->ss.init();
  p_->ss.mix_hash(reinterpret_cast<const uint8_t*>(kPrologue), std::strlen(kPrologue));
  p_->ss.mix_hash(p_->rs.data(), kKeyLen);                    // pre-message <- s
}
std::string Initiator::write_message1() {
  auto& I = *p_;
  if (!I.fixed_e) I.e = KeyPair::generate();
  std::string msg(reinterpret_cast<const char*>(I.e.pub.data()), kKeyLen);  // e
  I.ss.mix_hash(I.e.pub.data(), kKeyLen);
  I.ss.mix_key(dh(I.e.priv, I.rs));                                           // es
  msg += I.ss.encrypt_and_hash(std::string(reinterpret_cast<const char*>(I.s.pub.data()), kKeyLen));  // s
  I.ss.mix_key(dh(I.s.priv, I.rs));                                           // ss
  msg += I.ss.encrypt_and_hash("");                                           // payload
  I.sent = true;
  return msg;
}
HandshakeResult Initiator::read_message2(const std::string& m) {
  auto& I = *p_;
  if (!I.sent) throw std::runtime_error("noise: message 2 before message 1");
  if (m.size() != kKeyLen + 16) throw std::runtime_error("noise: bad message 2 length");
  std::memcpy(I.re.data(), m.data(), kKeyLen);                                // e
  I.ss.mix_hash(I.re.data(), kKeyLen);
  I.ss.mix_key(dh(I.e.priv, I.re));                                           // ee
  I.ss.mix_key(dh(I.s.priv, I.re));                                           // se (initiator: DH(s, re))
  I.ss.decrypt_and_hash(m.substr(kKeyLen));                                   // payload
  HandshakeResult r; I.ss.split(&r.send, &r.recv); r.remote_static = I.rs; r.handshake_hash = I.ss.h;
  sodium_memzero(I.e.priv.data(), kKeyLen);
  return r;
}

struct Responder::Impl { SymmetricState ss; KeyPair s, e; Key rs{}, re{}; int stage = 0; bool fixed_e = false; };
void Responder::set_test_ephemeral(const KeyPair& e) { p_->e = e; p_->fixed_e = true; }
Responder::Responder(const KeyPair& local) : p_(std::make_shared<Impl>()) {
  ensure_sodium();
  p_->s = local;
  p_->ss.init();
  p_->ss.mix_hash(reinterpret_cast<const uint8_t*>(kPrologue), std::strlen(kPrologue));
  p_->ss.mix_hash(p_->s.pub.data(), kKeyLen);                                // pre-message <- s (our own)
}
Key Responder::read_message1(const std::string& m) {
  auto& R = *p_;
  if (m.size() != kKeyLen + (kKeyLen + 16) + 16) throw std::runtime_error("noise: bad message 1 length");
  std::memcpy(R.re.data(), m.data(), kKeyLen);                                // e
  R.ss.mix_hash(R.re.data(), kKeyLen);
  R.ss.mix_key(dh(R.s.priv, R.re));                                           // es (responder: DH(s, re))
  std::string spub = R.ss.decrypt_and_hash(m.substr(kKeyLen, kKeyLen + 16)); // s
  std::memcpy(R.rs.data(), spub.data(), kKeyLen);
  R.ss.mix_key(dh(R.s.priv, R.rs));                                           // ss
  R.ss.decrypt_and_hash(m.substr(2 * kKeyLen + 16));                          // payload
  R.stage = 1;
  return R.rs;
}
std::string Responder::write_message2() {
  auto& R = *p_;
  if (R.stage != 1) throw std::runtime_error("noise: message 2 before message 1");
  if (!R.fixed_e) R.e = KeyPair::generate();
  std::string msg(reinterpret_cast<const char*>(R.e.pub.data()), kKeyLen);  // e
  R.ss.mix_hash(R.e.pub.data(), kKeyLen);
  R.ss.mix_key(dh(R.e.priv, R.re));                                           // ee
  R.ss.mix_key(dh(R.e.priv, R.rs));                                           // se (responder: DH(e, rs))
  msg += R.ss.encrypt_and_hash("");
  R.stage = 2;
  return msg;
}
HandshakeResult Responder::finish() {
  auto& R = *p_;
  if (R.stage != 2) throw std::runtime_error("noise: finish before message 2");
  HandshakeResult r; R.ss.split(&r.recv, &r.send);   // c1 = initiator->responder = our recv
  r.remote_static = R.rs; r.handshake_hash = R.ss.h;
  sodium_memzero(R.e.priv.data(), kKeyLen);
  return r;
}

// ---- encrypted channel --------------------------------------------------------------
namespace {
class NoiseChannel : public net::Channel {
 public:
  NoiseChannel(CipherState tx, CipherState rx, const Key& peer) : tx_(tx), rx_(rx), peer_(peer) {}
  void send(const net::Socket& s, const char* p, size_t n) override {
    while (n) {
      size_t take = std::min(n, kMaxRecordPlain);
      std::string ct = tx_.encrypt_with_ad("", std::string(p, take));
      std::string rec; put_u16(rec, ct.size()); rec += ct;
      s.send_all(rec.data(), rec.size());
      p += take; n -= take;
      tx_.records++; tx_.bytes += take;
      if (tx_.records >= kRekeyRecords || tx_.bytes >= kRekeyBytes) tx_.rekey();
    }
  }
  void recv(const net::Socket& s, char* p, size_t n) override {
    while (n) {
      if (buf_pos_ == buf_.size()) {
        char h[2]; s.recv_all(h, 2);
        size_t len = get_u16(h);
        if (len < 16) throw std::runtime_error("noise: bad record length");
        std::string ct(len, '\0'); s.recv_all(ct.data(), len);
        buf_ = rx_.decrypt_with_ad("", ct); buf_pos_ = 0;
        rx_.records++; rx_.bytes += buf_.size();
        if (rx_.records >= kRekeyRecords || rx_.bytes >= kRekeyBytes) rx_.rekey();
      }
      size_t take = std::min(n, buf_.size() - buf_pos_);
      std::memcpy(p, buf_.data() + buf_pos_, take); buf_pos_ += take; p += take; n -= take;
    }
  }
  bool encrypted() const override { return true; }
  std::string peer_key_b64() const override { return b64(peer_.data(), kKeyLen); }
 private:
  CipherState tx_, rx_; Key peer_;
  std::string buf_; size_t buf_pos_ = 0;
};

void send_hs(const net::Socket& s, const std::string& m) { std::string f; put_u16(f, m.size()); f += m; s.send_all(f.data(), f.size()); }
std::string recv_hs(const net::Socket& s, const std::string& prefix) {
  char h[2]; size_t have = std::min<size_t>(prefix.size(), 2);
  std::memcpy(h, prefix.data(), have);
  if (have < 2) s.recv_all(h + have, 2 - have);
  size_t len = get_u16(h);
  if (len == 0 || len > 65535) throw std::runtime_error("noise: bad handshake length");
  std::string m(len, '\0');
  size_t pre = prefix.size() > 2 ? prefix.size() - 2 : 0;
  if (pre) std::memcpy(m.data(), prefix.data() + 2, std::min(pre, len));
  if (len > pre) s.recv_all(m.data() + pre, len - pre);
  return m;
}
}  // namespace

std::unique_ptr<net::Channel> initiate(const net::Socket& s, const KeyPair& local, const Key& remote_static) {
  Initiator init(local, remote_static);
  send_hs(s, init.write_message1());
  HandshakeResult r = init.read_message2(recv_hs(s, ""));
  return std::make_unique<NoiseChannel>(r.send, r.recv, r.remote_static);
}
std::unique_ptr<net::Channel> respond(const net::Socket& s, const KeyPair& local, const std::string& prefix,
                                      const std::function<bool(const Key&)>& authorize, Key* initiator_key) {
  Responder resp(local);
  Key ik = resp.read_message1(recv_hs(s, prefix));
  if (initiator_key) *initiator_key = ik;
  if (!authorize(ik)) throw std::runtime_error("noise: client key not authorized: " + fingerprint(ik));
  send_hs(s, resp.write_message2());
  HandshakeResult r = resp.finish();
  return std::make_unique<NoiseChannel>(r.send, r.recv, r.remote_static);
}

double benchmark_bytes_per_s(size_t bytes) {
  ensure_sodium();
  CipherState a, b; Key k{}; randombytes_buf(k.data(), kKeyLen); a.init(k); b.init(k);
  std::string pt(kMaxRecordPlain, 'x');
  size_t done = 0; double t0 = util::now_s();
  while (done < bytes) {
    std::string ct = a.encrypt_with_ad("", pt);
    std::string back = b.decrypt_with_ad("", ct);
    done += back.size();
  }
  double dt = util::now_s() - t0;
  return dt > 0 ? static_cast<double>(done) / dt : 0.0;
}

// ---- plain-mode PSK -----------------------------------------------------------------
bool load_psk(const std::string& path, Key* out) {
  ensure_sodium();
  if (!util::file_exists(path)) return false;
  std::string s = util::read_file(path);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  if (s.size() < 16) throw std::runtime_error("psk file too short (need >= 16 bytes): " + path);
  crypto_hash_sha256(out->data(), reinterpret_cast<const unsigned char*>(s.data()), s.size());
  return true;
}
std::string psk_tag(const Key& psk, const std::string& challenge) {
  std::string tag(crypto_auth_BYTES, '\0');
  crypto_auth(reinterpret_cast<unsigned char*>(tag.data()), reinterpret_cast<const unsigned char*>(challenge.data()), challenge.size(), psk.data());
  return tag;
}
bool psk_verify(const Key& psk, const std::string& challenge, const std::string& tag) {
  if (tag.size() != crypto_auth_BYTES) return false;
  return crypto_auth_verify(reinterpret_cast<const unsigned char*>(tag.data()), reinterpret_cast<const unsigned char*>(challenge.data()), challenge.size(), psk.data()) == 0;
}

// ---- policy --------------------------------------------------------------------------
SecMode parse_sec_mode(const std::string& s) {
  if (s == "noise") return SecMode::Noise;
  if (s == "plain") return SecMode::Plain;
  return SecMode::Auto;
}
SecDecision decide(SecMode mode, bool key_known, bool spec_plain, bool tofu, bool addr_private) {
  SecDecision d;
  auto want_plain = mode == SecMode::Plain || (spec_plain && mode != SecMode::Noise);
  if (want_plain) {
    if (addr_private) { d.kind = SecDecision::Plain; d.reason = "plain: private address and explicit opt-in"; }
    else { d.kind = SecDecision::Refuse; d.reason = "refusing plaintext to public address (set up keys or use a private/VPN address)"; }
    return d;
  }
  if (key_known) { d.kind = SecDecision::Noise; d.reason = "noise: worker key known"; return d; }
  if (tofu) { d.kind = SecDecision::Tofu; d.reason = "noise: key unknown, trust-on-first-use enabled"; return d; }
  d.kind = SecDecision::Refuse;
  d.reason = "no key known for this worker: add #<pubkey> to its OPTICC_HOSTS entry or a line to known_workers, "
             "set OPTICC_TOFU=1 to record it on first contact, or OPTICC_SECURITY=plain for a LAN worker started with --accept plain";
  return d;
}

}  // namespace opticc::noise
