#include "doctest.h"
#include "keylight/json.hpp"
using keylight::Json;

TEST_CASE("parse lease-shaped json") {
    auto r = Json::parse(R"({"kid":"k1","issuedAt":1781076246,"entitlements":["pro","a"],"status":"active"})");
    REQUIRE(r.is_ok());
    Json j = r.value();
    CHECK(j["kid"].as_string() == "k1");
    CHECK(j["issuedAt"].as_int() == 1781076246);
    CHECK(j["entitlements"].size() == 2);
    CHECK(j["entitlements"].at(0).as_string() == "pro");
}

TEST_CASE("keys() returns object member names") {
    auto r = Json::parse(R"({"b":2,"a":1,"c":3})");
    REQUIRE(r.is_ok());
    auto ks = r.value().keys();
    // order must match insertion order
    REQUIRE(ks.size() == 3);
    CHECK(ks[0] == "b");
    CHECK(ks[1] == "a");
    CHECK(ks[2] == "c");
}

TEST_CASE("malformed json returns error Result") {
    auto r = Json::parse(R"({"key": "unterminated)");
    CHECK_FALSE(r.is_ok());
}

TEST_CASE("an integer that overflows int64 is promoted, not UB") {
    // `ival * 10 + digit` past INT64_MAX is signed-overflow UB. The parser
    // promotes to double instead; as_int() then saturates rather than
    // casting an out-of-range double (also UB).
    auto r = Json::parse(R"({"big":99999999999999999999999,"neg":-99999999999999999999999,)"
                         R"("max":9223372036854775807,"min":-9223372036854775808})");
    REQUIRE(r.is_ok());
    Json j = r.value();
    CHECK(j["big"].as_int() == INT64_MAX);
    CHECK(j["neg"].as_int() == INT64_MIN);
    CHECK(j["max"].as_int() == INT64_MAX);      // still an exact int64
    CHECK(j["min"].as_int() == INT64_MIN);      // -(2^63) overflows on the way in; saturates
}

TEST_CASE("as_int saturates an out-of-range double") {
    auto r = Json::parse(R"({"huge":1e300,"tiny":-1e300,"ok":42.9})");
    REQUIRE(r.is_ok());
    Json j = r.value();
    CHECK(j["huge"].as_int() == INT64_MAX);
    CHECK(j["tiny"].as_int() == INT64_MIN);
    CHECK(j["ok"].as_int()   == 42);
}
