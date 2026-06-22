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
