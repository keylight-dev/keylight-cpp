#include "doctest.h"
#include "keylight/result.hpp"
using namespace keylight;

TEST_CASE("result ok/err + base64") {
    // Result<int> ok path
    auto r = Result<int>::ok(5);
    CHECK(r.is_ok());
    CHECK(r.value() == 5);

    // Result<int> err path
    auto e = Result<int>::err({ErrorCode::Network, "down"});
    CHECK_FALSE(e.is_ok());
    CHECK(e.error_message() == "down");
    CHECK(e.error().code == ErrorCode::Network);

    // Known base64 vector from brief
    CHECK(base64_decode("YWJj") == "abc");

    // Padding cases
    CHECK(base64_decode("YQ==") == "a");
    CHECK(base64_decode("YWI=") == "ab");

    // Round-trip encode/decode for arbitrary bytes including padding triggers
    std::string s1 = "a";          // len 1 => "==" padding
    std::string s2 = "ab";         // len 2 => "="  padding
    std::string s3 = "abc";        // len 3 => no  padding
    std::string s4 = "Hello, World! This is a longer string with various bytes.";
    CHECK(base64_decode(base64_encode(s1)) == s1);
    CHECK(base64_decode(base64_encode(s2)) == s2);
    CHECK(base64_decode(base64_encode(s3)) == s3);
    CHECK(base64_decode(base64_encode(s4)) == s4);
    CHECK(base64_encode(s3) == "YWJj");

    // Byte-safety: arbitrary bytes including null and high-bit (used by Ed25519 keys/sigs)
    std::string explicit_binary("\x00\x01\xFF\xFE\x80\x7F", 6);
    CHECK(base64_decode(base64_encode(explicit_binary)) == explicit_binary);

    std::string all_bytes;
    for (int b = 0; b < 256; ++b) {
        all_bytes.push_back(static_cast<char>(b));
    }
    CHECK(base64_decode(base64_encode(all_bytes)) == all_bytes);

    // Result<void> specialisation
    auto vok = Result<void>::ok();
    CHECK(vok.is_ok());
    auto verr = Result<void>::err({ErrorCode::Config, "missing"});
    CHECK_FALSE(verr.is_ok());
    CHECK(verr.error_message() == "missing");
}
