#include "SerumSigning.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// The Ed25519 arithmetic follows TweetNaCl (public domain, by Bernstein,
// van Gastel, Janssen, Lange, Schwabe and Smetsers): small and slow, which is
// fine for one check per load.

namespace serum_signing {

namespace {

// ---- SHA-512

constexpr uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
    0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
    0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
    0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
    0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
    0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
    0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
    0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
    0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
    0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
    0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

inline uint64_t Rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

// ---- Field arithmetic modulo 2^255 - 19, 16 limbs of 16 bits.

using gf = int64_t[16];

const gf gf0 = {0};
const gf gf1 = {1};
const gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
              0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
               0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const gf X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
              0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const gf Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
              0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const gf I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
              0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};

void Set(gf r, const gf a) {
  for (int i = 0; i < 16; ++i) r[i] = a[i];
}

void Carry(gf o) {
  for (int i = 0; i < 16; ++i) {
    o[i] += (int64_t)1 << 16;
    const int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c * 65536;
  }
}

void Select(gf p, gf q, int b) {
  const int64_t c = ~(int64_t)(b - 1);
  for (int i = 0; i < 16; ++i) {
    const int64_t t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

void Pack25519(uint8_t* o, const gf n) {
  gf m, t;
  for (int i = 0; i < 16; ++i) t[i] = n[i];
  Carry(t);
  Carry(t);
  Carry(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    const int b = (int)((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    Select(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = (uint8_t)(t[i] & 0xff);
    o[2 * i + 1] = (uint8_t)(t[i] >> 8);
  }
}

bool Equal32(const uint8_t* a, const uint8_t* b) {
  unsigned d = 0;
  for (int i = 0; i < 32; ++i) d |= a[i] ^ b[i];
  return d == 0;
}

bool NotEqual(const gf a, const gf b) {
  uint8_t c[32], d[32];
  Pack25519(c, a);
  Pack25519(d, b);
  return !Equal32(c, d);
}

uint8_t Parity(const gf a) {
  uint8_t d[32];
  Pack25519(d, a);
  return d[0] & 1;
}

void Unpack25519(gf o, const uint8_t* n) {
  for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}

void Add(gf o, const gf a, const gf b) {
  for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

void Sub(gf o, const gf a, const gf b) {
  for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

void Mul(gf o, const gf a, const gf b) {
  int64_t t[31] = {0};
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; ++i) o[i] = t[i];
  Carry(o);
  Carry(o);
}

void Square(gf o, const gf a) { Mul(o, a, a); }

void Invert(gf o, const gf in) {
  gf c;
  Set(c, in);
  for (int a = 253; a >= 0; --a) {
    Square(c, c);
    if (a != 2 && a != 4) Mul(c, c, in);
  }
  Set(o, c);
}

void Pow2523(gf o, const gf in) {
  gf c;
  Set(c, in);
  for (int a = 250; a >= 0; --a) {
    Square(c, c);
    if (a != 1) Mul(c, c, in);
  }
  Set(o, c);
}

// ---- Points in extended coordinates.

void PointAdd(gf p[4], gf q[4]) {
  gf a, b, c, d, t, e, f, g, h;
  Sub(a, p[1], p[0]);
  Sub(t, q[1], q[0]);
  Mul(a, a, t);
  Add(b, p[0], p[1]);
  Add(t, q[0], q[1]);
  Mul(b, b, t);
  Mul(c, p[3], q[3]);
  Mul(c, c, D2);
  Mul(d, p[2], q[2]);
  Add(d, d, d);
  Sub(e, b, a);
  Sub(f, d, c);
  Add(g, d, c);
  Add(h, b, a);
  Mul(p[0], e, f);
  Mul(p[1], h, g);
  Mul(p[2], g, f);
  Mul(p[3], e, h);
}

void PointSwap(gf p[4], gf q[4], uint8_t b) {
  for (int i = 0; i < 4; ++i) Select(p[i], q[i], b);
}

void PointPack(uint8_t* r, gf p[4]) {
  gf tx, ty, zi;
  Invert(zi, p[2]);
  Mul(tx, p[0], zi);
  Mul(ty, p[1], zi);
  Pack25519(r, ty);
  r[31] ^= Parity(tx) << 7;
}

void ScalarMult(gf p[4], gf q[4], const uint8_t* s) {
  Set(p[0], gf0);
  Set(p[1], gf1);
  Set(p[2], gf1);
  Set(p[3], gf0);
  for (int i = 255; i >= 0; --i) {
    const uint8_t b = (s[i / 8] >> (i & 7)) & 1;
    PointSwap(p, q, b);
    PointAdd(q, p);
    PointAdd(p, p);
    PointSwap(p, q, b);
  }
}

void ScalarBase(gf p[4], const uint8_t* s) {
  gf q[4];
  Set(q[0], X);
  Set(q[1], Y);
  Set(q[2], gf1);
  Mul(q[3], X, Y);
  ScalarMult(p, q, s);
}

// The negated point of an encoded public key; false for bytes that are none.
bool UnpackNegative(gf r[4], const uint8_t p[32]) {
  gf t, chk, num, den, den2, den4, den6;
  Set(r[2], gf1);
  Unpack25519(r[1], p);
  Square(num, r[1]);
  Mul(den, num, D);
  Sub(num, num, r[2]);
  Add(den, r[2], den);
  Square(den2, den);
  Square(den4, den2);
  Mul(den6, den4, den2);
  Mul(t, den6, num);
  Mul(t, t, den);
  Pow2523(t, t);
  Mul(t, t, num);
  Mul(t, t, den);
  Mul(t, t, den);
  Mul(r[0], t, den);
  Square(chk, r[0]);
  Mul(chk, chk, den);
  if (NotEqual(chk, num)) Mul(r[0], r[0], I);
  Square(chk, r[0]);
  Mul(chk, chk, den);
  if (NotEqual(chk, num)) return false;
  if (Parity(r[0]) == (p[31] >> 7)) Sub(r[0], gf0, r[0]);
  Mul(r[3], r[0], r[1]);
  return true;
}

// ---- Scalars modulo the group order L.

const int64_t L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                       0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                       0,    0,    0,    0,    0,    0,    0,    0,
                       0,    0,    0,    0,    0,    0,    0,    0x10};

void ModL(uint8_t* r, int64_t x[64]) {
  int64_t carry;
  for (int i = 63; i >= 32; --i) {
    carry = 0;
    int j;
    for (j = i - 32; j < i - 12; ++j) {
      x[j] += carry - 16 * x[i] * L[j - (i - 32)];
      carry = (x[j] + 128) >> 8;
      x[j] -= carry * 256;
    }
    x[j] += carry;
    x[i] = 0;
  }
  carry = 0;
  for (int j = 0; j < 32; ++j) {
    x[j] += carry - (x[31] >> 4) * L[j];
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  for (int j = 0; j < 32; ++j) x[j] -= carry * L[j];
  for (int i = 0; i < 32; ++i) {
    x[i + 1] += x[i] >> 8;
    r[i] = (uint8_t)(x[i] & 255);
  }
}

void Reduce(uint8_t* r) {
  int64_t x[64];
  for (int i = 0; i < 64; ++i) x[i] = r[i];
  for (int i = 0; i < 64; ++i) r[i] = 0;
  ModL(r, x);
}

// Whether a scalar is below L, so that a signature has one encoding only.
bool ScalarIsCanonical(const uint8_t* s) {
  for (int i = 31; i >= 0; --i) {
    if (s[i] < L[i]) return true;
    if (s[i] > L[i]) return false;
  }
  return false;
}

void ExpandSeed(const Seed& seed, uint8_t d[64]) {
  const Digest h = Sha512Of(seed.data(), seed.size());
  memcpy(d, h.data(), 64);
  d[0] &= 248;
  d[31] &= 127;
  d[31] |= 64;
}

// ---- Bytes

void PutU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back((uint8_t)v);
  out.push_back((uint8_t)(v >> 8));
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i)));
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(v >> (8 * i)));
}

uint64_t GetLE(const uint8_t* p, int bytes) {
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

constexpr uint32_t kCertificateMagic = 0x54524353;  // "SCRT"
constexpr uint8_t kCertificateVersion = 1;
constexpr uint16_t kTrailerVersion = 1;
constexpr char kSignedMessagePrefix[] = "SERUM-CROMC-1";
constexpr char kChainTextPrefix[] = "SERUM-CERTIFICATE-1:";
constexpr size_t kMaxChain = 4;

std::vector<uint8_t> SignedMessage(const Digest& contentDigest) {
  std::vector<uint8_t> message(
      kSignedMessagePrefix,
      kSignedMessagePrefix + sizeof(kSignedMessagePrefix) - 1);
  message.insert(message.end(), contentDigest.begin(), contentDigest.end());
  return message;
}

// The root keys of PPUC colorization signing. The secret halves are kept
// offline by the project's trusted members. A key is only added here, never
// replaced: a new root key goes in beside the old one, so that files signed
// under the old one keep their signature.
const std::vector<PublicKey> kBuiltInRootKeys = {
    // 2026-11-05
    PublicKey{
        0x48, 0x90, 0x82, 0xd3, 0xb8, 0x56, 0xae, 0xbf, 0x31, 0x4d, 0x3c,
        0x30, 0xb4, 0x6c, 0x1a, 0xa2, 0x01, 0xde, 0xcd, 0x9b, 0xf1, 0x8f,
        0xfb, 0x6f, 0x57, 0x83, 0x0a, 0xe7, 0x14, 0xce, 0x0e, 0xaf,
    },
};

std::vector<PublicKey>& TestRootKeys() {
  static std::vector<PublicKey> keys;
  return keys;
}

}  // namespace

// ---- SHA-512

Sha512::Sha512() {
  static const uint64_t kInit[8] = {
      0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
      0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
      0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
  memcpy(m_state, kInit, sizeof(m_state));
}

void Sha512::block(const uint8_t* data) {
  uint64_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = 0;
    for (int j = 0; j < 8; ++j) w[i] = (w[i] << 8) | data[i * 8 + j];
  }
  for (int i = 16; i < 80; ++i) {
    const uint64_t s0 =
        Rotr(w[i - 15], 1) ^ Rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
    const uint64_t s1 =
        Rotr(w[i - 2], 19) ^ Rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3],
           e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
  for (int i = 0; i < 80; ++i) {
    const uint64_t s1 = Rotr(e, 14) ^ Rotr(e, 18) ^ Rotr(e, 41);
    const uint64_t ch = (e & f) ^ (~e & g);
    const uint64_t t1 = h + s1 + ch + kSha512K[i] + w[i];
    const uint64_t s0 = Rotr(a, 28) ^ Rotr(a, 34) ^ Rotr(a, 39);
    const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint64_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  m_state[0] += a;
  m_state[1] += b;
  m_state[2] += c;
  m_state[3] += d;
  m_state[4] += e;
  m_state[5] += f;
  m_state[6] += g;
  m_state[7] += h;
}

void Sha512::update(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  m_length += size;
  if (m_buffered > 0) {
    const size_t take = std::min(size, sizeof(m_buffer) - m_buffered);
    memcpy(m_buffer + m_buffered, p, take);
    m_buffered += take;
    p += take;
    size -= take;
    if (m_buffered < sizeof(m_buffer)) return;
    block(m_buffer);
    m_buffered = 0;
  }
  while (size >= sizeof(m_buffer)) {
    block(p);
    p += sizeof(m_buffer);
    size -= sizeof(m_buffer);
  }
  memcpy(m_buffer, p, size);
  m_buffered = size;
}

Digest Sha512::finish() {
  const uint64_t bits = m_length * 8;
  m_buffer[m_buffered++] = 0x80;
  if (m_buffered > 112) {
    memset(m_buffer + m_buffered, 0, sizeof(m_buffer) - m_buffered);
    block(m_buffer);
    m_buffered = 0;
  }
  memset(m_buffer + m_buffered, 0, 120 - m_buffered);
  for (int i = 0; i < 8; ++i)
    m_buffer[120 + i] = (uint8_t)(bits >> (56 - 8 * i));
  block(m_buffer);
  Digest out;
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 8; ++j)
      out[i * 8 + j] = (uint8_t)(m_state[i] >> (56 - 8 * j));
  return out;
}

Digest Sha512Of(const void* data, size_t size) {
  Sha512 sha;
  sha.update(data, size);
  return sha.finish();
}

// ---- Ed25519

PublicKey PublicKeyFromSeed(const Seed& seed) {
  uint8_t d[64];
  ExpandSeed(seed, d);
  gf p[4];
  ScalarBase(p, d);
  PublicKey key;
  PointPack(key.data(), p);
  return key;
}

Signature Sign(const Seed& seed, const void* message, size_t size) {
  uint8_t d[64];
  ExpandSeed(seed, d);
  const PublicKey key = PublicKeyFromSeed(seed);

  Sha512 nonceHash;
  nonceHash.update(d + 32, 32);
  nonceHash.update(message, size);
  Digest r = nonceHash.finish();
  Reduce(r.data());
  gf p[4];
  ScalarBase(p, r.data());
  Signature signature;
  PointPack(signature.data(), p);

  Sha512 challengeHash;
  challengeHash.update(signature.data(), 32);
  challengeHash.update(key.data(), 32);
  challengeHash.update(message, size);
  Digest h = challengeHash.finish();
  Reduce(h.data());

  int64_t x[64] = {0};
  for (int i = 0; i < 32; ++i) x[i] = r[i];
  for (int i = 0; i < 32; ++i)
    for (int j = 0; j < 32; ++j) x[i + j] += (int64_t)h[i] * d[j];
  ModL(signature.data() + 32, x);
  return signature;
}

bool Verify(const PublicKey& key, const void* message, size_t size,
            const Signature& signature) {
  if (!ScalarIsCanonical(signature.data() + 32)) return false;
  gf p[4], q[4];
  if (!UnpackNegative(q, key.data())) return false;
  Sha512 challengeHash;
  challengeHash.update(signature.data(), 32);
  challengeHash.update(key.data(), 32);
  challengeHash.update(message, size);
  Digest h = challengeHash.finish();
  Reduce(h.data());
  ScalarMult(p, q, h.data());
  ScalarBase(q, signature.data() + 32);
  PointAdd(p, q);
  uint8_t t[32];
  PointPack(t, p);
  return Equal32(signature.data(), t);
}

// ---- Certificates

std::vector<uint8_t> Certificate::signedBytes() const {
  std::vector<uint8_t> out;
  PutU32(out, kCertificateMagic);
  out.push_back(kCertificateVersion);
  out.push_back((uint8_t)role);
  PutU16(out, 0);
  out.insert(out.end(), key.begin(), key.end());
  PutU64(out, (uint64_t)issuedAt);
  PutU16(out, (uint16_t)identity.size());
  out.insert(out.end(), identity.begin(), identity.end());
  PutU16(out, (uint16_t)name.size());
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), signer.begin(), signer.end());
  return out;
}

std::vector<uint8_t> Certificate::serialize() const {
  std::vector<uint8_t> out = signedBytes();
  out.insert(out.end(), signature.begin(), signature.end());
  return out;
}

bool Certificate::parse(const uint8_t* data, size_t size, Certificate& out) {
  size_t offset = 0;
  auto take = [&](size_t n) -> const uint8_t* {
    if (n > size - offset) return nullptr;
    const uint8_t* p = data + offset;
    offset += n;
    return p;
  };
  const uint8_t* head = take(8);
  if (!head || GetLE(head, 4) != kCertificateMagic ||
      head[4] != kCertificateVersion)
    return false;
  if (head[5] != (uint8_t)CertificateRole::Issuer &&
      head[5] != (uint8_t)CertificateRole::Author)
    return false;
  out.role = (CertificateRole)head[5];
  const uint8_t* key = take(32);
  const uint8_t* issued = take(8);
  const uint8_t* identityLength = take(2);
  if (!key || !issued || !identityLength) return false;
  memcpy(out.key.data(), key, 32);
  out.issuedAt = (int64_t)GetLE(issued, 8);
  const uint8_t* identity = take((size_t)GetLE(identityLength, 2));
  if (!identity) return false;
  out.identity.assign((const char*)identity, (size_t)GetLE(identityLength, 2));
  const uint8_t* nameLength = take(2);
  if (!nameLength) return false;
  const uint8_t* name = take((size_t)GetLE(nameLength, 2));
  if (!name) return false;
  out.name.assign((const char*)name, (size_t)GetLE(nameLength, 2));
  const uint8_t* signer = take(32);
  const uint8_t* signature = take(64);
  if (!signer || !signature || offset != size) return false;
  memcpy(out.signer.data(), signer, 32);
  memcpy(out.signature.data(), signature, 64);
  return true;
}

Certificate Certify(CertificateRole role, const PublicKey& key,
                    int64_t issuedAt, const std::string& identity,
                    const std::string& name, const Seed& signerSeed) {
  Certificate certificate;
  certificate.role = role;
  certificate.key = key;
  certificate.issuedAt = issuedAt;
  certificate.identity = identity.substr(0, 0xffff);
  certificate.name = name.substr(0, 0xffff);
  certificate.signer = PublicKeyFromSeed(signerSeed);
  const std::vector<uint8_t> bytes = certificate.signedBytes();
  certificate.signature = Sign(signerSeed, bytes.data(), bytes.size());
  return certificate;
}

std::vector<uint8_t> SerializeChain(const std::vector<Certificate>& chain) {
  std::vector<uint8_t> out;
  PutU16(out, (uint16_t)chain.size());
  for (const Certificate& certificate : chain) {
    const std::vector<uint8_t> bytes = certificate.serialize();
    PutU32(out, (uint32_t)bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
  }
  return out;
}

bool ParseChain(const uint8_t* data, size_t size,
                std::vector<Certificate>& out) {
  out.clear();
  if (size < 2) return false;
  const size_t count = (size_t)GetLE(data, 2);
  if (count == 0 || count > kMaxChain) return false;
  size_t offset = 2;
  for (size_t i = 0; i < count; ++i) {
    if (size - offset < 4) return false;
    const size_t length = (size_t)GetLE(data + offset, 4);
    offset += 4;
    if (length > size - offset) return false;
    Certificate certificate;
    if (!Certificate::parse(data + offset, length, certificate)) return false;
    offset += length;
    out.push_back(std::move(certificate));
  }
  return offset == size;
}

namespace {
constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string ChainToText(const std::vector<uint8_t>& chainBytes) {
  std::string out = kChainTextPrefix;
  size_t i = 0;
  for (; i + 2 < chainBytes.size(); i += 3) {
    const uint32_t v =
        (chainBytes[i] << 16) | (chainBytes[i + 1] << 8) | chainBytes[i + 2];
    for (int k = 3; k >= 0; --k) out += kBase64[(v >> (6 * k)) & 63];
  }
  if (i < chainBytes.size()) {
    const bool two = i + 1 < chainBytes.size();
    const uint32_t v =
        (chainBytes[i] << 16) | (two ? chainBytes[i + 1] << 8 : 0);
    out += kBase64[(v >> 18) & 63];
    out += kBase64[(v >> 12) & 63];
    out += two ? kBase64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

bool ChainFromText(const std::string& text, std::vector<uint8_t>& chainBytes) {
  chainBytes.clear();
  const size_t start = text.find(kChainTextPrefix);
  if (start == std::string::npos) return false;
  uint32_t value = 0;
  int bits = 0;
  for (size_t i = start + sizeof(kChainTextPrefix) - 1; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '=') break;
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
    const char* p = strchr(kBase64, c);
    if (!p || c == '\0') return false;
    value = (value << 6) | (uint32_t)(p - kBase64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      chainBytes.push_back((uint8_t)(value >> bits));
      value &= (1u << bits) - 1;
    }
  }
  std::vector<Certificate> chain;
  return ParseChain(chainBytes.data(), chainBytes.size(), chain);
}

const std::vector<PublicKey>& RootPublicKeys() {
  if (!TestRootKeys().empty()) return TestRootKeys();
  return kBuiltInRootKeys;
}

const std::vector<PublicKey>& RevokedKeys() {
  // Keys that signed what they should not have, or got out. A colorization
  // signed with one of them shows as modified.
  static const std::vector<PublicKey> keys;
  return keys;
}

void SetRootPublicKeysForTesting(const std::vector<PublicKey>& keys) {
  TestRootKeys() = keys;
}

namespace {

bool Contains(const std::vector<PublicKey>& keys, const PublicKey& key) {
  for (const PublicKey& k : keys)
    if (k == key) return true;
  return false;
}

bool SignatureValid(const Certificate& certificate) {
  const std::vector<uint8_t> bytes = certificate.signedBytes();
  return Verify(certificate.signer, bytes.data(), bytes.size(),
                certificate.signature);
}

}  // namespace

bool CheckChain(const std::vector<Certificate>& chain, std::string& reason) {
  if (chain.empty()) {
    reason = "no certificate";
    return false;
  }
  if (chain.front().role != CertificateRole::Author) {
    reason = "the first certificate is not an author's";
    return false;
  }
  for (size_t i = 0; i < chain.size(); ++i) {
    const Certificate& certificate = chain[i];
    if (Contains(RevokedKeys(), certificate.key) ||
        Contains(RevokedKeys(), certificate.signer)) {
      reason = "a key of the certificate of " + certificate.name +
               " has been revoked";
      return false;
    }
    if (!SignatureValid(certificate)) {
      reason = "the certificate of " + certificate.name + " is not valid";
      return false;
    }
    if (Contains(RootPublicKeys(), certificate.signer)) {
      if (i + 1 != chain.size()) {
        reason = "certificates follow one a root key signed";
        return false;
      }
      return true;
    }
    if (i + 1 == chain.size()) {
      reason = "no trusted key signed the certificate of " + certificate.name;
      return false;
    }
    const Certificate& next = chain[i + 1];
    if (next.role != CertificateRole::Issuer ||
        next.key != certificate.signer) {
      reason = "the certificate of " + certificate.name +
               " was not signed by the issuer that follows it";
      return false;
    }
  }
  reason = "no trusted key signed the certificates";
  return false;
}

// ---- The signature block

std::vector<uint8_t> BuildTrailer(const Digest& contentDigest,
                                  const std::vector<uint8_t>& chainBytes,
                                  const Seed& authorSeed) {
  std::vector<uint8_t> out;
  PutU32(out, kTrailerMagic);
  PutU16(out, kTrailerVersion);
  PutU16(out, 0);
  PutU32(out, (uint32_t)chainBytes.size());
  out.insert(out.end(), chainBytes.begin(), chainBytes.end());
  const std::vector<uint8_t> message = SignedMessage(contentDigest);
  const Signature signature = Sign(authorSeed, message.data(), message.size());
  out.insert(out.end(), signature.begin(), signature.end());
  PutU32(out, (uint32_t)(out.size() + kTrailerTailBytes));
  PutU32(out, kTrailerMagic);
  return out;
}

uint64_t TrailerBytes(FILE* fp, uint64_t size) {
  uint8_t tail[kTrailerTailBytes];
  if (size < kTrailerTailBytes ||
      fseek(fp, (long)(size - kTrailerTailBytes), SEEK_SET) != 0 ||
      fread(tail, 1, sizeof(tail), fp) != sizeof(tail) ||
      GetLE(tail + 4, 4) != kTrailerMagic)
    return 0;
  const uint64_t trailerSize = GetLE(tail, 4);
  return trailerSize <= size ? trailerSize : 0;
}

CheckResult CheckFile(const char* path) {
  CheckResult result;
  FILE* fp = fopen(path, "rb");
  if (!fp) {
    result.reason = "the file cannot be read";
    return result;
  }
  struct Closer {
    FILE* fp;
    ~Closer() { fclose(fp); }
  } closer{fp};
  if (fseek(fp, 0, SEEK_END) != 0) return result;
  const long sizeLong = ftell(fp);
  if (sizeLong < 0) return result;
  const uint64_t size = (uint64_t)sizeLong;
  result.contentBytes = size;
  if (size < kTrailerTailBytes) return result;
  uint8_t tail[kTrailerTailBytes];
  if (fseek(fp, (long)(size - kTrailerTailBytes), SEEK_SET) != 0 ||
      fread(tail, 1, sizeof(tail), fp) != sizeof(tail) ||
      GetLE(tail + 4, 4) != kTrailerMagic)
    return result;  // no signature
  const uint64_t trailerSize = GetLE(tail, 4);
  // From here on the file claims a signature: anything wrong with it means
  // that it is no longer the file that was signed.
  result.authenticity = Authenticity::Modified;
  if (trailerSize < 12 + 64 + kTrailerTailBytes || trailerSize > size ||
      trailerSize > (16u << 20)) {
    result.reason = "the signature block is damaged";
    return result;
  }
  std::vector<uint8_t> trailer((size_t)trailerSize);
  if (fseek(fp, (long)(size - trailerSize), SEEK_SET) != 0 ||
      fread(trailer.data(), 1, trailer.size(), fp) != trailer.size() ||
      GetLE(trailer.data(), 4) != kTrailerMagic ||
      GetLE(trailer.data() + 4, 2) != kTrailerVersion) {
    result.reason = "the signature block is damaged";
    return result;
  }
  const uint64_t chainSize = GetLE(trailer.data() + 8, 4);
  if (12 + chainSize + 64 + kTrailerTailBytes != trailerSize) {
    result.reason = "the signature block is damaged";
    return result;
  }
  std::vector<Certificate> chain;
  if (!ParseChain(trailer.data() + 12, (size_t)chainSize, chain)) {
    result.reason = "the certificates are damaged";
    return result;
  }
  result.identity = chain.front().identity;
  result.name = chain.front().name;
  result.contentBytes = size - trailerSize;
  Signature signature;
  memcpy(signature.data(), trailer.data() + 12 + chainSize, 64);

  if (fseek(fp, 0, SEEK_SET) != 0) {
    result.reason = "the file cannot be read";
    return result;
  }
  Sha512 sha;
  std::vector<uint8_t> buffer(1u << 16);
  uint64_t left = result.contentBytes;
  while (left > 0) {
    const size_t n = (size_t)std::min<uint64_t>(left, buffer.size());
    if (fread(buffer.data(), 1, n, fp) != n) {
      result.reason = "the file cannot be read";
      return result;
    }
    sha.update(buffer.data(), n);
    left -= n;
  }
  const std::vector<uint8_t> message = SignedMessage(sha.finish());

  std::string reason;
  if (!CheckChain(chain, reason)) {
    result.reason = reason;
    return result;
  }
  if (!Verify(chain.front().key, message.data(), message.size(), signature)) {
    result.reason = "the file was changed after it was signed";
    return result;
  }
  result.authenticity = Authenticity::Signed;
  return result;
}

}  // namespace serum_signing
