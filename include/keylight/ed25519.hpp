// Keylight Ed25519 — verify-only, header-only, zero external dependencies.
//
// Core algorithm adapted from TweetNaCl (tweetnacl.c, public domain) by
// Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange,
// Peter Schwabe, Sjaak Smetsers. Source: https://tweetnacl.cr.yp.to/
//
// Only the verification path (crypto_sign_open equivalent) is retained.
// Signing and key-generation are NOT included.
//
// SHA-512 is vendored inline (from the same TweetNaCl source).
// No dependency on sha256.hpp or any system crypto library.
//
// Public API (namespace keylight):
//   bool keylight::ed25519_verify(
//       const uint8_t* msg, size_t msg_len,
//       const std::array<uint8_t,64>& sig,
//       const std::array<uint8_t,32>& pubkey);
//
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace keylight {

// All internals are anonymous-namespace scoped (translation-unit private).
namespace {

using kl_u8  = uint8_t;
using kl_u64 = uint64_t;
using kl_i64 = int64_t;
// GF(2^255-19) element: 16 limbs of 16 bits each (radix 2^16 representation).
using kl_gf  = kl_i64[16];

// ── SHA-512 ───────────────────────────────────────────────────────────────────
// Adapted from TweetNaCl's crypto_hash.

static const kl_u64 kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static inline kl_u64 kl_rotr64(kl_u64 x, int n) {
    return (x >> n) | (x << (64 - n));
}

static void kl_sha512_block(kl_u64 h[8], const kl_u8 blk[128]) {
    kl_u64 w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = 0;
        for (int j = 0; j < 8; ++j) w[i] = (w[i] << 8) | blk[8*i + j];
    }
    for (int i = 16; i < 80; ++i) {
        kl_u64 s0 = kl_rotr64(w[i-15],1) ^ kl_rotr64(w[i-15],8) ^ (w[i-15]>>7);
        kl_u64 s1 = kl_rotr64(w[i-2],19) ^ kl_rotr64(w[i-2],61) ^ (w[i-2]>>6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    kl_u64 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 80; ++i) {
        kl_u64 S1  = kl_rotr64(e,14) ^ kl_rotr64(e,18) ^ kl_rotr64(e,41);
        kl_u64 ch  = (e & f) ^ (~e & g);
        kl_u64 t1  = hh + S1 + ch + kSha512K[i] + w[i];
        kl_u64 S0  = kl_rotr64(a,28) ^ kl_rotr64(a,34) ^ kl_rotr64(a,39);
        kl_u64 maj = (a & b) ^ (a & c) ^ (b & c);
        kl_u64 t2  = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
    h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

// Streaming SHA-512 context.
struct KlSha512Ctx {
    kl_u64 h[8];
    kl_u8  buf[128];
    kl_u64 total;   // bytes fed so far (not counting buf)
    int    used;
};

static void kl_sha512_init(KlSha512Ctx& ctx) {
    ctx.h[0] = 0x6a09e667f3bcc908ULL; ctx.h[1] = 0xbb67ae8584caa73bULL;
    ctx.h[2] = 0x3c6ef372fe94f82bULL; ctx.h[3] = 0xa54ff53a5f1d36f1ULL;
    ctx.h[4] = 0x510e527fade682d1ULL; ctx.h[5] = 0x9b05688c2b3e6c1fULL;
    ctx.h[6] = 0x1f83d9abfb41bd6bULL; ctx.h[7] = 0x5be0cd19137e2179ULL;
    ctx.total = 0; ctx.used = 0;
}

static void kl_sha512_update(KlSha512Ctx& ctx, const kl_u8* data, size_t len) {
    while (len > 0) {
        int space = 128 - ctx.used;
        int take  = (len < (size_t)space) ? (int)len : space;
        for (int i = 0; i < take; ++i) ctx.buf[ctx.used + i] = data[i];
        ctx.used += take; data += take; len -= take;
        if (ctx.used == 128) {
            kl_sha512_block(ctx.h, ctx.buf);
            ctx.total += 128; ctx.used = 0;
        }
    }
}

static void kl_sha512_final(KlSha512Ctx& ctx, kl_u8 out[64]) {
    kl_u64 bits = (ctx.total + (kl_u64)ctx.used) * 8;
    ctx.buf[ctx.used++] = 0x80;
    if (ctx.used > 112) {
        while (ctx.used < 128) ctx.buf[ctx.used++] = 0;
        kl_sha512_block(ctx.h, ctx.buf); ctx.used = 0;
    }
    while (ctx.used < 112) ctx.buf[ctx.used++] = 0;
    for (int i = 0; i < 8; ++i) ctx.buf[112 + i] = 0;
    for (int i = 0; i < 8; ++i) ctx.buf[120 + i] = static_cast<kl_u8>(bits >> (56 - 8*i));
    kl_sha512_block(ctx.h, ctx.buf);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            out[8*i + j] = static_cast<kl_u8>(ctx.h[i] >> (56 - 8*j));
}

// ── GF(2^255-19) field arithmetic ────────────────────────────────────────────
// Verbatim from TweetNaCl (constants verified against the source).

// Neutral element limbs
static const kl_gf kGf0 = {0};
static const kl_gf kGf1 = {1};

// d = -121665/121666 mod p  (Edwards curve constant)
static const kl_gf kD = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
// 2*d mod p
static const kl_gf kD2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
// sqrt(-1) mod p
static const kl_gf kI = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
// Base point X coordinate
static const kl_gf kBX = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
// Base point Y coordinate
static const kl_gf kBY = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};

// Carry propagation: normalize limbs to approximately [0, 2^16).
static void gf_car(kl_gf o) {
    kl_i64 c;
    for (int i = 0; i < 16; ++i) {
        o[i] += (kl_i64)1 << 16;
        c = o[i] >> 16;
        o[(i+1) * (i<15)] += c - 1 + 37*(c-1)*(i==15);
        o[i] -= c << 16;
    }
}

// Conditional swap: if b==1, swap p and q; else leave them.
static void gf_cswap(kl_gf p, kl_gf q, kl_u8 b) {
    kl_i64 t, mask = -(kl_i64)b;
    for (int i = 0; i < 16; ++i) {
        t = mask & (p[i] ^ q[i]);
        p[i] ^= t; q[i] ^= t;
    }
}

// Pack a field element into 32 bytes (little-endian canonical).
static void gf_pack(kl_u8 o[32], const kl_gf n) {
    kl_gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    gf_car(t); gf_car(t); gf_car(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i-1]>>16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14]>>16) & 1);
        int b = (m[15]>>16) & 1;
        m[14] &= 0xffff;
        gf_cswap(t, m, (kl_u8)(1-b));
    }
    for (int i = 0; i < 16; ++i) {
        o[2*i]   = static_cast<kl_u8>(t[i]);
        o[2*i+1] = static_cast<kl_u8>(t[i] >> 8);
    }
}

static int gf_neq(const kl_gf a, const kl_gf b) {
    kl_u8 c[32], d[32];
    gf_pack(c, a); gf_pack(d, b);
    int r = 0;
    for (int i = 0; i < 32; ++i) r |= c[i] ^ d[i];
    return r != 0;
}

static kl_u8 gf_par(const kl_gf a) {
    kl_u8 d[32]; gf_pack(d, a); return d[0] & 1;
}

static void gf_unpack(kl_gf o, const kl_u8 n[32]) {
    for (int i = 0; i < 16; ++i)
        o[i] = (kl_i64)n[2*i] + ((kl_i64)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

static void gf_add(kl_gf o, const kl_gf a, const kl_gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}
static void gf_sub(kl_gf o, const kl_gf a, const kl_gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}
static void gf_mul(kl_gf o, const kl_gf a, const kl_gf b) {
    kl_i64 t[31] = {};
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
            t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    gf_car(o); gf_car(o);
}
static void gf_sq(kl_gf o, const kl_gf a) { gf_mul(o, a, a); }

// x^((p-5)/8) used for sqrt recovery in point decompression.
static void gf_pow2523(kl_gf o, const kl_gf i) {
    kl_gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 250; a >= 0; --a) {
        gf_sq(c, c);
        if (a != 1) gf_mul(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

// x^(p-2) = 1/x mod p.
static void gf_inv(kl_gf o, const kl_gf i) {
    kl_gf c;
    for (int j = 0; j < 16; ++j) c[j] = i[j];
    for (int a = 253; a >= 0; --a) {
        gf_sq(c, c);
        if (a != 2 && a != 4) gf_mul(c, c, i);
    }
    for (int j = 0; j < 16; ++j) o[j] = c[j];
}

// ── Extended twisted Edwards point arithmetic ─────────────────────────────────
// Points in (X:Y:Z:T) where x=X/Z, y=Y/Z, T=X*Y/Z.

// Unified point addition (TweetNaCl's add()).
// Result stored in p: p ← p + q.
static void pt_add(kl_gf p[4], kl_gf q[4]) {
    kl_gf a, b, c, d, t, e, f, g, h;
    gf_sub(a, p[1], p[0]);
    gf_sub(t, q[1], q[0]);
    gf_mul(a, a, t);
    gf_add(b, p[0], p[1]);
    gf_add(t, q[0], q[1]);
    gf_mul(b, b, t);
    gf_mul(c, p[3], q[3]);
    gf_mul(c, c, kD2);
    gf_mul(d, p[2], q[2]);
    gf_add(d, d, d);
    gf_sub(e, b, a);
    gf_sub(f, d, c);
    gf_add(g, d, c);
    gf_add(h, b, a);
    gf_mul(p[0], e, f);
    gf_mul(p[1], h, g);
    gf_mul(p[2], g, f);
    gf_mul(p[3], e, h);
}

// Conditional swap of all four coordinates of a point.
static void pt_cswap(kl_gf p[4], kl_gf q[4], kl_u8 b) {
    for (int i = 0; i < 4; ++i) gf_cswap(p[i], q[i], b);
}

// Scalar multiplication: p ← [s]*q (TweetNaCl's scalarmult()).
// q is modified during computation (standard double-and-add ladder).
static void pt_scalarmult(kl_gf p[4], kl_gf q[4], const kl_u8 s[32]) {
    // Initialize p to the neutral element (0, 1, 1, 0).
    for (int j = 0; j < 16; ++j) {
        p[0][j] = kGf0[j];
        p[1][j] = kGf1[j];
        p[2][j] = kGf1[j];
        p[3][j] = kGf0[j];
    }
    for (int i = 255; i >= 0; --i) {
        kl_u8 b = (s[i/8] >> (i & 7)) & 1;
        pt_cswap(p, q, b);
        pt_add(q, p);   // q = q + p
        pt_add(p, p);   // p = 2*p
        pt_cswap(p, q, b);
    }
}

// Scalar multiplication against the standard base point B: p ← [s]*B.
static void pt_scalarbase(kl_gf p[4], const kl_u8 s[32]) {
    kl_gf q[4];
    for (int i = 0; i < 16; ++i) {
        q[0][i] = kBX[i];
        q[1][i] = kBY[i];
        q[2][i] = kGf1[i];
    }
    gf_mul(q[3], kBX, kBY);
    pt_scalarmult(p, q, s);
}

// Decompress a public key into extended coordinates, negating the X coordinate.
// On success, r represents the point -A (negated pubkey), returns true.
// On invalid encoding (not on curve), returns false.
static bool pt_unpackneg(kl_gf r[4], const kl_u8 p[32]) {
    kl_gf t, chk, num, den, den2, den4, den6;
    for (int i = 0; i < 16; ++i) r[2][i] = kGf1[i];
    gf_unpack(r[1], p);
    gf_sq(num, r[1]);
    gf_mul(den, num, kD);
    gf_sub(num, num, r[2]);       // num = y^2 - 1
    gf_add(den, r[2], den);       // den = d*y^2 + 1
    gf_sq(den2, den);
    gf_sq(den4, den2);
    gf_mul(den6, den4, den2);
    gf_mul(t, den6, num);
    gf_mul(t, t, den);
    gf_pow2523(t, t);             // t = (num/den)^((p-5)/8)
    gf_mul(t, t, num);
    gf_mul(t, t, den);
    gf_mul(t, t, den);
    gf_mul(r[0], t, den);         // r[0] = candidate X
    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) gf_mul(r[0], r[0], kI);  // try sqrt(-1)*X
    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) return false;   // not a valid point
    // Negate X to get -A: if current sign matches the encoded sign, flip it.
    if (gf_par(r[0]) == (p[31] >> 7))
        gf_sub(r[0], kGf0, r[0]);
    gf_mul(r[3], r[0], r[1]);
    return true;
}

// Compress a point from extended coordinates to 32 bytes.
static void pt_pack(kl_u8 r[32], kl_gf p[4]) {
    kl_gf tx, ty, zi;
    gf_inv(zi, p[2]);     // zi = 1/Z
    gf_mul(tx, p[0], zi);
    gf_mul(ty, p[1], zi);
    gf_pack(r, ty);
    r[31] ^= gf_par(tx) << 7;
}

// ── Scalar reduction mod L ────────────────────────────────────────────────────
// L = 2^252 + 27742317777372353535851937790883648493

static void kl_modL(kl_u8 r[32], kl_i64 x[64]) {
    static const kl_i64 L[32] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    };
    kl_i64 carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0; int j;
        for (j = i-32; j < i-12; ++j) {
            x[j] += carry - 16*x[i]*L[j-(i-32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry; x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31]>>4)*L[j];
        carry = x[j] >> 8; x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry*L[j];
    for (int i = 0; i < 32; ++i) {
        x[i+1] += x[i] >> 8;
        r[i] = static_cast<kl_u8>(x[i] & 255);
    }
}

// Reduce a 64-byte little-endian integer mod L, result → 32 bytes.
static void kl_reduce(kl_u8 out[32], const kl_u8 in[64]) {
    kl_i64 x[64];
    for (int i = 0; i < 64; ++i) x[i] = static_cast<kl_i64>(in[i]);
    kl_modL(out, x);
}

// Constant-time 32-byte comparison: returns 0 if equal.
static int kl_ct_eq32(const kl_u8* a, const kl_u8* b) {
    unsigned diff = 0;
    for (int i = 0; i < 32; ++i)
        diff |= static_cast<unsigned>(a[i] ^ b[i]);
    // Returns 0 if equal, nonzero if not.
    return (int)((diff | (0u - diff)) >> 31);
}

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────────

/// Verify an Ed25519 signature.
///
/// Implements the standard Ed25519 verification equation:
///   [S]·B = R + [h]·A   where h = SHA-512(R ‖ A ‖ M) mod L
///
/// Equivalently verified as: pack([S]·B + [h]·(-A)) == R
///
/// @param msg      Pointer to message bytes. May be nullptr when msg_len == 0.
/// @param msg_len  Length of the message in bytes.
/// @param sig      64-byte signature (R ‖ S, little-endian).
/// @param pubkey   32-byte compressed public key.
/// @return         true if and only if the signature is valid.
inline bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                            const std::array<uint8_t, 64>& sig,
                            const std::array<uint8_t, 32>& pubkey) {
    // Quick reject: high 3 bits of S (sig[63]) must be clear (S < 2^253 < L*8).
    if (sig[63] & 0xe0) return false;

    // Decompress pubkey A and store as -A in extended coordinates.
    kl_gf A[4];
    if (!pt_unpackneg(A, pubkey.data())) return false;

    // h = SHA-512(R ‖ A ‖ M) mod L
    // where R = sig[0..31], A = pubkey[0..31].
    kl_u8 prefix[64];
    for (int i = 0; i < 32; ++i) prefix[i]    = sig[i];
    for (int i = 0; i < 32; ++i) prefix[32+i] = pubkey[i];

    KlSha512Ctx sha_ctx;
    kl_sha512_init(sha_ctx);
    kl_sha512_update(sha_ctx, prefix, 64);
    if (msg_len > 0) kl_sha512_update(sha_ctx, msg, msg_len);
    kl_u8 hram[64];
    kl_sha512_final(sha_ctx, hram);

    kl_u8 h[32];
    kl_reduce(h, hram);

    // [h]*(-A): scalar mult of -A by h. A is modified but we don't need it after.
    kl_gf hA[4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 16; ++j)
            hA[i][j] = A[i][j];
    pt_scalarmult(hA, A, h);  // hA = [h]*(-A), A (the copy) is modified

    // [S]*B: scalar mult of base point by S = sig[32..63].
    kl_gf SB[4];
    pt_scalarbase(SB, sig.data() + 32);

    // R_computed = [S]*B + [h]*(-A)
    pt_add(SB, hA);

    // Pack and compare to R = sig[0..31].
    kl_u8 R_computed[32];
    pt_pack(R_computed, SB);

    return kl_ct_eq32(sig.data(), R_computed) == 0;
}

} // namespace keylight
