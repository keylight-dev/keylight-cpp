// tests/test_amalgamation.cpp — standalone compile test for keylight_single.hpp
//
// This file MUST include ONLY keylight_single.hpp.  It must compile without
// any other Keylight headers on the include path (other than the single-header
// file itself, which must be in the include path or resolved relative to the
// source root).
//
// No doctest framework is used here on purpose: doctest pulls in extra headers
// and its config macros.  We use static_assert + a simple main() so the only
// dependency is the C++ standard library, exactly as an end-user would see it.

#include "keylight_single.hpp"

#include <cassert>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// FakeTransport — minimal Transport implementation for compile testing.
// Always returns a canned 200 JSON response.
// ---------------------------------------------------------------------------
struct FakeTransport : keylight::Transport {
    std::string canned_body;
    int         canned_status = 200;

    explicit FakeTransport(std::string body = "{}", int status = 200)
        : canned_body(std::move(body)), canned_status(status) {}

    keylight::Result<keylight::HttpResponse> request(
        const std::string& /*method*/,
        const std::string& /*url*/,
        const std::map<std::string, std::string>& /*headers*/,
        const std::string& /*body*/) override
    {
        keylight::HttpResponse resp;
        resp.status = canned_status;
        resp.body   = canned_body;
        return keylight::Result<keylight::HttpResponse>::ok(resp);
    }
};

// ---------------------------------------------------------------------------
// FakeStore — minimal LicenseStore implementation.
// ---------------------------------------------------------------------------
struct FakeStore : keylight::LicenseStore {
    std::string stored;

    keylight::Result<std::string> load() override {
        return keylight::Result<std::string>::ok(stored);
    }

    keylight::Result<void> save(const std::string& data) override {
        stored = data;
        return keylight::Result<void>::ok();
    }

    keylight::Result<void> clear() override {
        stored.clear();
        return keylight::Result<void>::ok();
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_sdk_version() {
    std::string ver = KEYLIGHT_SDK_VERSION;
    assert(!ver.empty() && "KEYLIGHT_SDK_VERSION must be non-empty");
    std::cout << "  sdk_version: " << ver << "\n";
}

static void test_canonical_payload() {
    // Verify that canonical_payload produces the expected pipe-delimited string.
    keylight::Lease l;
    l.kid            = "k1";
    l.licenseKeyHash = "abc";
    l.instanceId     = "dev1";
    l.issuedAt       = 1000;
    l.expiresAt      = 9000;
    l.status         = "active";
    // No entitlements → trailing empty field
    std::string p = keylight::canonical_payload(l);
    assert(p == "v3|k1|abc|dev1|1000|9000|active|" && "canonical_payload mismatch");

    // With entitlements (must be sorted ascending)
    l.entitlements = {"pro", "basic"};
    p = keylight::canonical_payload(l);
    assert(p == "v3|k1|abc|dev1|1000|9000|active|basic,pro" && "entitlements sort mismatch");
    std::cout << "  canonical_payload: ok\n";
}

static void test_base64_roundtrip() {
    std::string original = "Hello, Keylight!";
    std::string encoded  = keylight::base64_encode(original);
    std::string decoded  = keylight::base64_decode(encoded);
    assert(decoded == original && "base64 round-trip failed");
    std::cout << "  base64 round-trip: ok\n";
}

static void test_config_construction() {
    keylight::Config cfg;
    cfg.tenantId  = "test-tenant";
    cfg.productId = "test-product";
    cfg.sdkKey    = "sdk_test_key";
    assert(cfg.maxOfflineDays == 7);
    assert(cfg.apiBaseUrl == "https://api.keylight.dev");
    std::cout << "  Config construction: ok\n";
}

static void test_result_ok_err() {
    auto ok_result = keylight::Result<int>::ok(42);
    assert(ok_result.is_ok());
    assert(ok_result.value() == 42);

    auto err_result = keylight::Result<int>::err(
        {keylight::ErrorCode::Network, "no network"});
    assert(!err_result.is_ok());
    assert(err_result.error().code == keylight::ErrorCode::Network);

    std::cout << "  Result<T>: ok\n";
}

static void test_result_void() {
    auto ok_v = keylight::Result<void>::ok();
    assert(ok_v.is_ok());

    auto err_v = keylight::Result<void>::err(
        {keylight::ErrorCode::Io, "disk full"});
    assert(!err_v.is_ok());
    std::cout << "  Result<void>: ok\n";
}

static void test_verifier_unknown_kid() {
    // A verifier with no trusted keys must always return not-trusted.
    keylight::Verifier v({});
    keylight::Lease l;
    l.kid       = "unknown";
    l.status    = "active";
    l.issuedAt  = 1000;
    l.expiresAt = 9999999999LL;
    l.signature = "AAAA";

    auto vr = v.verify(l, 1001);
    assert(!vr.kidKnown);
    assert(!vr.is_trusted());
    std::cout << "  Verifier (unknown kid): ok\n";
}

static void test_client_initial_state() {
    keylight::Config cfg;
    cfg.tenantId  = "acme";
    cfg.productId = "widget";
    cfg.sdkKey    = "sk_test";

    FakeTransport transport;
    FakeStore     store;

    keylight::Client client(cfg, transport, store);
    assert(client.state() == keylight::State::Invalid);
    std::cout << "  Client initial state (Invalid): ok\n";
}

static void test_json_parse() {
    auto jr = keylight::Json::parse(R"({"hello":"world","n":42})");
    assert(jr.is_ok());
    assert(jr.value()["hello"].as_string() == "world");
    assert(jr.value()["n"].as_int() == 42);
    std::cout << "  Json::parse: ok\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "test_amalgamation: running checks\n";

    test_sdk_version();
    test_canonical_payload();
    test_base64_roundtrip();
    test_config_construction();
    test_result_ok_err();
    test_result_void();
    test_verifier_unknown_kid();
    test_client_initial_state();
    test_json_parse();

    std::cout << "test_amalgamation: ALL PASSED\n";
    return 0;
}
