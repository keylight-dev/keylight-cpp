// Tests for keylight::ed25519_verify — RFC 8032 §7.1 Test 1 (empty message)
#include "doctest.h"
#include "keylight/ed25519.hpp"
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

// ── hex decode helpers ────────────────────────────────────────────────────────

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    throw std::invalid_argument("bad hex char");
}

static std::array<uint8_t, 32> hx32(const char* hex) {
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 32; ++i)
        out[i] = static_cast<uint8_t>((hex_nibble(hex[2*i]) << 4) | hex_nibble(hex[2*i+1]));
    return out;
}

static std::array<uint8_t, 64> hx64(const char* hex) {
    std::array<uint8_t, 64> out{};
    for (int i = 0; i < 64; ++i)
        out[i] = static_cast<uint8_t>((hex_nibble(hex[2*i]) << 4) | hex_nibble(hex[2*i+1]));
    return out;
}

// ── RFC 8032 §7.1 Test 1 — empty message ─────────────────────────────────────

TEST_CASE("ed25519 verify RFC8032 test1 (empty msg)") {
    // pubkey: d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
    auto pk = hx32("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    // sig (128 hex chars = 64 bytes):
    // e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b
    auto sig = hx64("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");

    // Valid signature must accept
    CHECK(keylight::ed25519_verify(nullptr, 0, sig, pk) == true);

    // Tamper: flip a bit in the signature → must reject
    sig[0] ^= 0x01;
    CHECK(keylight::ed25519_verify(nullptr, 0, sig, pk) == false);
}

// ── RFC 8032 §7.1 Test 2 — one-byte message ─────────────────────────────────
// (extra coverage; pubkey/sig from the RFC)
TEST_CASE("ed25519 verify RFC8032 test2 (1-byte msg)") {
    auto pk = hx32("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    auto sig = hx64("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
    uint8_t msg[1] = {0x72};
    CHECK(keylight::ed25519_verify(msg, 1, sig, pk) == true);
    sig[63] ^= 0xFF;
    CHECK(keylight::ed25519_verify(msg, 1, sig, pk) == false);
}
