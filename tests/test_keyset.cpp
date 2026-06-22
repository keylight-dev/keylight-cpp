// tests/test_keyset.cpp — unit tests for keylight::fetchKeyset
//
// Uses a FakeTransport (no network) — always included in the default build.

#include "doctest.h"
#include "keylight/keyset.hpp"
#include "keylight/transport.hpp"
#include "keylight/result.hpp"

#include <map>
#include <string>

using namespace keylight;

// ---------------------------------------------------------------------------
// FakeTransport — canned HTTP response
// ---------------------------------------------------------------------------
namespace {

class FakeTransport : public Transport {
public:
    int         status = 200;
    std::string body;
    bool        fail   = false;

    std::string last_method;
    std::string last_url;

    Result<HttpResponse> request(
        const std::string& method,
        const std::string& url,
        const std::map<std::string, std::string>&,
        const std::string&) override
    {
        last_method = method;
        last_url    = url;
        if (fail) {
            return Result<HttpResponse>::err(
                {ErrorCode::Network, "simulated network failure"});
        }
        return Result<HttpResponse>::ok({status, body});
    }
};

} // anonymous namespace

// Canned keyset JSON matching the demo tenant shape
static const char* KEYSET_JSON = R"({
  "primary_kid": "k1",
  "keys": [
    {
      "kid": "k1",
      "alg": "ed25519",
      "public_key": "8QkyJGwaIqAuN0jdsCnBtv3D9fylv4PHqCVufx7xje0="
    },
    {
      "kid": "k2",
      "alg": "ed25519",
      "public_key": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
    }
  ]
})";

TEST_CASE("fetchKeyset — builds kid→public_key map from valid response") {
    FakeTransport ft;
    ft.status = 200;
    ft.body   = KEYSET_JSON;

    auto res = fetchKeyset(ft, "https://api.keylight.dev", "keylight-notes-demo");

    CHECK(res.is_ok());
    const auto& m = res.value();
    CHECK(m.size() == 2);
    CHECK(m.count("k1") == 1);
    CHECK(m.at("k1") == "8QkyJGwaIqAuN0jdsCnBtv3D9fylv4PHqCVufx7xje0=");
    CHECK(m.count("k2") == 1);
    CHECK(m.at("k2") == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
}

TEST_CASE("fetchKeyset — builds correct URL") {
    FakeTransport ft;
    ft.status = 200;
    ft.body   = KEYSET_JSON;

    fetchKeyset(ft, "https://api.keylight.dev", "keylight-notes-demo");

    CHECK(ft.last_method == "GET");
    CHECK(ft.last_url ==
          "https://api.keylight.dev/keylight-notes-demo/.well-known/keylight-keys");
}

TEST_CASE("fetchKeyset — strips trailing slash from baseUrl") {
    FakeTransport ft;
    ft.status = 200;
    ft.body   = KEYSET_JSON;

    fetchKeyset(ft, "https://api.keylight.dev/", "keylight-notes-demo");

    CHECK(ft.last_url ==
          "https://api.keylight.dev/keylight-notes-demo/.well-known/keylight-keys");
}

TEST_CASE("fetchKeyset — network failure → err(Network)") {
    FakeTransport ft;
    ft.fail = true;

    auto res = fetchKeyset(ft, "https://api.keylight.dev", "keylight-notes-demo");

    CHECK(!res.is_ok());
    CHECK(res.error().code == ErrorCode::Network);
}

TEST_CASE("fetchKeyset — non-200 response → err(Http)") {
    FakeTransport ft;
    ft.status = 404;
    ft.body   = R"({"error":"not_found"})";

    auto res = fetchKeyset(ft, "https://api.keylight.dev", "keylight-notes-demo");

    CHECK(!res.is_ok());
    CHECK(res.error().code == ErrorCode::Http);
}

TEST_CASE("fetchKeyset — invalid JSON → err(BadResponse)") {
    FakeTransport ft;
    ft.status = 200;
    ft.body   = "not json";

    auto res = fetchKeyset(ft, "https://api.keylight.dev", "keylight-notes-demo");

    CHECK(!res.is_ok());
    CHECK(res.error().code == ErrorCode::BadResponse);
}

TEST_CASE("fetchKeyset — missing keys array → err(BadResponse)") {
    FakeTransport ft;
    ft.status = 200;
    ft.body   = R"({"primary_kid":"k1"})";

    auto res = fetchKeyset(ft, "https://api.keylight.dev", "keylight-notes-demo");

    CHECK(!res.is_ok());
    CHECK(res.error().code == ErrorCode::BadResponse);
}

TEST_CASE("fetchKeyset — empty keys array → err(BadResponse)") {
    FakeTransport ft;
    ft.status = 200;
    ft.body   = R"({"primary_kid":"k1","keys":[]})";

    auto res = fetchKeyset(ft, "https://api.keylight.dev", "keylight-notes-demo");

    CHECK(!res.is_ok());
    CHECK(res.error().code == ErrorCode::BadResponse);
}
