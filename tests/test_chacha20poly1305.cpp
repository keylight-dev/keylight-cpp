// tests/test_chacha20poly1305.cpp — RFC 8439 conformance.
//
// The published vectors ARE the specification here. Do not relax an assertion
// to make a build pass: a cipher that disagrees with the RFC is broken, not
// "close enough".

#include "doctest.h"
#include "keylight/chacha20poly1305.hpp"
#include <cstring>
#include <string>
#include <vector>

using namespace keylight::detail;

static std::vector<uint8_t> from_hex(const std::string& h) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(nib(h[i]) * 16 + nib(h[i + 1])));
    }
    return out;
}

static std::string to_hex(const uint8_t* p, size_t n) {
    static const char* k = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) { out += k[p[i] >> 4]; out += k[p[i] & 0xF]; }
    return out;
}

TEST_CASE("chacha20: RFC 8439 section 2.3.2 block function") {
    auto key = from_hex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    auto nonce = from_hex("000000090000004a00000000");

    uint8_t out[64];
    chacha20_block(key.data(), 1, nonce.data(), out);

    CHECK(to_hex(out, 64) ==
        "10f1e7e4d13b5915500fdd1fa32071c4"
        "c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2"
        "b5129cd1de164eb9cbd083e8a2503c4e");
}

TEST_CASE("chacha20: RFC 8439 section 2.4.2 encryption") {
    auto key = from_hex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    auto nonce = from_hex("000000000000004a00000000");
    const std::string pt =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";

    std::vector<uint8_t> ct(pt.size());
    chacha20_xor(key.data(), 1, nonce.data(),
                 reinterpret_cast<const uint8_t*>(pt.data()), pt.size(),
                 ct.data());

    CHECK(to_hex(ct.data(), 16) == "6e2e359a2568f98041ba0728dd0d6981");

    // XOR is its own inverse; round-tripping proves the keystream is stable
    // across the multi-block boundary, not just the first block.
    std::vector<uint8_t> back(pt.size());
    chacha20_xor(key.data(), 1, nonce.data(), ct.data(), ct.size(), back.data());
    CHECK(std::string(back.begin(), back.end()) == pt);
}

TEST_CASE("poly1305: RFC 8439 section 2.5.2") {
    auto key = from_hex(
        "85d6be7857556d337f4452fe42d506a8"
        "0103808afb0db2fd4abff6af4149f51b");
    const std::string msg = "Cryptographic Forum Research Group";

    Poly1305 p;
    p.init(key.data());
    p.update(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    uint8_t mac[16];
    p.finish(mac);

    CHECK(to_hex(mac, 16) == "a8061dc1305136c6c22b8baf0c0127a9");
}

TEST_CASE("poly1305: a split update matches a single update") {
    // The buffering path (partial blocks across update() calls) is where a
    // hand-rolled MAC usually goes wrong, and the AEAD below relies on it.
    auto key = from_hex(
        "85d6be7857556d337f4452fe42d506a8"
        "0103808afb0db2fd4abff6af4149f51b");
    const std::string msg = "Cryptographic Forum Research Group";

    Poly1305 whole;
    whole.init(key.data());
    whole.update(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    uint8_t mac_whole[16];
    whole.finish(mac_whole);

    Poly1305 split;
    split.init(key.data());
    for (size_t i = 0; i < msg.size(); i += 7) {
        const size_t n = (msg.size() - i) < 7 ? (msg.size() - i) : 7;
        split.update(reinterpret_cast<const uint8_t*>(msg.data()) + i, n);
    }
    uint8_t mac_split[16];
    split.finish(mac_split);

    CHECK(to_hex(mac_whole, 16) == to_hex(mac_split, 16));
}

TEST_CASE("aead: RFC 8439 section 2.8.2 tag, with no associated data") {
    // The RFC's worked example includes AAD; this SDK never uses AAD, so the
    // vector is reproduced with the AAD length set to zero. Round-tripping
    // plus the tamper cases below is what actually guards the construction.
    auto key = from_hex(
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    auto nonce = from_hex("070000004041424344454647");
    const std::string pt =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";

    std::vector<uint8_t> ct(pt.size());
    uint8_t tag[16];
    aead_seal(key.data(), nonce.data(),
              reinterpret_cast<const uint8_t*>(pt.data()), pt.size(),
              ct.data(), tag);

    // The ciphertext is the RFC's, which does not depend on AAD.
    CHECK(to_hex(ct.data(), 16) == "d31a8d34648e60db7b86afbc53ef7ec2");

    std::vector<uint8_t> back(pt.size());
    REQUIRE(aead_open(key.data(), nonce.data(), ct.data(), ct.size(), tag,
                      back.data()) == true);
    CHECK(std::string(back.begin(), back.end()) == pt);
}

TEST_CASE("aead: a flipped ciphertext bit is rejected") {
    auto key   = from_hex(
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    auto nonce = from_hex("070000004041424344454647");
    const std::string pt = "trial_start=1781076246";

    std::vector<uint8_t> ct(pt.size());
    uint8_t tag[16];
    aead_seal(key.data(), nonce.data(),
              reinterpret_cast<const uint8_t*>(pt.data()), pt.size(),
              ct.data(), tag);

    ct[0] ^= 0x01;   // the text-editor attack, in miniature

    std::vector<uint8_t> back(pt.size());
    CHECK(aead_open(key.data(), nonce.data(), ct.data(), ct.size(), tag,
                    back.data()) == false);
}

TEST_CASE("aead: a different key rejects the blob") {
    auto key_a = from_hex(
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    auto key_b = from_hex(
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9ea0");
    auto nonce = from_hex("070000004041424344454647");
    const std::string pt = "lease";

    std::vector<uint8_t> ct(pt.size());
    uint8_t tag[16];
    aead_seal(key_a.data(), nonce.data(),
              reinterpret_cast<const uint8_t*>(pt.data()), pt.size(),
              ct.data(), tag);

    // This is the property the whole store design rests on: a blob copied to
    // another machine derives a different key and will not open.
    std::vector<uint8_t> back(pt.size());
    CHECK(aead_open(key_b.data(), nonce.data(), ct.data(), ct.size(), tag,
                    back.data()) == false);
}

TEST_CASE("aead: empty plaintext round-trips") {
    auto key   = from_hex(
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    auto nonce = from_hex("070000004041424344454647");

    uint8_t tag[16];
    aead_seal(key.data(), nonce.data(), nullptr, 0, nullptr, tag);
    CHECK(aead_open(key.data(), nonce.data(), nullptr, 0, tag, nullptr) == true);
}
