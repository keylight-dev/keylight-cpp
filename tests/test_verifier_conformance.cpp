#include "doctest.h"
#include "keylight/verifier.hpp"
#include "keylight/json.hpp"
#include <fstream>
#include <sstream>

using namespace keylight;

static std::string slurp(const char* p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

TEST_CASE("cross-SDK conformance vectors") {
    auto raw = slurp("tests/fixtures/vectors.json");
    REQUIRE_FALSE(raw.empty());
    auto root = Json::parse(raw).value();

    int skew = (int)root["skewSeconds"].as_int();
    auto vecs = root["vectors"];
    REQUIRE(vecs.size() == 8);

    for (size_t i = 0; i < vecs.size(); ++i) {
        auto v = vecs.at(i);
        Lease lease = Lease::from_json(v["lease"]).value();

        std::map<std::string, std::string> keys;
        for (auto& kid : v["trustedKeys"].keys())
            keys[kid] = v["trustedKeys"][kid].as_string();

        Verifier verifier(keys, skew);
        auto got = verifier.verify(lease, v["now"].as_int());
        auto exp = v["expect"];

        CAPTURE(v["name"].as_string());
        CHECK(got.kidKnown       == exp["kidKnown"].as_bool());
        CHECK(got.signatureValid == exp["signatureValid"].as_bool());
        CHECK(got.expired        == exp["expired"].as_bool());
    }
}
