// tests/test_client.cpp — TDD for keylight::Client state machine
// Step 1: written before client.hpp exists (intentionally fails until Step 3).

#include "doctest.h"
#include "keylight/client.hpp"
#include "keylight/config.hpp"
#include "keylight/store.hpp"
#include "keylight/transport.hpp"
#include "keylight/json.hpp"
#include <string>
#include <map>

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
