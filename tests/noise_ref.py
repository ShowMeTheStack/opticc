#!/usr/bin/env python3
"""Independent reference of Noise_IK_25519_ChaChaPoly_BLAKE2b as opticc uses it (prologue "opticc-noise-v1",
HASHLEN 64, HKDF = HMAC-BLAKE2b per Noise rev 34 §4.3, 96-bit nonce = 4 zero bytes || LE64 counter,
AD = h for handshake payloads, Split -> (c1 initiator->responder, c2 responder->initiator)).
Written from the Noise specification only (not from the C++), using the `cryptography` package for X25519
and ChaCha20-Poly1305 and hashlib for BLAKE2b. Prints known-answer vectors as JSON for tests/test_noise.cpp.

Usage: noise_ref.py [--selftest]
"""
import hashlib, hmac, json, struct, sys
from cryptography.hazmat.primitives.asymmetric import x25519
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives import serialization

PROTOCOL = b"Noise_IK_25519_ChaChaPoly_BLAKE2b"
PROLOGUE = b"opticc-noise-v1"
HASHLEN = 64

def H(*parts):
    h = hashlib.blake2b(digest_size=HASHLEN)
    for p in parts: h.update(p)
    return h.digest()
def HMAC(key, data): return hmac.new(key, data, lambda: hashlib.blake2b(digest_size=HASHLEN)).digest()   # blocklen 128 handled by hmac
def HKDF2(ck, ikm):
    temp = HMAC(ck, ikm); o1 = HMAC(temp, b"\x01"); o2 = HMAC(temp, o1 + b"\x02"); return o1, o2
def nonce(n): return b"\x00" * 4 + struct.pack("<Q", n)
def dh(priv, pub): return x25519.X25519PrivateKey.from_private_bytes(priv).exchange(x25519.X25519PublicKey.from_public_bytes(pub))
def pubof(priv): return x25519.X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)

class Cipher:
    def __init__(self): self.k = None; self.n = 0
    def init(self, k): self.k = k; self.n = 0
    def enc(self, ad, pt):
        if self.k is None: return pt
        ct = ChaCha20Poly1305(self.k).encrypt(nonce(self.n), pt, ad); self.n += 1; return ct
    def dec(self, ad, ct):
        if self.k is None: return ct
        pt = ChaCha20Poly1305(self.k).decrypt(nonce(self.n), ct, ad); self.n += 1; return pt

class Sym:
    def __init__(self):
        self.h = PROTOCOL + b"\x00" * (HASHLEN - len(PROTOCOL)) if len(PROTOCOL) <= HASHLEN else H(PROTOCOL)
        self.ck = self.h; self.cs = Cipher()
    def mix_hash(self, d): self.h = H(self.h, d)
    def mix_key(self, ikm):
        self.ck, tk = HKDF2(self.ck, ikm); self.cs.init(tk[:32])
    def enc_hash(self, pt): ct = self.cs.enc(self.h, pt); self.mix_hash(ct); return ct
    def dec_hash(self, ct): pt = self.cs.dec(self.h, ct); self.mix_hash(ct); return pt
    def split(self):
        t1, t2 = HKDF2(self.ck, b""); c1, c2 = Cipher(), Cipher(); c1.init(t1[:32]); c2.init(t2[:32]); return c1, c2

def handshake(i_s, i_e, r_s, r_e):
    """IK: pre-message <- s; -> e, es, s, ss ; <- e, ee, se.  Returns msg1, msg2, (i_send, i_recv), (r_send, r_recv)."""
    I = Sym(); I.mix_hash(PROLOGUE); I.mix_hash(pubof(r_s))
    R = Sym(); R.mix_hash(PROLOGUE); R.mix_hash(pubof(r_s))
    # -> e, es, s, ss
    msg1 = pubof(i_e); I.mix_hash(pubof(i_e)); I.mix_key(dh(i_e, pubof(r_s)))
    msg1 += I.enc_hash(pubof(i_s)); I.mix_key(dh(i_s, pubof(r_s))); msg1 += I.enc_hash(b"")
    # responder reads
    re = msg1[:32]; R.mix_hash(re); R.mix_key(dh(r_s, re))
    rs = R.dec_hash(msg1[32:80]); assert rs == pubof(i_s); R.mix_key(dh(r_s, rs)); R.dec_hash(msg1[80:])
    # <- e, ee, se
    msg2 = pubof(r_e); R.mix_hash(pubof(r_e)); R.mix_key(dh(r_e, re)); R.mix_key(dh(r_e, rs)); msg2 += R.enc_hash(b"")
    # initiator reads
    I.mix_hash(msg2[:32]); I.mix_key(dh(i_e, msg2[:32])); I.mix_key(dh(i_s, msg2[:32])); I.dec_hash(msg2[32:])
    assert I.h == R.h
    i1, i2 = I.split(); r1, r2 = R.split()
    return msg1, msg2, (i1, i2), (r1, r2), I.h

def main():
    i_s = bytes(range(1, 33)); i_e = bytes(range(0x41, 0x61)); r_s = bytes(range(0x81, 0xa1)); r_e = bytes(range(0xc1, 0xe1))
    # clamp as X25519 private keys are used raw by libsodium/cryptography (both clamp internally) -> same scalar
    msg1, msg2, (i_send, i_recv), (r_recv, r_send), hh = handshake(i_s, i_e, r_s, r_e)
    t1 = i_send.enc(b"", b"hello from initiator"); t2 = r_send.enc(b"", b"hello from responder")
    assert r_recv.dec(b"", t1) == b"hello from initiator" and i_recv.dec(b"", t2) == b"hello from responder"
    vec = {"i_s": i_s.hex(), "i_e": i_e.hex(), "r_s": r_s.hex(), "r_e": r_e.hex(), "i_s_pub": pubof(i_s).hex(), "r_s_pub": pubof(r_s).hex(),
           "msg1": msg1.hex(), "msg2": msg2.hex(), "handshake_hash": hh.hex(), "t_i2r": t1.hex(), "t_r2i": t2.hex()}
    print(json.dumps(vec, indent=1))

if __name__ == "__main__":
    main()
