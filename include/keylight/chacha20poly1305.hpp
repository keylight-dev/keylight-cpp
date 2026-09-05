#pragma once
// keylight/chacha20poly1305.hpp — RFC 8439 AEAD_CHACHA20_POLY1305, vendored.
//
// Used to encrypt the local license store with a key bound to this machine.
// Vendored rather than depended upon: this SDK is header-only with no external
// dependencies, and it already vendors SHA-256 and Ed25519 for the same reason.
//
// Conformance is pinned to the RFC 8439 test vectors in
// tests/test_chacha20poly1305.cpp. Those vectors are the specification.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace keylight {
namespace detail {

inline uint32_t cc_rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

inline uint32_t cc_load32_le(const uint8_t* p) {
    return  static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

inline void cc_store32_le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline void cc_store64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline void cc_quarter_round(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    a += b; d ^= a; d = cc_rotl32(d, 16);
    c += d; b ^= c; b = cc_rotl32(b, 12);
    a += b; d ^= a; d = cc_rotl32(d, 8);
    c += d; b ^= c; b = cc_rotl32(b, 7);
}

// One 64-byte ChaCha20 block (RFC 8439 section 2.3).
inline void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
    static const uint8_t kSigma[16] = {
        'e','x','p','a','n','d',' ','3','2','-','b','y','t','e',' ','k'
    };

    uint32_t s[16];
    for (int i = 0; i < 4; ++i)  s[i]      = cc_load32_le(kSigma + 4 * i);
    for (int i = 0; i < 8; ++i)  s[4 + i]  = cc_load32_le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; ++i)  s[13 + i] = cc_load32_le(nonce + 4 * i);

    uint32_t x[16];
    std::memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; ++i) {          // 20 rounds = 10 double rounds
        cc_quarter_round(x[0], x[4], x[8],  x[12]);
        cc_quarter_round(x[1], x[5], x[9],  x[13]);
        cc_quarter_round(x[2], x[6], x[10], x[14]);
        cc_quarter_round(x[3], x[7], x[11], x[15]);
        cc_quarter_round(x[0], x[5], x[10], x[15]);
        cc_quarter_round(x[1], x[6], x[11], x[12]);
        cc_quarter_round(x[2], x[7], x[8],  x[13]);
        cc_quarter_round(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; ++i) cc_store32_le(out + 4 * i, x[i] + s[i]);
}

// XOR `len` bytes with the keystream starting at `counter`. In-place safe
// (`in` and `out` may alias).
inline void chacha20_xor(const uint8_t key[32], uint32_t counter,
                         const uint8_t nonce[12],
                         const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t block[64];
    size_t  off = 0;
    while (off < len) {
        chacha20_block(key, counter, nonce, block);
        const size_t n = (len - off) < 64 ? (len - off) : 64;
        for (size_t i = 0; i < n; ++i) out[off + i] = in[off + i] ^ block[i];
        off += n;
        ++counter;
    }
}

} // namespace detail
} // namespace keylight
