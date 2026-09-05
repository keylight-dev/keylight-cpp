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

// Poly1305 one-time authenticator (RFC 8439 section 2.5).
//
// 32-bit radix-2^26 limb representation: five 26-bit limbs hold the 130-bit
// accumulator so every partial product fits in a uint64_t without needing
// 128-bit arithmetic, which MSVC does not offer portably.
class Poly1305 {
public:
    void init(const uint8_t key[32]) {
        // r is clamped per the RFC: specific bits are cleared so the
        // multiplication cannot overflow the limb representation.
        r_[0] = (cc_load32_le(key +  0)     ) & 0x3ffffff;
        r_[1] = (cc_load32_le(key +  3) >> 2) & 0x3ffff03;
        r_[2] = (cc_load32_le(key +  6) >> 4) & 0x3ffc0ff;
        r_[3] = (cc_load32_le(key +  9) >> 6) & 0x3f03fff;
        r_[4] = (cc_load32_le(key + 12) >> 8) & 0x00fffff;

        for (int i = 0; i < 5; ++i) h_[i] = 0;
        for (int i = 0; i < 4; ++i) pad_[i] = cc_load32_le(key + 16 + 4 * i);

        leftover_ = 0;
        final_    = false;
    }

    void update(const uint8_t* m, size_t bytes) {
        if (leftover_) {
            size_t want = 16 - leftover_;
            if (want > bytes) want = bytes;
            for (size_t i = 0; i < want; ++i) buffer_[leftover_ + i] = m[i];
            bytes     -= want;
            m         += want;
            leftover_ += want;
            if (leftover_ < 16) return;
            blocks(buffer_, 16);
            leftover_ = 0;
        }
        if (bytes >= 16) {
            const size_t want = bytes & ~static_cast<size_t>(15);
            blocks(m, want);
            m     += want;
            bytes -= want;
        }
        for (size_t i = 0; i < bytes; ++i) buffer_[leftover_ + i] = m[i];
        leftover_ += bytes;
    }

    void finish(uint8_t mac[16]) {
        if (leftover_) {
            size_t i = leftover_;
            buffer_[i++] = 1;
            for (; i < 16; ++i) buffer_[i] = 0;
            final_ = true;
            blocks(buffer_, 16);
        }

        uint32_t h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3], h4 = h_[4];
        uint32_t c;
        c = h1 >> 26; h1 &= 0x3ffffff;
        h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
        h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
        h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;

        // Compute h + -p and select it in constant time if h >= p.
        uint32_t g0 = h0 + 5;             c = g0 >> 26; g0 &= 0x3ffffff;
        uint32_t g1 = h1 + c;             c = g1 >> 26; g1 &= 0x3ffffff;
        uint32_t g2 = h2 + c;             c = g2 >> 26; g2 &= 0x3ffffff;
        uint32_t g3 = h3 + c;             c = g3 >> 26; g3 &= 0x3ffffff;
        uint32_t g4 = h4 + c - (1u << 26);

        uint32_t mask = (g4 >> 31) - 1;   // all ones when g4 did not borrow
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0;
        h1 = (h1 & mask) | g1;
        h2 = (h2 & mask) | g2;
        h3 = (h3 & mask) | g3;
        h4 = (h4 & mask) | g4;

        // Collapse the 26-bit limbs back to four 32-bit words.
        h0 = (h0      ) | (h1 << 26);
        h1 = (h1 >>  6) | (h2 << 20);
        h2 = (h2 >> 12) | (h3 << 14);
        h3 = (h3 >> 18) | (h4 <<  8);

        uint64_t f;
        f = static_cast<uint64_t>(h0) + pad_[0];              h0 = static_cast<uint32_t>(f);
        f = static_cast<uint64_t>(h1) + pad_[1] + (f >> 32);  h1 = static_cast<uint32_t>(f);
        f = static_cast<uint64_t>(h2) + pad_[2] + (f >> 32);  h2 = static_cast<uint32_t>(f);
        f = static_cast<uint64_t>(h3) + pad_[3] + (f >> 32);  h3 = static_cast<uint32_t>(f);

        cc_store32_le(mac +  0, h0);
        cc_store32_le(mac +  4, h1);
        cc_store32_le(mac +  8, h2);
        cc_store32_le(mac + 12, h3);
    }

private:
    void blocks(const uint8_t* m, size_t bytes) {
        const uint32_t hibit = final_ ? 0 : (1u << 24);
        const uint32_t r0 = r_[0], r1 = r_[1], r2 = r_[2], r3 = r_[3], r4 = r_[4];
        const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
        uint32_t h0 = h_[0], h1 = h_[1], h2 = h_[2], h3 = h_[3], h4 = h_[4];

        while (bytes >= 16) {
            h0 += (cc_load32_le(m +  0)     ) & 0x3ffffff;
            h1 += (cc_load32_le(m +  3) >> 2) & 0x3ffffff;
            h2 += (cc_load32_le(m +  6) >> 4) & 0x3ffffff;
            h3 += (cc_load32_le(m +  9) >> 6) & 0x3ffffff;
            h4 += (cc_load32_le(m + 12) >> 8) | hibit;

            uint64_t d0 = static_cast<uint64_t>(h0)*r0 + static_cast<uint64_t>(h1)*s4
                        + static_cast<uint64_t>(h2)*s3 + static_cast<uint64_t>(h3)*s2
                        + static_cast<uint64_t>(h4)*s1;
            uint64_t d1 = static_cast<uint64_t>(h0)*r1 + static_cast<uint64_t>(h1)*r0
                        + static_cast<uint64_t>(h2)*s4 + static_cast<uint64_t>(h3)*s3
                        + static_cast<uint64_t>(h4)*s2;
            uint64_t d2 = static_cast<uint64_t>(h0)*r2 + static_cast<uint64_t>(h1)*r1
                        + static_cast<uint64_t>(h2)*r0 + static_cast<uint64_t>(h3)*s4
                        + static_cast<uint64_t>(h4)*s3;
            uint64_t d3 = static_cast<uint64_t>(h0)*r3 + static_cast<uint64_t>(h1)*r2
                        + static_cast<uint64_t>(h2)*r1 + static_cast<uint64_t>(h3)*r0
                        + static_cast<uint64_t>(h4)*s4;
            uint64_t d4 = static_cast<uint64_t>(h0)*r4 + static_cast<uint64_t>(h1)*r3
                        + static_cast<uint64_t>(h2)*r2 + static_cast<uint64_t>(h3)*r1
                        + static_cast<uint64_t>(h4)*r0;

            uint32_t c;
            c = static_cast<uint32_t>(d0 >> 26); h0 = static_cast<uint32_t>(d0) & 0x3ffffff;
            d1 += c; c = static_cast<uint32_t>(d1 >> 26); h1 = static_cast<uint32_t>(d1) & 0x3ffffff;
            d2 += c; c = static_cast<uint32_t>(d2 >> 26); h2 = static_cast<uint32_t>(d2) & 0x3ffffff;
            d3 += c; c = static_cast<uint32_t>(d3 >> 26); h3 = static_cast<uint32_t>(d3) & 0x3ffffff;
            d4 += c; c = static_cast<uint32_t>(d4 >> 26); h4 = static_cast<uint32_t>(d4) & 0x3ffffff;
            h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
            h1 += c;

            m     += 16;
            bytes -= 16;
        }

        h_[0] = h0; h_[1] = h1; h_[2] = h2; h_[3] = h3; h_[4] = h4;
    }

    uint32_t r_[5]{};
    uint32_t h_[5]{};
    uint32_t pad_[4]{};
    size_t   leftover_ = 0;
    uint8_t  buffer_[16]{};
    bool     final_    = false;
};

// AEAD_CHACHA20_POLY1305 (RFC 8439 section 2.8), with no associated data —
// the store has none, and an unused AAD parameter is a footgun.
//
// `ct` must have room for `pt_len` bytes. `pt` and `ct` may alias.
inline void aead_seal(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t* pt, size_t pt_len,
                      uint8_t* ct, uint8_t tag[16]) {
    // Block 0 of the keystream is the one-time Poly1305 key; the message
    // starts at block 1.
    uint8_t poly_key[64];
    chacha20_block(key, 0, nonce, poly_key);

    if (pt_len > 0) chacha20_xor(key, 1, nonce, pt, pt_len, ct);

    Poly1305 p;
    p.init(poly_key);

    static const uint8_t kZeros[16] = {0};
    if (pt_len > 0) {
        p.update(ct, pt_len);
        const size_t rem = pt_len % 16;
        if (rem) p.update(kZeros, 16 - rem);
    }

    uint8_t lens[16];
    cc_store64_le(lens,     0);          // AAD length
    cc_store64_le(lens + 8, static_cast<uint64_t>(pt_len));
    p.update(lens, 16);
    p.finish(tag);
}

// Returns false on tag mismatch. `pt` is only written when the tag verifies,
// so a caller can never accidentally consume unauthenticated plaintext.
inline bool aead_open(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t* ct, size_t ct_len,
                      const uint8_t tag[16], uint8_t* pt) {
    uint8_t poly_key[64];
    chacha20_block(key, 0, nonce, poly_key);

    Poly1305 p;
    p.init(poly_key);

    static const uint8_t kZeros[16] = {0};
    if (ct_len > 0) {
        p.update(ct, ct_len);
        const size_t rem = ct_len % 16;
        if (rem) p.update(kZeros, 16 - rem);
    }

    uint8_t lens[16];
    cc_store64_le(lens,     0);
    cc_store64_le(lens + 8, static_cast<uint64_t>(ct_len));
    p.update(lens, 16);

    uint8_t expected[16];
    p.finish(expected);

    // Constant-time compare: an early return would leak how many leading tag
    // bytes an attacker guessed correctly.
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= static_cast<uint8_t>(expected[i] ^ tag[i]);
    if (diff != 0) return false;

    if (ct_len > 0) chacha20_xor(key, 1, nonce, ct, ct_len, pt);
    return true;
}

} // namespace detail
} // namespace keylight
