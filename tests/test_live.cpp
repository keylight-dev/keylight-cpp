// tests/test_live.cpp — env-gated live integration tests vs the demo tenant
//
// Mirrors keylight-js tests/live_integration.test.ts and
//         keylight-rust keylight/tests/live_integration.rs
//
// Gate 1 (env):    set KEYLIGHT_LIVE=1 to run. Without it the whole suite
//                  returns immediately — no network calls, test passes.
// Gate 2 (compile): this file is only linked into keylight_tests_httplib
//                  (built with KEYLIGHT_BUILD_HTTPLIB_TRANSPORT=ON + OpenSSL).
//                  The real HttplibTransport is only used when that flag is set.
//
// Demo tenant constants — verbatim match with JS/Rust live tests:
//   BASE      = "https://api.keylight.dev"
//   TENANT    = "keylight-notes-demo"
//   PRODUCT   = "notes"
//   keyPrefix = "NOTES"
//   No SDK key — keyset fetch + activate/deactivate are public on the demo tenant.

#include "doctest.h"
#include "keylight/keylight.hpp"
#include "keylight/keyset.hpp"

#ifdef KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
#  include "keylight/transport/httplib.hpp"
#endif

#include <cstdlib>
#include <map>
#include <string>

using namespace keylight;

// ---------------------------------------------------------------------------
// Demo tenant constants
// ---------------------------------------------------------------------------
static constexpr const char* BASE      = "https://api.keylight.dev";
static constexpr const char* TENANT    = "keylight-notes-demo";
static constexpr const char* PRODUCT   = "notes";
static constexpr const char* KEY_PRO   = "NOTES-PRO0-0000-0001";  // active, pro entitlement
static constexpr const char* KEY_REVK  = "NOTES-REVK-0000-0002";  // revoked
static constexpr const char* KEY_EXPD  = "NOTES-EXPD-0000-0003";  // expired

// ---------------------------------------------------------------------------
// MemoryStore — in-memory LicenseStore; each test gets its own instance
// ---------------------------------------------------------------------------
namespace {

class MemoryStore : public LicenseStore {
public:
    std::string data_;
    bool        has_data_ = false;

    Result<std::string> load() override {
        if (!has_data_) return Result<std::string>::ok(std::string{});
        return Result<std::string>::ok(data_);
    }
    Result<void> save(const std::string& d) override {
        data_     = d;
        has_data_ = true;
        return Result<void>::ok();
    }
    Result<void> clear() override {
        data_.clear();
        has_data_ = false;
        return Result<void>::ok();
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Live tests — all guarded by KEYLIGHT_LIVE env-var AND httplib transport
// ---------------------------------------------------------------------------

TEST_CASE("live: fetchKeyset returns non-empty map for demo tenant") {
    if (std::getenv("KEYLIGHT_LIVE") == nullptr) {
        MESSAGE("KEYLIGHT_LIVE not set — skipping live tests");
        return;
    }

#ifdef KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
    HttplibTransport transport;

    auto res = fetchKeyset(transport, BASE, TENANT);
    CHECK(res.is_ok());
    if (!res.is_ok()) {
        MESSAGE("fetchKeyset error: " << res.error().message);
        return;
    }
    const auto& keys = res.value();
    CHECK(!keys.empty());
    MESSAGE("Fetched " << keys.size() << " key(s) from demo tenant");
    for (const auto& [kid, pubkey] : keys) {
        MESSAGE("  kid=" << kid << " pubkey=" << pubkey.substr(0, 20) << "...");
    }
#else
    MESSAGE("KEYLIGHT_BUILD_HTTPLIB_TRANSPORT not set — skipping live fetchKeyset");
#endif
}

TEST_CASE("live: active pro key activates, has pro entitlement, then deactivates") {
    if (std::getenv("KEYLIGHT_LIVE") == nullptr) {
        return;
    }

#ifdef KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
    HttplibTransport transport;

    // Step 1: fetch live keyset
    auto ks_res = fetchKeyset(transport, BASE, TENANT);
    REQUIRE(ks_res.is_ok());
    auto trusted_keys = ks_res.value();
    REQUIRE(!trusted_keys.empty());

    // Step 2: build client with real clock (no injected clock)
    Config cfg;
    cfg.apiBaseUrl   = BASE;
    cfg.tenantId     = TENANT;
    cfg.productId    = PRODUCT;
    cfg.keyPrefix    = "NOTES";
    cfg.trustedKeys  = trusted_keys;

    MemoryStore store;
    Client client(cfg, transport, store);

    // Step 3: activate the active pro key
    auto act_res = client.activate(KEY_PRO);
    CHECK(act_res.is_ok());
    if (!act_res.is_ok()) {
        MESSAGE("activate error: " << act_res.error().message);
        return;
    }
    MESSAGE("activate state: " << static_cast<int>(act_res.value()));
    CHECK(act_res.value() == State::Licensed);
    CHECK(client.hasEntitlement("pro"));

    // Step 4: deactivate to free the demo seat (IMPORTANT cleanup)
    auto deact_res = client.deactivate();
    CHECK(deact_res.is_ok());
    CHECK(client.state() == State::Invalid);
    MESSAGE("deactivate: seat released");
#else
    MESSAGE("KEYLIGHT_BUILD_HTTPLIB_TRANSPORT not set — skipping live pro-key test");
#endif
}

TEST_CASE("live: revoked key does not activate") {
    if (std::getenv("KEYLIGHT_LIVE") == nullptr) {
        return;
    }

#ifdef KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
    HttplibTransport transport;

    auto ks_res = fetchKeyset(transport, BASE, TENANT);
    REQUIRE(ks_res.is_ok());
    auto trusted_keys = ks_res.value();

    Config cfg;
    cfg.apiBaseUrl   = BASE;
    cfg.tenantId     = TENANT;
    cfg.productId    = PRODUCT;
    cfg.keyPrefix    = "NOTES";
    cfg.trustedKeys  = trusted_keys;

    MemoryStore store;
    Client client(cfg, transport, store);

    auto act_res = client.activate(KEY_REVK);
    // Revoked key: either returns ok with non-Licensed state, or err
    // The key point is state must NOT be Licensed
    State s = client.state();
    MESSAGE("revoked key state: " << static_cast<int>(s));
    CHECK(s != State::Licensed);
#else
    MESSAGE("KEYLIGHT_BUILD_HTTPLIB_TRANSPORT not set — skipping live revoked-key test");
#endif
}

TEST_CASE("live: expired key yields no pro entitlement") {
    if (std::getenv("KEYLIGHT_LIVE") == nullptr) {
        return;
    }

#ifdef KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
    HttplibTransport transport;

    auto ks_res = fetchKeyset(transport, BASE, TENANT);
    REQUIRE(ks_res.is_ok());
    auto trusted_keys = ks_res.value();

    Config cfg;
    cfg.apiBaseUrl   = BASE;
    cfg.tenantId     = TENANT;
    cfg.productId    = PRODUCT;
    cfg.keyPrefix    = "NOTES";
    cfg.trustedKeys  = trusted_keys;

    MemoryStore store;
    Client client(cfg, transport, store);

    // Activate the expired key — may succeed or fail at the transport level
    // but the pro entitlement must not be granted
    client.activate(KEY_EXPD);
    State s = client.state();
    MESSAGE("expired key state: " << static_cast<int>(s));
    CHECK(!client.hasEntitlement("pro"));
#else
    MESSAGE("KEYLIGHT_BUILD_HTTPLIB_TRANSPORT not set — skipping live expired-key test");
#endif
}
