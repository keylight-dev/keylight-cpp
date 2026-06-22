// tests/test_client.cpp — TDD for keylight::Client state machine
// Step 1: written before client.hpp exists (intentionally fails until Step 3).

#include "doctest.h"
#include "keylight/client.hpp"
#include "keylight/config.hpp"
#include "keylight/store.hpp"
#include "keylight/transport.hpp"
#include "keylight/json.hpp"
#include <atomic>
#include <string>
#include <map>
#include <vector>

using namespace keylight;

// ---------------------------------------------------------------------------
// FakeTransport — returns a canned HTTP response; optionally captures the body
// ---------------------------------------------------------------------------
class FakeTransport : public Transport {
public:
    int         next_status = 200;
    std::string next_body;

    // After each request(), the request body is stored here.
    std::string last_request_body;

    Result<HttpResponse> request(
        const std::string&,
        const std::string&,
        const std::map<std::string, std::string>&,
        const std::string& body) override
    {
        last_request_body = body;
        HttpResponse r;
        r.status = next_status;
        r.body   = next_body;
        return Result<HttpResponse>::ok(r);
    }
};

// ---------------------------------------------------------------------------
// MemoryStore — in-memory LicenseStore (no disk I/O)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Conformance vector[0] "valid-active" constants
//   kid:       k1
//   pubkey:    8QkyJGwaIqAuN0jdsCnBtv3D9fylv4PHqCVufx7xje0=
//   now:       1781076256  (lease expiresAt=1781681046; active at this now)
//   lease JSON embedded in activate response
// ---------------------------------------------------------------------------
static const char* VALID_ACTIVE_PUBKEY = "8QkyJGwaIqAuN0jdsCnBtv3D9fylv4PHqCVufx7xje0=";
static const int64_t VALID_ACTIVE_NOW  = 1781076256LL;

// The activate response wraps the lease under "lease" plus activated=true.
static const std::string ACTIVATE_RESPONSE = R"({
  "activated": true,
  "instance_id": "inst-abc",
  "license_expires_at": 1781681046,
  "lease": {
    "kid": "k1",
    "licenseKeyHash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "instanceId": "00000000-0000-4000-8000-000000000001",
    "issuedAt": 1781076246,
    "expiresAt": 1781681046,
    "status": "active",
    "signature": "SUrg6IHJBkO4PB80hiwXhkCFgHTxp5Ao6i9fRnajIH3ws3E+F444xYUQL9UyJYMz4cC+6f8YDMfwrxIv1mQeBw==",
    "entitlements": ["pro"]
  }
})";

// A response whose lease has an unknown kid → verify fails → Invalid
static const std::string INVALID_KID_RESPONSE = R"({
  "activated": true,
  "instance_id": "inst-xyz",
  "license_expires_at": 1781681046,
  "lease": {
    "kid": "k9",
    "licenseKeyHash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "instanceId": "00000000-0000-4000-8000-000000000001",
    "issuedAt": 1781076246,
    "expiresAt": 1781681046,
    "status": "active",
    "signature": "SUrg6IHJBkO4PB80hiwXhkCFgHTxp5Ao6i9fRnajIH3ws3E+F444xYUQL9UyJYMz4cC+6f8YDMfwrxIv1mQeBw==",
    "entitlements": ["pro"]
  }
})";

// Validate response with the same valid-active lease
static const std::string VALIDATE_RESPONSE = R"({
  "valid": true,
  "license_expires_at": 1781681046,
  "lease": {
    "kid": "k1",
    "licenseKeyHash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "instanceId": "00000000-0000-4000-8000-000000000001",
    "issuedAt": 1781076246,
    "expiresAt": 1781681046,
    "status": "active",
    "signature": "SUrg6IHJBkO4PB80hiwXhkCFgHTxp5Ao6i9fRnajIH3ws3E+F444xYUQL9UyJYMz4cC+6f8YDMfwrxIv1mQeBw==",
    "entitlements": ["pro"]
  }
})";

// ---------------------------------------------------------------------------
// Helpers to build test Client
// ---------------------------------------------------------------------------
static Config make_config() {
    Config cfg;
    cfg.tenantId  = "tenant1";
    cfg.productId = "prod1";
    cfg.trustedKeys["k1"] = VALID_ACTIVE_PUBKEY;
    return cfg;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("Client: activate with valid-active lease → State::Licensed") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;

    // Pin clock to the conformance vector's 'now' so the lease is not expired
    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    auto result = client.activate("XXXX-YYYY-ZZZZ-0001");
    REQUIRE(result.is_ok());
    CHECK(result.value() == State::Licensed);
    CHECK(client.state() == State::Licensed);
}

TEST_CASE("Client: hasEntitlement reflects activated lease") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    auto result = client.activate("XXXX-YYYY-ZZZZ-0001");
    REQUIRE(result.is_ok());
    CHECK(result.value() == State::Licensed);

    CHECK(client.hasEntitlement("pro")   == true);
    CHECK(client.hasEntitlement("admin") == false);
}

TEST_CASE("Client: deactivate clears store; state becomes Invalid") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    // Activate first
    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    REQUIRE(client.state() == State::Licensed);

    // Deactivate — server returns 200 {}
    transport.next_body = R"({"deactivated":true})";
    auto dr = client.deactivate();
    REQUIRE(dr.is_ok());

    // Store must be empty
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().empty());

    // State after deactivate: no lease → Invalid
    CHECK(client.state() == State::Invalid);
}

TEST_CASE("Client: tampered/unknown-kid lease → State::Invalid") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    transport.next_status = 200;
    transport.next_body   = INVALID_KID_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    auto result = client.activate("XXXX-YYYY-ZZZZ-0001");
    REQUIRE(result.is_ok());
    CHECK(result.value() == State::Invalid);
    CHECK(client.state() == State::Invalid);
}

TEST_CASE("Client: validate updates state") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    // Pre-populate store with the valid-active lease so validate has
    // something to update; also need instance_id. We'll store a minimal
    // blob that the client will re-read.
    // Just activate first, then validate.
    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    REQUIRE(client.state() == State::Licensed);

    // Now validate — server returns the same good lease
    transport.next_body = VALIDATE_RESPONSE;
    auto vr = client.validate();
    REQUIRE(vr.is_ok());
    CHECK(vr.value() == State::Licensed);
}

TEST_CASE("Client: cachedLicenseExpiresAt after activate") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    auto exp = client.cachedLicenseExpiresAt();
    REQUIRE(exp.has_value());
    CHECK(exp.value() == 1781681046LL);
}

TEST_CASE("Client: activateAsync returns same result as activate") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    auto fut = client.activateAsync("XXXX-YYYY-ZZZZ-0001");
    auto result = fut.get();
    REQUIRE(result.is_ok());
    CHECK(result.value() == State::Licensed);
}

TEST_CASE("Client: state() is Invalid before any activation") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    CHECK(client.state() == State::Invalid);
    CHECK(client.hasEntitlement("pro") == false);
}

TEST_CASE("Client: validate() sends license_key in request body") {
    // Regression test: Worker's ValidateBodySchema requires both license_key
    // and instance_id.  Before this fix validate() only sent instance_id,
    // causing 4xx from the real API and silent stale state.
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    // Activate so the client has a stored license_key and instance_id
    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    const std::string TEST_KEY = "XXXX-YYYY-ZZZZ-0001";
    REQUIRE(client.activate(TEST_KEY).is_ok());
    REQUIRE(client.state() == State::Licensed);

    // Now validate — capture the body sent to the transport
    transport.next_body = VALIDATE_RESPONSE;
    transport.last_request_body.clear();

    auto vr = client.validate();
    REQUIRE(vr.is_ok());
    CHECK(vr.value() == State::Licensed);

    // The captured body must contain "license_key":"XXXX-YYYY-ZZZZ-0001"
    const std::string& body = transport.last_request_body;
    CHECK(body.find("\"license_key\"") != std::string::npos);
    CHECK(body.find(TEST_KEY) != std::string::npos);

    // It must also still contain instance_id (regression guard)
    CHECK(body.find("\"instance_id\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// E2 helpers
// ---------------------------------------------------------------------------

// A transport that always fails with a network error — used to prove no
// network call is made (or to simulate offline).
class FailingTransport : public Transport {
public:
    mutable int call_count = 0;
    Result<HttpResponse> request(
        const std::string&,
        const std::string&,
        const std::map<std::string, std::string>&,
        const std::string&) override
    {
        ++call_count;
        return Result<HttpResponse>::err({ErrorCode::Network, "simulated network failure"});
    }
};

// Persist a valid-active lease blob directly into the store, mimicking what
// activate() would have written (format: {"lease":{...},"expiresAt":N,...}).
// Also stores lastValidatedOnline (for offline-grace tests).
static void seed_store_with_valid_lease(MemoryStore& store,
                                        int64_t      now,
                                        int64_t      expires_at = 1781681046LL,
                                        int64_t      last_validated_online = 0)
{
    // Use same lease as ACTIVATE_RESPONSE
    std::string blob = R"({"lease":{)"
        R"("kid":"k1",)"
        R"("licenseKeyHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
        R"("instanceId":"00000000-0000-4000-8000-000000000001",)"
        R"("issuedAt":1781076246,)"
        "\"expiresAt\":" + std::to_string(expires_at) + R"(,)"
        R"("status":"active",)"
        R"("signature":"SUrg6IHJBkO4PB80hiwXhkCFgHTxp5Ao6i9fRnajIH3ws3E+F444xYUQL9UyJYMz4cC+6f8YDMfwrxIv1mQeBw==",)"
        R"("entitlements":["pro"]})"
        ",\"expiresAt\":" + std::to_string(expires_at) +
        ",\"instanceId\":\"inst-abc\""
        ",\"licenseKey\":\"XXXX-YYYY-ZZZZ-0001\"";

    int64_t lvo = (last_validated_online == 0) ? now : last_validated_online;
    blob += ",\"lastValidatedOnline\":" + std::to_string(lvo);
    blob += "}";

    store.save(blob);
}

// ---------------------------------------------------------------------------
// E2 TEST CASES
// ---------------------------------------------------------------------------

TEST_CASE("E2: checkOnLaunch with cached valid lease → Licensed, NO network call") {
    auto cfg = make_config();
    FailingTransport transport;
    MemoryStore      store;

    // now = VALID_ACTIVE_NOW; lease expires at 1781681046 (well in the future)
    // last_validated_online = now (freshly validated → debounce will skip refresh)
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    // Re-create the client so checkOnLaunch can load from store
    // (the seeded store is already loaded on construction via refresh_state_from_store_)
    // But we need to call checkOnLaunch explicitly.
    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Licensed);

    // Crucially: FailingTransport must NOT have been called
    // (the lease is fresh — debounce should prevent the network call).
    CHECK(transport.call_count == 0);
}

TEST_CASE("E2: checkOnLaunch with expired-beyond-grace lease → Expired") {
    auto cfg = make_config();
    FailingTransport transport; // network unavailable (past grace, no recovery)
    MemoryStore   store;

    // The valid-active conformance lease has expiresAt=1781681046.
    // We set now = 1781681046 + 1 (1 second past expiry) so the signature
    // still verifies (lease is authentic), but the lease is expired at 'now'.
    // The lastValidatedOnline is set to VALID_ACTIVE_NOW so the offline grace
    // (7d from last online validation) is NOT exceeded (now - lvo ≈ 7 days).
    // Under these conditions the lease verifies as trusted but is expired → Expired.
    int64_t lease_expires_at = 1781681046LL;
    int64_t now = lease_expires_at + 1; // 1s after expiry

    // Seed the store with the original (unmodified, valid-signature) lease.
    // lastValidatedOnline = VALID_ACTIVE_NOW (≈ 7 days before now)
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW, lease_expires_at, VALID_ACTIVE_NOW);

    Client client(cfg, transport, store, [now]{ return now; });

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    // The lease is trusted but expired → State::Expired
    CHECK(r.value() == State::Expired);
}

TEST_CASE("E2: refreshIfNeeded within offline grace keeps Licensed on network failure") {
    auto cfg = make_config();
    FailingTransport transport;
    MemoryStore      store;

    // last_validated_online = 8 hours ago (past debounce=5min, past stale=6h → refresh triggered)
    // but within maxOfflineDays=7 days grace window
    int64_t now = VALID_ACTIVE_NOW + 8 * 3600; // 8 hours later
    int64_t last_lvo = VALID_ACTIVE_NOW;        // validated at t=0, 8h ago

    // Lease expires at 1781681046 which is still in the future at 'now'
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW, 1781681046LL, last_lvo);

    Client client(cfg, transport, store, [now]{ return now; });

    // State from store (loaded on construction) should be Licensed
    REQUIRE(client.state() == State::Licensed);

    // refreshIfNeeded → network fails → but within grace window → stays Licensed
    auto r = client.refreshIfNeeded();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Licensed);

    // Transport WAS called (refresh was attempted, it failed gracefully)
    CHECK(transport.call_count > 0);

    // State still Licensed (grace)
    CHECK(client.state() == State::Licensed);
}

TEST_CASE("E2: on('change', cb) fires when state transitions") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    std::vector<State> received;
    auto sub = client.on("change", [&](State s) {
        received.push_back(s);
    });

    // Before activation: no transition has happened
    CHECK(received.empty());

    // Activate → state transitions Invalid → Licensed
    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    REQUIRE(received.size() == 1);
    CHECK(received[0] == State::Licensed);

    // Deactivate → transitions Licensed → Invalid
    transport.next_body = R"({"deactivated":true})";
    REQUIRE(client.deactivate().is_ok());

    REQUIRE(received.size() == 2);
    CHECK(received[1] == State::Invalid);
}

TEST_CASE("E2: subscribe() fires on state changes") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    int call_count = 0;
    State last_state = State::Invalid;
    auto sub = client.subscribe([&](State s) {
        ++call_count;
        last_state = s;
    });

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    CHECK(call_count == 1);
    CHECK(last_state == State::Licensed);
}

TEST_CASE("E2: no spurious events on same-state transitions") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    // Pre-seed store so construction starts as Licensed
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    REQUIRE(client.state() == State::Licensed);

    int call_count = 0;
    auto sub = client.subscribe([&](State) { ++call_count; });

    // Validate returns same Licensed state — no transition event
    transport.next_status = 200;
    transport.next_body   = VALIDATE_RESPONSE;
    REQUIRE(client.validate().is_ok());
    REQUIRE(client.state() == State::Licensed);

    CHECK(call_count == 0);
}
