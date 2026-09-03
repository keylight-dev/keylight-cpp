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

// Every check below is a bare assert().  A Release build defines NDEBUG, which
// would compile all of them away and leave this test passing vacuously — the
// release workflow builds Release, so that hole was live.  Undefine it before
// <cassert> so the checks run in every configuration.
#undef NDEBUG
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

    // Headers of the most recent request (used by the SDK-key auth check).
    std::map<std::string, std::string> last_headers;

    explicit FakeTransport(std::string body = "{}", int status = 200)
        : canned_body(std::move(body)), canned_status(status) {}

    keylight::Result<keylight::HttpResponse> request(
        const std::string& /*method*/,
        const std::string& /*url*/,
        const std::map<std::string, std::string>& headers,
        const std::string& /*body*/) override
    {
        last_headers = headers;
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
    assert(cfg.maxOfflineDays == 15);
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

static void test_sdk_key_header() {
    keylight::Config cfg;
    cfg.tenantId  = "acme";
    cfg.productId = "widget";
    cfg.sdkKey    = "sdk_live_single_header";

    FakeTransport transport(R"({"activated":false})");
    FakeStore     store;

    keylight::Client client(cfg, transport, store);
    (void)client.activate("KEY-0001");

    auto it = transport.last_headers.find("X-Keylight-SDK-Key");
    assert(it != transport.last_headers.end() &&
           "activate must send X-Keylight-SDK-Key");
    assert(it->second == "sdk_live_single_header" && "SDK key value mismatch");
    std::cout << "  X-Keylight-SDK-Key header: ok\n";
}

static void test_local_trial() {
    keylight::Config cfg;
    cfg.tenantId          = "acme";
    cfg.productId         = "widget";
    cfg.sdkKey            = "sk_test";
    cfg.trialDurationDays = 14;

    FakeTransport transport;
    FakeStore     store;

    int64_t now = 1'700'000'000LL;
    keylight::Client client(cfg, transport, store, [&]{ return now; });

    // Nothing starts a trial implicitly.
    assert(client.checkTrial() == keylight::TrialStatus::NotStarted);
    assert(client.state() == keylight::State::Invalid);

    auto r = client.startTrial();
    assert(r.is_ok() && r.value() == keylight::State::Trial);
    assert(client.checkTrial() == keylight::TrialStatus::Active);
    assert(client.trialDaysLeft() == 14);

    // Persisted and restored, with no network call.
    now += 3 * 86400;
    keylight::Client relaunched(cfg, transport, store, [&]{ return now; });
    assert(relaunched.state() == keylight::State::Trial);
    assert(relaunched.trialDaysLeft() == 11);

    // Elapsed → Expired, and never restartable.
    now += 20 * 86400;
    assert(relaunched.checkTrial() == keylight::TrialStatus::Expired);
    assert(relaunched.startTrial().value() == keylight::State::Expired);
    std::cout << "  local trial: ok\n";
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
    test_sdk_key_header();
    test_local_trial();
    test_json_parse();

    std::cout << "test_amalgamation: ALL PASSED\n";
    return 0;
}
