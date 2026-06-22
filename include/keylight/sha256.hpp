// Keylight SHA-256 — header-only, zero external dependencies.
// Core transform adapted from Brad Conte's public-domain sha256.c
// (https://github.com/B-Con/crypto-algorithms), CC0 / public domain.
// Wrapped in namespace keylight; internals in anonymous namespace.
//
// PUBLIC UTILITY — standalone SHA-256 for integrators (e.g. hashing license
// keys, building custom audit trails).  The core SDK verification path does
// NOT use this header (Ed25519 uses SHA-512 internally); sha256.hpp is
// therefore NOT part of keylight.hpp's include closure.
// Include it directly when you need it:
//   #include <keylight/sha256.hpp>
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace keylight {

namespace {

// ── SHA-256 constants ─────────────────────────────────────────────────────────

static const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

// ── Bit-manipulation helpers ──────────────────────────────────────────────────

inline uint32_t rotr32(uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}

inline uint32_t ch(uint32_t e, uint32_t f, uint32_t g) noexcept {
    return (e & f) ^ (~e & g);
}

inline uint32_t maj(uint32_t a, uint32_t b, uint32_t c) noexcept {
    return (a & b) ^ (a & c) ^ (b & c);
}

inline uint32_t ep0(uint32_t a) noexcept {
    return rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
}

inline uint32_t ep1(uint32_t e) noexcept {
    return rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
}

inline uint32_t sig0(uint32_t x) noexcept {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

inline uint32_t sig1(uint32_t x) noexcept {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

// ── SHA-256 context ───────────────────────────────────────────────────────────

struct Sha256Ctx {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
};

inline void sha256_init(Sha256Ctx& ctx) noexcept {
    ctx.datalen = 0;
    ctx.bitlen  = 0;
    ctx.state[0] = 0x6a09e667u;
    ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u;
    ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu;
    ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu;
    ctx.state[7] = 0x5be0cd19u;
}

inline void sha256_transform(Sha256Ctx& ctx, const uint8_t* data) noexcept {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];

    for (unsigned i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (static_cast<uint32_t>(data[j    ]) << 24)
              | (static_cast<uint32_t>(data[j + 1]) << 16)
              | (static_cast<uint32_t>(data[j + 2]) <<  8)
              |  static_cast<uint32_t>(data[j + 3]);

    for (unsigned i = 16; i < 64; ++i)
        m[i] = sig1(m[i - 2]) + m[i - 7] + sig0(m[i - 15]) + m[i - 16];

    a = ctx.state[0]; b = ctx.state[1]; c = ctx.state[2]; d = ctx.state[3];
    e = ctx.state[4]; f = ctx.state[5]; g = ctx.state[6]; h = ctx.state[7];

    for (unsigned i = 0; i < 64; ++i) {
        t1 = h + ep1(e) + ch(e, f, g) + kK[i] + m[i];
        t2 = ep0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

inline void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) {
        ctx.data[ctx.datalen++] = data[i];
        if (ctx.datalen == 64) {
            sha256_transform(ctx, ctx.data);
            ctx.bitlen += 512;
            ctx.datalen = 0;
        }
    }
}

inline void sha256_final(Sha256Ctx& ctx, uint8_t* hash) noexcept {
    uint32_t i = ctx.datalen;

    // Pad message
    if (ctx.datalen < 56) {
        ctx.data[i++] = 0x80u;
        while (i < 56) ctx.data[i++] = 0x00u;
    } else {
        ctx.data[i++] = 0x80u;
        while (i < 64) ctx.data[i++] = 0x00u;
        sha256_transform(ctx, ctx.data);
        for (unsigned k = 0; k < 56; ++k) ctx.data[k] = 0x00u;
    }

    // Append bit length (big-endian)
    ctx.bitlen += static_cast<uint64_t>(ctx.datalen) * 8u;
    ctx.data[63] = static_cast<uint8_t>( ctx.bitlen        & 0xffu);
    ctx.data[62] = static_cast<uint8_t>((ctx.bitlen >>  8) & 0xffu);
    ctx.data[61] = static_cast<uint8_t>((ctx.bitlen >> 16) & 0xffu);
    ctx.data[60] = static_cast<uint8_t>((ctx.bitlen >> 24) & 0xffu);
    ctx.data[59] = static_cast<uint8_t>((ctx.bitlen >> 32) & 0xffu);
    ctx.data[58] = static_cast<uint8_t>((ctx.bitlen >> 40) & 0xffu);
    ctx.data[57] = static_cast<uint8_t>((ctx.bitlen >> 48) & 0xffu);
    ctx.data[56] = static_cast<uint8_t>((ctx.bitlen >> 56) & 0xffu);
    sha256_transform(ctx, ctx.data);

    // Produce big-endian digest bytes
    for (unsigned j = 0; j < 4; ++j) {
        hash[     j] = static_cast<uint8_t>((ctx.state[0] >> (24 - j * 8)) & 0xffu);
        hash[ 4 + j] = static_cast<uint8_t>((ctx.state[1] >> (24 - j * 8)) & 0xffu);
        hash[ 8 + j] = static_cast<uint8_t>((ctx.state[2] >> (24 - j * 8)) & 0xffu);
        hash[12 + j] = static_cast<uint8_t>((ctx.state[3] >> (24 - j * 8)) & 0xffu);
        hash[16 + j] = static_cast<uint8_t>((ctx.state[4] >> (24 - j * 8)) & 0xffu);
        hash[20 + j] = static_cast<uint8_t>((ctx.state[5] >> (24 - j * 8)) & 0xffu);
        hash[24 + j] = static_cast<uint8_t>((ctx.state[6] >> (24 - j * 8)) & 0xffu);
        hash[28 + j] = static_cast<uint8_t>((ctx.state[7] >> (24 - j * 8)) & 0xffu);
    }
}

static const char kHexChars[] = "0123456789abcdef";

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────────

/// Hash `len` bytes at `data`; return raw 32-byte digest.
inline std::array<uint8_t, 32> sha256_bytes(const uint8_t* data, size_t len) noexcept {
    Sha256Ctx ctx;
    sha256_init(ctx);
    sha256_update(ctx, data, len);
    std::array<uint8_t, 32> digest{};
    sha256_final(ctx, digest.data());
    return digest;
}

/// Hash a UTF-8 string; return 64-char lowercase hex digest.
inline std::string sha256_hex(const std::string& input) {
    auto digest = sha256_bytes(
        reinterpret_cast<const uint8_t*>(input.data()), input.size());
    std::string hex;
    hex.reserve(64);
    for (uint8_t byte : digest) {
        hex += kHexChars[(byte >> 4) & 0xfu];
        hex += kHexChars[ byte       & 0xfu];
    }
    return hex;
}

} // namespace keylight
