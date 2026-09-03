// tests/test_client.cpp — TDD for keylight::Client state machine
// Step 1: written before client.hpp exists (intentionally fails until Step 3).

#include "doctest.h"
#include "keylight/client.hpp"
#include "keylight/config.hpp"
#include "keylight/store.hpp"
#include "keylight/transport.hpp"
#include "keylight/json.hpp"
#include <atomic>
#include <chrono>
#include <string>
#include <map>
#include <thread>
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

    // After each request(), the request headers are stored here.
    std::map<std::string, std::string> last_request_headers;

    Result<HttpResponse> request(
        const std::string&,
        const std::string&,
        const std::map<std::string, std::string>& headers,
        const std::string& body) override
    {
        last_request_body    = body;
        last_request_headers = headers;
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

// A validate response representing a server-side revoke/expire caught
// on the next launch: signature verifies (kid "k1", same conformance
// vector's public key), and the lease's own expiresAt has NOT yet passed,
// but the server-assigned status is no longer "active" — this is the
// "expired-status" vector from tests/fixtures/vectors.json. resolve_from_lease_
// maps any trusted, non-"active" status to State::Expired, which is the
// "deny" outcome the revocation-parity design calls for.
static const std::string REVOKED_VALIDATE_RESPONSE = R"({
  "valid": false,
  "lease": {
    "kid": "k1",
    "licenseKeyHash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "instanceId": "00000000-0000-4000-8000-000000000001",
    "issuedAt": 1781076246,
    "expiresAt": 1781681046,
    "status": "expired",
    "signature": "FO4clyjAnjZpPaV/eecMvjbUz28fslWVwIpAPwTvnGvCWW5mveKpjsAxDgxUP6PdPRIPblAMYrS/0d0vnc+FDA==",
    "entitlements": []
  }
})";

// A REAL revoke / deactivated-instance response, exactly as the production
// worker sends it: HTTP 422, `{"error": "..."}`, NO `lease` field, NO `valid`
// field at all. Before the 422-decodable fix, Client::validate() /
// validate_and_reconcile_() short-circuited on `resp.status != 200` and
// treated this as a transient failure, silently keeping the old cached
// "active" lease — the revoke was never enforced.
static const std::string REAL_REVOKE_422_RESPONSE =
    R"({"error":"Instance not found or deactivated"})";

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
// Device telemetry — cpu_cores / memory ride the SAME payload as sdk/platform
// ---------------------------------------------------------------------------
static bool body_has_legal_bucket(const std::string& body,
                                  const std::string& field,
                                  const std::vector<std::string>& allowed) {
    const std::string needle = "\"" + field + "\":\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return false;
    auto start = pos + needle.size();
    auto end   = body.find('"', start);
    if (end == std::string::npos) return false;
    const std::string value = body.substr(start, end - start);
    for (const auto& a : allowed) if (a == value) return true;
    return false;
}

TEST_CASE("Client: activate() sends cpu_cores and memory buckets") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;

    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    const std::string& body = transport.last_request_body;
    // Existing telemetry must still be there (additive-only guard).
    CHECK(body.find("\"platform\"")    != std::string::npos);
    CHECK(body.find("\"sdk\"")         != std::string::npos);
    CHECK(body.find("\"sdk_version\"") != std::string::npos);

    CHECK(body_has_legal_bucket(body, "cpu_cores",
                                {"1-2", "3-4", "5-8", "9-16", "17+"}));
    CHECK(body_has_legal_bucket(body, "memory",
                                {"<4GB", "4-8GB", "8-16GB",
                                 "16-32GB", "32-64GB", "64GB+"}));
}

TEST_CASE("Client: validate() sends cpu_cores and memory buckets") {
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    transport.next_body = VALIDATE_RESPONSE;
    transport.last_request_body.clear();
    REQUIRE(client.validate().is_ok());

    const std::string& body = transport.last_request_body;
    CHECK(body_has_legal_bucket(body, "cpu_cores",
                                {"1-2", "3-4", "5-8", "9-16", "17+"}));
    CHECK(body_has_legal_bucket(body, "memory",
                                {"<4GB", "4-8GB", "8-16GB",
                                 "16-32GB", "32-64GB", "64GB+"}));
}

TEST_CASE("Client: raw core count and raw byte count never reach the wire") {
    // Fingerprinting guard: only the coarse bucket may cross the wire.
    auto cfg = make_config();
    FakeTransport  transport;
    MemoryStore    store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    const std::string& body = transport.last_request_body;
    const uint64_t raw_bytes = keylight::detail::detect_physical_memory_bytes();
    if (raw_bytes != 0) {
        CHECK(body.find(std::to_string(raw_bytes)) == std::string::npos);
    }
    // A bucket string is never a bare number, so a numeric cpu_cores value
    // would have to appear as "cpu_cores":<digits> — assert it does not.
    CHECK(body.find("\"cpu_cores\":\"") != std::string::npos);
    CHECK(body.find("\"memory\":\"")    != std::string::npos);
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

// A FakeTransport that counts calls atomically (safe for concurrent access).
// Defined here (rather than only in the E3 section below) so E2 tests can
// assert the network WAS called (as opposed to FailingTransport, which
// proves failure-handling, or FakeTransport, which doesn't count calls).
class CountingTransport : public Transport {
public:
    std::atomic<int> call_count{0};
    int              next_status = 200;
    std::string      next_body;

    Result<HttpResponse> request(
        const std::string&,
        const std::string&,
        const std::map<std::string, std::string>&,
        const std::string&) override
    {
        ++call_count;
        HttpResponse r;
        r.status = next_status;
        r.body   = next_body;
        return Result<HttpResponse>::ok(r);
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

TEST_CASE("E2: checkOnLaunch with cached valid lease → still validates online (no debounce skip)") {
    // Per the cross-SDK revocation/offline-bound parity design (2026-07-08),
    // checkOnLaunch must ALWAYS perform a server round-trip so a dashboard
    // revoke lands on the next launch — it must NOT delegate to
    // refreshIfNeeded()'s staleness gate (5min debounce / 6h stale / 24h
    // near-expiry), which would skip the network call entirely for a fresh
    // cached lease and let a revoke lag for hours.
    auto cfg = make_config();
    CountingTransport transport;
    MemoryStore       store;

    // now = VALID_ACTIVE_NOW; lease expires at 1781681046 (well in the future)
    // last_validated_online = now (freshly validated — this is exactly the
    // case the OLD refreshIfNeeded-delegating checkOnLaunch would debounce).
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    transport.next_status = 200;
    transport.next_body   = VALIDATE_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Licensed);

    // Crucially: the transport MUST have been called exactly once, even
    // though the cached lease was validated online "just now".
    CHECK(transport.call_count == 1);
}

TEST_CASE("E2: checkOnLaunch catches a dashboard revoke even with a fresh cached lease") {
    // Server-side revoke/expire represented as a trusted, signed lease whose
    // status is no longer "active" (conformance vector "expired-status" from
    // tests/fixtures/vectors.json — signature verifies, but the lease's own
    // expiresAt has not yet passed; the server-side status is what changed).
    auto cfg = make_config();
    CountingTransport transport;
    MemoryStore       store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    transport.next_status = 200;
    transport.next_body   = REVOKED_VALIDATE_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.state() == State::Licensed);

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Expired);
    CHECK(client.state() == State::Expired);

    // The revoke must be caught on THIS launch, not several hours later.
    CHECK(transport.call_count == 1);
}

TEST_CASE("E4: checkOnLaunch enforces a REAL revoke — HTTP 422, no lease, no 'valid' field") {
    // This is the exact shape the production worker sends for a dashboard
    // revoke / deactivated instance: HTTP 422, `{"error": "..."}`, no lease,
    // no `valid` field at all (unlike REVOKED_VALIDATE_RESPONSE above, which
    // is a synthetic 200 + trusted "expired"-status lease). Before the fix,
    // `validate()`/`validate_and_reconcile_` short-circuited on
    // `resp.status != 200` BEFORE parsing the body, so this response was
    // swallowed as "transient" and the cached "active" lease was kept —
    // the revoke was never enforced and state() stayed Licensed.
    auto cfg = make_config();
    CountingTransport transport;
    MemoryStore       store;

    // Seed a cached, currently-valid "active" lease (as if activated earlier).
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    transport.next_status = 422;
    transport.next_body   = REAL_REVOKE_422_RESPONSE;

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.state() == State::Licensed);

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Invalid);
    CHECK(client.state() == State::Invalid);
    CHECK(transport.call_count == 1);

    // The stale lease must actually be cleared from the store — not just
    // masked in memory — so a later relaunch doesn't resurrect Licensed from
    // a stored blob that still contains the revoked lease.
    Client relaunched(cfg, transport, store,
                       []{ return VALID_ACTIVE_NOW; });
    CHECK(relaunched.state() == State::Invalid);
}

TEST_CASE("E4: checkOnLaunch with a 422 + expired lease keeps the lease (Expired, not wiped)") {
    // Distinguish the two 422 shapes: when the server DOES send a lease
    // (status "expired"/"fallback" — the license itself is stale, not
    // revoked), that lease must be stored/kept, not discarded, so state()
    // resolves to Expired off real data.
    auto cfg = make_config();
    CountingTransport transport;
    MemoryStore       store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    transport.next_status = 422;
    transport.next_body   = REVOKED_VALIDATE_RESPONSE; // valid:false + trusted "expired"-status lease

    Client client(cfg, transport, store,
                  []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.state() == State::Licensed);

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Expired);
    CHECK(client.state() == State::Expired);
    CHECK(transport.call_count == 1);
}

TEST_CASE("E2: checkOnLaunch with a transient failure keeps access within the offline cap") {
    auto cfg = make_config(); // maxOfflineDays default (15)
    FailingTransport transport;
    MemoryStore      store;

    // Validated online 10 days ago: past the OLD default cap (7 days) but
    // within the NEW default cap (15 days) — this is the case that
    // specifically exercises the 7→15 bump, not just "grace exists".
    int64_t now      = 1781681046LL - 3600;  // 1h before the lease's own expiry
    int64_t last_lvo = now - 10LL * 86400;
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW, 1781681046LL, last_lvo);

    Client client(cfg, transport, store, [now]{ return now; });
    REQUIRE(client.state() == State::Licensed);

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Licensed);
    CHECK(client.state() == State::Licensed);

    // The network WAS attempted (checkOnLaunch always validates); it just
    // failed transiently and must not deny access within the cap.
    CHECK(transport.call_count == 1);
}

TEST_CASE("E2: checkOnLaunch denies once offline past maxOfflineDays (15)") {
    auto cfg = make_config(); // default maxOfflineDays == 15
    FailingTransport transport;
    MemoryStore      store;

    // now stays before the lease's own expiresAt (1781681046) so this
    // exercises the offline-cap path, not a raw lease-expiry downgrade.
    int64_t now      = 1781681046LL - 3600;       // 1h before natural expiry
    int64_t last_lvo = now - 16LL * 86400;         // last validated 16 days ago

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW, 1781681046LL, last_lvo);

    Client client(cfg, transport, store, [now]{ return now; });
    REQUIRE(client.state() == State::Licensed);

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Expired);
    CHECK(client.state() == State::Expired);
}

TEST_CASE("E2: checkOnLaunch with maxOfflineDays disabled never denies for age") {
    auto cfg = make_config();
    cfg.maxOfflineDays = 0; // 0 == cap disabled (uncapped offline), per existing convention
    FailingTransport transport;
    MemoryStore      store;

    // Validated online ~1000 days ago — would fail any normal cap, but the
    // cap is disabled so age alone must never cause a denial.
    int64_t now      = 1781681046LL - 3600; // still before the lease's own expiresAt
    int64_t last_lvo = now - 1000LL * 86400;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW, 1781681046LL, last_lvo);

    Client client(cfg, transport, store, [now]{ return now; });
    REQUIRE(client.state() == State::Licensed);

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Licensed);
    CHECK(client.state() == State::Licensed);
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

// ---------------------------------------------------------------------------
// E3: opt-in background auto-validation
// ---------------------------------------------------------------------------
// CountingTransport is defined in the E2 helpers section above.

TEST_CASE("E3: startAutoValidation + stopAutoValidation cleanly joins") {
    // No background thread without a stored license, so seed one first.
    // We seed so that refreshIfNeeded fires (last_validated > stale threshold
    // triggers a validate call on the thread). The transport responds with a
    // stale-refresh-triggering clock: make the interval very short via the
    // injected interval parameter (1 ms).
    auto cfg = make_config();
    cfg.autoValidationIntervalMs = 1; // 1 ms — test seam for fast iteration
    CountingTransport transport;
    MemoryStore       store;

    // Seed with a stale last_validated_online (8h ago) so refreshIfNeeded
    // will attempt a network call when the thread ticks.
    int64_t now = VALID_ACTIVE_NOW + 8 * 3600;
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW, 1781681046LL, VALID_ACTIVE_NOW);
    transport.next_status = 200;
    transport.next_body   = VALIDATE_RESPONSE;

    Client client(cfg, transport, store, [now]{ return now; });

    // start → stop must return without hanging
    client.startAutoValidation();
    client.stopAutoValidation(); // must join cleanly (no hang)

    // Idempotent: stopping again is a no-op
    client.stopAutoValidation();
}

TEST_CASE("E3: no background thread without startAutoValidation") {
    // Construct and immediately destroy — no thread must have been started.
    // If a thread were running this would likely hang or crash on destruction
    // of a joinable thread; the test itself serves as the check.
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    {
        Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
        // Do NOT call startAutoValidation.
        // Scope exit: destructor must not hang or std::terminate.
    }
    CHECK(true); // reached here — no hang
}

TEST_CASE("E3: destructor stops running auto-validation cleanly") {
    auto cfg = make_config();
    cfg.autoValidationIntervalMs = 1; // fast tick
    FakeTransport transport;
    MemoryStore   store;

    transport.next_status = 200;
    transport.next_body   = VALIDATE_RESPONSE;

    {
        Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
        client.startAutoValidation();
        // Go out of scope without calling stopAutoValidation.
        // Destructor must join the thread cleanly.
    }
    CHECK(true); // reached here — destructor did not hang/crash
}

TEST_CASE("E3: startAutoValidation is idempotent (double-start safe)") {
    auto cfg = make_config();
    cfg.autoValidationIntervalMs = 1;
    FakeTransport transport;
    MemoryStore   store;

    transport.next_status = 200;
    transport.next_body   = VALIDATE_RESPONSE;

    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });

    client.startAutoValidation();
    client.startAutoValidation(); // second call must be a no-op, not spawn a second thread
    client.stopAutoValidation();
}

TEST_CASE("E3: worker invokes refreshIfNeeded at least once when stale") {
    // Set up a stale store so that refreshIfNeeded() will fire a network call.
    // Then start auto-validation, sleep briefly, stop, and check the transport
    // was called.
    auto cfg = make_config();
    cfg.autoValidationIntervalMs = 5; // 5 ms — fast enough for the test
    CountingTransport transport;
    MemoryStore       store;

    // Clock is 8h past last_validated_online → refresh will be triggered
    int64_t now = VALID_ACTIVE_NOW + 8 * 3600;
    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW, 1781681046LL, VALID_ACTIVE_NOW);
    transport.next_status = 200;
    transport.next_body   = VALIDATE_RESPONSE;

    Client client(cfg, transport, store, [now]{ return now; });

    client.startAutoValidation();

    // Give the worker thread time to tick at least once (100 ms >> 5 ms interval)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    client.stopAutoValidation();

    // The worker must have called refreshIfNeeded at least once → transport hit
    CHECK(transport.call_count >= 1);
}

TEST_CASE("Client: deactivate sends license_key alongside instance_id") {
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    transport.next_body = R"({"deactivated":true})";
    REQUIRE(client.deactivate().is_ok());

    // DeactivateBodySchema requires BOTH fields. A body missing license_key is
    // rejected by zod before any mutation runs, so the seat is never released.
    CHECK(transport.last_request_body.find("\"license_key\"")        != std::string::npos);
    CHECK(transport.last_request_body.find("XXXX-YYYY-ZZZZ-0001")    != std::string::npos);
    CHECK(transport.last_request_body.find("\"instance_id\"")        != std::string::npos);
}

TEST_CASE("Client: deactivate surfaces a server rejection but still clears the store") {
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    transport.next_status = 422;
    transport.next_body   = R"({"error":"Instance not found or deactivated"})";

    auto dr = client.deactivate();
    CHECK(!dr.is_ok());
    CHECK(dr.error().message.find("Instance not found") != std::string::npos);

    // The local half is cleared regardless — the app must not stay "licensed"
    // on a machine the user asked to release.
    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().empty());
    CHECK(client.state() == State::Invalid);
}

TEST_CASE("Client: activate sends os_version and arch when detectable") {
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    const std::string& body = transport.last_request_body;

    // Both fields are omitted entirely when the platform cannot report them —
    // the SDK never guesses — so assert presence only when detection worked.
    if (!keylight::detail::detect_os_version().empty()) {
        CHECK(body.find("\"os_version\"") != std::string::npos);
        CHECK(body.find(keylight::detail::detect_os_version()) != std::string::npos);
    }
    if (std::string(keylight::detail::current_arch()).empty() == false) {
        CHECK(body.find("\"arch\"") != std::string::npos);
        CHECK(body.find(keylight::detail::current_arch()) != std::string::npos);
    }
    // device_class is NEVER sent from this SDK: the server derives it.
    CHECK(body.find("\"device_class\"") == std::string::npos);
}

TEST_CASE("Client: activate sends a real instance_name, not the literal 'device'") {
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    CHECK(transport.last_request_body.find("\"instance_name\"") != std::string::npos);

    // On any machine that can report a hostname the field must carry it.
    // "device" survives only as the fallback when the read fails, so this
    // asserts against the machine's actual name rather than a fixed string.
    std::string host = keylight::detail::detect_machine_name();
    if (!host.empty()) {
        CHECK(transport.last_request_body.find(host) != std::string::npos);
    }
}

TEST_CASE("Client: every request carries a unique X-Keylight-Request-Id") {
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });

    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    auto it = transport.last_request_headers.find("X-Keylight-Request-Id");
    REQUIRE(it != transport.last_request_headers.end());

    const std::string first = it->second;
    CHECK(first.size() == 32);
    CHECK(first.find_first_not_of("0123456789abcdef") == std::string::npos);

    // A correlation id that repeats correlates nothing.
    transport.next_body = VALIDATE_RESPONSE;
    REQUIRE(client.validate().is_ok());
    CHECK(transport.last_request_headers["X-Keylight-Request-Id"] != first);
}

TEST_CASE("Client: activate surfaces the worker's error message") {
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });

    transport.next_status = 404;
    transport.next_body   = R"({"error":"License key not found"})";

    auto r = client.activate("XXXX-YYYY-ZZZZ-0001");
    REQUIRE(!r.is_ok());
    // This string goes straight into the integrator's UI. "activate HTTP 404"
    // tells the customer nothing they can act on.
    CHECK(r.error().message == "License key not found");
}

TEST_CASE("Client: activate falls back to the status line on an unparseable body") {
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });

    transport.next_status = 502;
    transport.next_body   = "<html>gateway</html>";

    auto r = client.activate("XXXX-YYYY-ZZZZ-0001");
    REQUIRE(!r.is_ok());
    CHECK(r.error().message == "activate HTTP 502");
}
