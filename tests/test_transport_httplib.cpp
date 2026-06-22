// tests/test_transport_httplib.cpp
//
// CONTRACT TESTS for the Transport interface.
// A FakeTransport validates that the abstract interface compiles, dispatches,
// and that Result<HttpResponse> propagates correctly — no OpenSSL required.
//
// The real HttplibTransport is compiled only when
//   KEYLIGHT_BUILD_HTTPLIB_TRANSPORT=ON
// and a live call is only attempted when the env-var KEYLIGHT_LIVE is set.

#include "doctest.h"
#include "keylight/transport.hpp"

#ifdef KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
#  include "keylight/transport/httplib.hpp"
#endif

#include <cstdlib>
#include <map>
#include <string>

// ---------------------------------------------------------------------------
// FakeTransport — test double, zero external dependencies
// ---------------------------------------------------------------------------
namespace {

class FakeTransport : public keylight::Transport {
public:
    // Configurable for positive / negative tests
    bool        succeed    = true;
    int         status     = 200;
    std::string body       = R"({"ok":true})";

    // Capture the last call for assertions
    std::string last_method;
    std::string last_url;
    std::map<std::string, std::string> last_headers;
    std::string last_body;

    keylight::Result<keylight::HttpResponse> request(
        const std::string&                        method,
        const std::string&                        url,
        const std::map<std::string, std::string>& headers,
        const std::string&                        req_body
    ) override {
        last_method  = method;
        last_url     = url;
        last_headers = headers;
        last_body    = req_body;

        if (!succeed) {
            return keylight::Result<keylight::HttpResponse>::err(
                {keylight::ErrorCode::Network, "simulated network failure"});
        }
        return keylight::Result<keylight::HttpResponse>::ok({status, body});
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Contract tests
// ---------------------------------------------------------------------------
TEST_CASE("Transport interface — FakeTransport compiles and dispatches") {
    FakeTransport ft;

    // Baseline: pointer-to-Transport dispatches to the fake
    keylight::Transport* tp = &ft;
    auto res = tp->request("GET", "https://api.keylight.dev/v1/status", {}, "");

    CHECK(res.is_ok());
    CHECK(res.value().status == 200);
    CHECK(res.value().body == R"({"ok":true})");
}

TEST_CASE("Transport interface — captures method, url, headers, body") {
    FakeTransport ft;
    std::map<std::string, std::string> hdrs = {
        {"Authorization", "Bearer tok"},
        {"Content-Type",  "application/json"},
    };
    std::string payload = R"({"key":"KL-TEST"})";

    ft.request("POST", "https://api.keylight.dev/v1/activate", hdrs, payload);

    CHECK(ft.last_method  == "POST");
    CHECK(ft.last_url     == "https://api.keylight.dev/v1/activate");
    CHECK(ft.last_headers.at("Authorization") == "Bearer tok");
    CHECK(ft.last_body    == payload);
}

TEST_CASE("Transport interface — network error propagates as ErrorCode::Network") {
    FakeTransport ft;
    ft.succeed = false;

    auto res = ft.request("GET", "https://api.keylight.dev/v1/status", {}, "");

    CHECK(!res.is_ok());
    CHECK(res.error().code == keylight::ErrorCode::Network);
}

TEST_CASE("Transport interface — arbitrary HTTP status codes pass through") {
    FakeTransport ft;
    ft.status = 404;
    ft.body   = R"({"error":"not_found"})";

    auto res = ft.request("GET", "https://api.keylight.dev/v1/status", {}, "");

    CHECK(res.is_ok());               // transport success (got a response)
    CHECK(res.value().status == 404); // HTTP 404 — caller decides what to do
    CHECK(res.value().body   == R"({"error":"not_found"})");
}

TEST_CASE("Transport interface — empty body and empty headers are valid") {
    FakeTransport ft;
    auto res = ft.request("DELETE", "https://api.keylight.dev/v1/devices/abc",
                          {}, "");
    CHECK(res.is_ok());
    CHECK(ft.last_headers.empty());
    CHECK(ft.last_body.empty());
}

// ---------------------------------------------------------------------------
// HttplibTransport — only compiled + run when opt-in flag is set AND
// the KEYLIGHT_LIVE env-var is present (to avoid CI hitting the network).
// ---------------------------------------------------------------------------
#ifdef KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
TEST_CASE("HttplibTransport — live smoke (KEYLIGHT_LIVE env required)") {
    if (!std::getenv("KEYLIGHT_LIVE")) {
        MESSAGE("KEYLIGHT_LIVE not set — skipping live transport test");
        return;
    }

    keylight::HttplibTransport ht;
    auto res = ht.request("GET", "https://api.keylight.dev/v1/status", {}, "");

    // We only assert the transport layer worked — not the HTTP status
    CHECK(res.is_ok());
    MESSAGE("HTTP status: " << res.value().status);
    MESSAGE("body (first 200 chars): " <<
            res.value().body.substr(0, 200));
}
#endif
