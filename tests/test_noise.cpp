// Unit tests: Noise IK channel, keys, PSK, security policy.
// SPDX-License-Identifier: GPL-2.0-only
#include <sodium.h>
#include <sys/stat.h>

#include <cstring>
#include <functional>
#include <string>

#include "opticc/noise.hpp"
#include "opticc/util.hpp"
#include "test_harness.hpp"

using namespace opticc;

static std::string hex2bin(const char* h) {
  std::string out; size_t n = std::strlen(h);
  for (size_t i = 0; i + 1 < n; i += 2) out.push_back(static_cast<char>(std::stoi(std::string(h + i, 2), nullptr, 16)));
  return out;
}
static std::string bin2hex(const std::string& b) {
  static const char* d = "0123456789abcdef"; std::string o;
  for (unsigned char c : b) { o.push_back(d[c >> 4]); o.push_back(d[c & 15]); }
  return o;
}

// RFC 7748 §6.1 X25519 test vector (Alice/Bob).
OPTICC_TEST(kat_x25519_rfc7748) {
  CHECK(sodium_init() >= 0);
  std::string a_priv = hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  std::string a_pub  = hex2bin("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  std::string b_pub  = hex2bin("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  std::string shared = hex2bin("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  unsigned char pub[32], sh[32];
  crypto_scalarmult_curve25519_base(pub, reinterpret_cast<const unsigned char*>(a_priv.data()));
  CHECK(std::string(reinterpret_cast<char*>(pub), 32) == a_pub);
  CHECK(crypto_scalarmult_curve25519(sh, reinterpret_cast<const unsigned char*>(a_priv.data()), reinterpret_cast<const unsigned char*>(b_pub.data())) == 0);
  CHECK(std::string(reinterpret_cast<char*>(sh), 32) == shared);
}

// RFC 8439 §2.8.2 AEAD_CHACHA20_POLY1305 test vector (tag and first ciphertext block).
OPTICC_TEST(kat_chacha20poly1305_rfc8439) {
  std::string key; for (int i = 0x80; i <= 0x9f; ++i) key.push_back(static_cast<char>(i));
  std::string nonce = hex2bin("070000004041424344454647");
  std::string aad = hex2bin("50515253c0c1c2c3c4c5c6c7");
  std::string pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
  std::string ct(pt.size() + 16, '\0'); unsigned long long clen = 0;
  crypto_aead_chacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(ct.data()), &clen,
      reinterpret_cast<const unsigned char*>(pt.data()), pt.size(), reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
      nullptr, reinterpret_cast<const unsigned char*>(nonce.data()), reinterpret_cast<const unsigned char*>(key.data()));
  CHECK(clen == pt.size() + 16);
  CHECK(bin2hex(ct.substr(0, 16)) == "d31a8d34648e60db7b86afbc53ef7ec2");
  CHECK(bin2hex(ct.substr(pt.size())) == "1ae10b594f09e26a7e902ecbd0600691");
}

OPTICC_TEST(noise_keys_base64_fingerprint) {
  noise::KeyPair kp = noise::KeyPair::generate();
  std::string b = kp.pub_b64();
  CHECK(b.size() == 44);
  noise::Key k; CHECK(noise::parse_pub(b, &k)); CHECK(k == kp.pub);
  CHECK(!noise::parse_pub("not base64!!", &k));
  CHECK(!noise::parse_pub(b.substr(0, 20), &k));          // wrong length
  std::string fp = noise::fingerprint(kp.pub);
  CHECK(fp.rfind("SHA256:", 0) == 0); CHECK(fp.size() == 7 + 43); CHECK(fp.back() != '=');
  // load_or_create: files, permissions, round trip
  std::string dir = util::make_temp_dir("opticc-keys-");
  noise::KeyPair a; bool created = noise::load_or_create_keypair(dir, "client", &a); CHECK(created);
  struct stat st{}; CHECK(::stat((dir + "/client.key").c_str(), &st) == 0); CHECK((st.st_mode & 0777) == 0600);
  noise::KeyPair a2; CHECK(!noise::load_or_create_keypair(dir, "client", &a2));
  CHECK(a2.pub == a.pub); CHECK(a2.priv == a.priv);
  CHECK(util::read_file(dir + "/client.pub") == a.pub_b64() + "\n");
  util::remove_tree(dir);
}

static bool throws(const std::function<void()>& f) { try { f(); return false; } catch (const std::exception&) { return true; } }

OPTICC_TEST(noise_handshake_transcript_and_tamper) {
  noise::KeyPair ci = noise::KeyPair::generate(), wk = noise::KeyPair::generate();
  noise::Initiator init(ci, wk.pub); noise::Responder resp(wk);
  std::string m1 = init.write_message1();
  CHECK(m1.size() == 32 + 48 + 16);
  noise::Key ik = resp.read_message1(m1); CHECK(ik == ci.pub);
  std::string m2 = resp.write_message2(); CHECK(m2.size() == 32 + 16);
  noise::HandshakeResult ri = init.read_message2(m2), rr = resp.finish();
  CHECK(ri.handshake_hash == rr.handshake_hash);
  CHECK(ri.remote_static == wk.pub); CHECK(rr.remote_static == ci.pub);
  CHECK(ri.send.k == rr.recv.k); CHECK(ri.recv.k == rr.send.k); CHECK(ri.send.k != ri.recv.k);
  // transport both ways, several records, then tamper
  std::string pt(70000, 'q'); for (size_t i = 0; i < pt.size(); i += 11) pt[i] = static_cast<char>('a' + i % 26);
  std::string c1 = ri.send.encrypt_with_ad("", pt); CHECK(rr.recv.decrypt_with_ad("", c1) == pt);
  std::string c2 = rr.send.encrypt_with_ad("", "pong"); CHECK(ri.recv.decrypt_with_ad("", c2) == "pong");
  std::string c3 = ri.send.encrypt_with_ad("", "third"); std::string bad = c3; bad[2] = static_cast<char>(bad[2] ^ 1);
  CHECK(throws([&] { noise::CipherState tmp = rr.recv; tmp.decrypt_with_ad("", bad); }));
  CHECK(rr.recv.decrypt_with_ad("", c3) == "third");
  // replay / reorder is rejected (nonce advanced)
  CHECK(throws([&] { rr.recv.decrypt_with_ad("", c3); }));
  // rekey on both ends keeps the channel usable; a stale key does not
  noise::CipherState s2 = ri.send, r2 = rr.recv; s2.rekey(); r2.rekey();
  std::string c4 = s2.encrypt_with_ad("", "after rekey"); CHECK(r2.decrypt_with_ad("", c4) == "after rekey");
  CHECK(throws([&] { noise::CipherState stale = rr.recv; stale.decrypt_with_ad("", c4); }));
  // every single-byte tamper of message 1 or 2 is rejected
  int rejected = 0;
  for (size_t i = 0; i < m1.size(); ++i) {
    std::string t = m1; t[i] = static_cast<char>(t[i] ^ 0x80);
    noise::Responder r3(wk); if (throws([&] { r3.read_message1(t); })) rejected++;
  }
  CHECK(rejected == static_cast<int>(m1.size()));
  rejected = 0;
  for (size_t i = 0; i < m2.size(); ++i) {
    noise::Initiator i4(ci, wk.pub); std::string m = i4.write_message1();
    noise::Responder r4(wk); r4.read_message1(m); std::string mm = r4.write_message2();
    mm[i] = static_cast<char>(mm[i] ^ 0x80);
    if (throws([&] { i4.read_message2(mm); })) rejected++;
  }
  CHECK(rejected == static_cast<int>(m2.size()));
  // wrong responder key (initiator talks to an impostor) fails
  noise::KeyPair imp = noise::KeyPair::generate();
  noise::Initiator i5(ci, wk.pub); std::string m5 = i5.write_message1();
  noise::Responder r5(imp); CHECK(throws([&] { r5.read_message1(m5); }));
}

OPTICC_TEST(noise_psk_challenge) {
  noise::Key k1{}, k2{}; randombytes_buf(k1.data(), 32); randombytes_buf(k2.data(), 32);
  std::string ch(32, '\0'); randombytes_buf(ch.data(), 32);
  std::string tag = noise::psk_tag(k1, ch);
  CHECK(tag.size() == 32);
  CHECK(noise::psk_verify(k1, ch, tag)); CHECK(!noise::psk_verify(k2, ch, tag)); CHECK(!noise::psk_verify(k1, ch + "x", tag));
  std::string dir = util::make_temp_dir("opticc-psk-");
  util::write_file(dir + "/psk", "correct horse battery staple\n");
  noise::Key kf{}; CHECK(noise::load_psk(dir + "/psk", &kf));
  noise::Key kf2{}; CHECK(noise::load_psk(dir + "/psk", &kf2)); CHECK(kf == kf2);
  util::write_file(dir + "/short", "tiny");
  CHECK(throws([&] { noise::Key x{}; noise::load_psk(dir + "/short", &x); }));
  CHECK(!noise::load_psk(dir + "/missing", &kf));
  util::remove_tree(dir);
}

OPTICC_TEST(security_policy_table) {
  using D = noise::SecDecision; using M = noise::SecMode;
  struct Row { M mode; bool key, spec_plain, tofu, priv; D::Kind want; };
  const Row rows[] = {
    {M::Auto,  true,  false, false, false, D::Noise},   // key known, public address -> noise
    {M::Auto,  true,  false, false, true,  D::Noise},   // key known, private -> still noise (plain needs opt-in)
    {M::Auto,  false, false, false, true,  D::Refuse},  // no key, no opt-in, no tofu -> refuse with remedy
    {M::Auto,  false, false, true,  false, D::Tofu},    // tofu allowed
    {M::Auto,  false, true,  false, true,  D::Plain},   // ",plain" suffix on a private address
    {M::Auto,  false, true,  false, false, D::Refuse},  // ",plain" to a public address -> refuse
    {M::Plain, true,  false, false, true,  D::Plain},   // OPTICC_SECURITY=plain, private
    {M::Plain, true,  false, false, false, D::Refuse},  // OPTICC_SECURITY=plain, public -> refuse
    {M::Noise, true,  true,  false, true,  D::Noise},   // noise forced beats the plain suffix
    {M::Noise, false, true,  false, true,  D::Refuse},  // noise forced, no key, no tofu
    {M::Noise, false, false, true,  true,  D::Tofu},
  };
  for (auto& r : rows) {
    D d = noise::decide(r.mode, r.key, r.spec_plain, r.tofu, r.priv);
    if (d.kind != r.want) std::printf("  policy row mode=%d key=%d plain=%d tofu=%d priv=%d -> %d (want %d): %s\n", int(r.mode), r.key, r.spec_plain, r.tofu, r.priv, int(d.kind), int(r.want), d.reason.c_str());
    CHECK(d.kind == r.want);
  }
  D ref = noise::decide(M::Plain, false, false, false, false);
  CHECK(ref.reason.find("refusing plaintext to public address") != std::string::npos);
  // address classification
  auto ip = [](int a, int b, int c, int d) { return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d); };
  CHECK(util::is_private_ipv4(ip(10, 1, 2, 3))); CHECK(util::is_private_ipv4(ip(172, 16, 0, 1))); CHECK(util::is_private_ipv4(ip(172, 31, 255, 255)));
  CHECK(!util::is_private_ipv4(ip(172, 32, 0, 1))); CHECK(util::is_private_ipv4(ip(192, 168, 1, 1))); CHECK(util::is_private_ipv4(ip(169, 254, 1, 1)));
  CHECK(util::is_private_ipv4(ip(100, 64, 0, 1))); CHECK(util::is_private_ipv4(ip(100, 127, 255, 255))); CHECK(!util::is_private_ipv4(ip(100, 128, 0, 1)));
  CHECK(util::is_private_ipv4(ip(127, 0, 0, 1))); CHECK(!util::is_private_ipv4(ip(8, 8, 8, 8))); CHECK(!util::is_private_ipv4(ip(64, 176, 96, 141)));
  CHECK(util::resolve_ipv4("127.0.0.1") == ip(127, 0, 0, 1));
  CHECK(util::resolve_ipv4("localhost") == ip(127, 0, 0, 1));
}

OPTICC_TEST(noise_benchmark_runs) {
  double bps = noise::benchmark_bytes_per_s(4u << 20);
  std::printf("  encrypt+decrypt throughput: %.0f MB/s\n", bps / 1e6);
  CHECK(bps > 10e6);
}

// Noise IK token order. Known-answer vectors from tests/noise_ref.py, an independent
// Python implementation written from the Noise specification (X25519/ChaCha20-Poly1305 from the
// `cryptography` package, BLAKE2b from hashlib, HMAC from hmac). Fixed static and ephemeral keys; the C++
// handshake must reproduce message 1, message 2, the handshake hash and the first transport record each way.
OPTICC_TEST(noise_ik_reference_vectors) {
  auto kp_from_priv = [](const char* hexpriv) {
    noise::KeyPair kp; std::string pr = hex2bin(hexpriv);
    std::memcpy(kp.priv.data(), pr.data(), 32);
    crypto_scalarmult_curve25519_base(kp.pub.data(), kp.priv.data());
    return kp;
  };
  noise::KeyPair i_s = kp_from_priv("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
  noise::KeyPair i_e = kp_from_priv("4142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60");
  noise::KeyPair r_s = kp_from_priv("8182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0");
  noise::KeyPair r_e = kp_from_priv("c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0");
  CHECK(bin2hex(std::string(reinterpret_cast<const char*>(i_s.pub.data()), 32)) == "07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c");
  CHECK(bin2hex(std::string(reinterpret_cast<const char*>(r_s.pub.data()), 32)) == "883186b800b41d5cf0429695da9b3cc4f328ebcd184a6e482fa578c103f06c77");
  noise::Initiator I(i_s, r_s.pub); I.set_test_ephemeral(i_e);
  noise::Responder R(r_s); R.set_test_ephemeral(r_e);
  std::string m1 = I.write_message1();
  CHECK(bin2hex(m1) == "64b101b1d0be5a8704bd078f9895001fc03e8e9f9522f188dd128d9846d48466ea46fec0bee3ce0e052ec0cfad8ea652235164c7c9730cd3671b131e60a74e0034c1c6d447df994370d7598a4bb793ea3ef3813d2d5564983849d4a617315e83");
  noise::Key rs = R.read_message1(m1);
  CHECK(rs == i_s.pub);
  std::string m2 = R.write_message2();
  CHECK(bin2hex(m2) == "3a553d74792d727efa9b9a4cde3da1ad93f1a2d0c09cb639b1a3c0fda14cbe2410b4bc38ea129458b4a6cf89a4bbb766");
  noise::HandshakeResult hi = I.read_message2(m2);
  noise::HandshakeResult hr = R.finish();
  std::string hh(reinterpret_cast<const char*>(hi.handshake_hash.data()), 64);
  CHECK(bin2hex(hh) == "f12a7dab7455cdf37715a85780e20b09388a7e686256582d20f8487281647820c67910328ae9a7ad79611240f381958329c18215b4078be8abf84f042b4b99e4");
  CHECK(hi.handshake_hash == hr.handshake_hash);
  std::string t1 = hi.send.encrypt_with_ad("", "hello from initiator");
  CHECK(bin2hex(t1) == "f67710be1306422a1cf634471bada1c68695534e88a8f4be41a220b9d968c43ad9c78f07");
  CHECK(hr.recv.decrypt_with_ad("", t1) == "hello from initiator");
  std::string t2 = hr.send.encrypt_with_ad("", "hello from responder");
  CHECK(bin2hex(t2) == "d7a325e62a03c6d3a737214b2337cee0d0d164095fb7418b600f29e9428004d91cdd2c24");
  CHECK(hi.recv.decrypt_with_ad("", t2) == "hello from responder");
}
