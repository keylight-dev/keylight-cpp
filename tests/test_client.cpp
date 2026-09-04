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
#include <set>
#include <vector>
#include <stdexcept>

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

// A transport that behaves like FakeTransport (configurable status/body)
// until told to go offline, at which point every subsequent request() fails
// with a network error. This lets a single Client instance activate online
// and then "lose the network" mid-session, without constructing a second
// Client (which would re-run refresh_state_from_store_ and defeat tests that
// need to isolate apply_offline_grace_'s own deny branch from the launch
// path's construction-time bound).
class GoesOfflineTransport : public Transport {
public:
    int         next_status = 200;
    std::string next_body;
    bool        offline     = false;

    Result<HttpResponse> request(
        const std::string&,
        const std::string&,
        const std::map<std::string, std::string>&,
        const std::string&) override
    {
        if (offline) {
            return Result<HttpResponse>::err({ErrorCode::Network, "simulated network failure"});
        }
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

// Persist a trusted, "fallback"-status lease blob directly into the store —
// the same lease as FALLBACK_VALIDATE_RESPONSE, in the shape validate()
// would have written after receiving it. Used to exercise the offline
// relaunch path (refresh_state_from_store_ -> derive_state_from_verify_)
// independently of the online path (resolve_from_lease_), which
// FALLBACK_VALIDATE_RESPONSE + a live transport already covers elsewhere.
static void seed_store_with_fallback_lease(MemoryStore& store, int64_t now) {
    std::string blob = R"({"lease":{)"
        R"("kid":"k1",)"
        R"("licenseKeyHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
        R"("instanceId":"00000000-0000-4000-8000-000000000001",)"
        R"("issuedAt":1781076246,)"
        R"("expiresAt":1781681046,)"
        R"("status":"fallback",)"
        R"("signature":"H/L/1y6x6Cg11Hle+6RNFioM9N6gFWGeR9tOORsNZlcL+kinqJdtb3T5dD2Irh5Q9bH1avSUQZGQXtkqaeEVDw==",)"
        R"("entitlements":[]})"
        R"(,"expiresAt":1781681046)"
        R"(,"instanceId":"inst-abc")"
        R"(,"licenseKey":"XXXX-YYYY-ZZZZ-0001")"
        ",\"lastValidatedOnline\":" + std::to_string(now) +
        "}";

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

    // refresh_state_from_store_ now applies the offline bound directly on
    // construction (this is the behavior this task adds), so the cached
    // lease already resolves to Expired before any network call — not just
    // after checkOnLaunch's own grace check below.
    Client client(cfg, transport, store, [now]{ return now; });
    REQUIRE(client.state() == State::Expired);

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

// A signature-valid lease whose server-assigned status is "fallback": the
// server could not mint a full lease (signing-key incident, key rotation).
// Copied verbatim from the `fallback-status` conformance vector
// (tests/fixtures/vectors.json, vectors[3]) so it stays byte-identical to the
// Rust and worker suites.
static const std::string FALLBACK_VALIDATE_RESPONSE = R"({
  "valid": true,
  "lease": {
    "kid": "k1",
    "licenseKeyHash": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "instanceId": "00000000-0000-4000-8000-000000000001",
    "issuedAt": 1781076246,
    "expiresAt": 1781681046,
    "status": "fallback",
    "signature": "H/L/1y6x6Cg11Hle+6RNFioM9N6gFWGeR9tOORsNZlcL+kinqJdtb3T5dD2Irh5Q9bH1avSUQZGQXtkqaeEVDw==",
    "entitlements": []
  }
})";

TEST_CASE("Client: enum values are stable for anyone who persisted a State") {
    // Renumbering would silently reinterpret a stored integer as a different
    // state. New values are appended only.
    CHECK(static_cast<int>(State::Licensed) == 0);
    CHECK(static_cast<int>(State::Trial)    == 1);
    CHECK(static_cast<int>(State::Expired)  == 2);
    CHECK(static_cast<int>(State::Invalid)  == 3);
    CHECK(static_cast<int>(State::FreeTier) == 4);
    CHECK(static_cast<int>(State::Limited)  == 5);
}

TEST_CASE("Client: a fallback lease degrades to Limited, it does not lock the app") {
    auto cfg = make_config();
    FakeTransport transport;
    MemoryStore   store;

    transport.next_status = 200;
    transport.next_body   = FALLBACK_VALIDATE_RESPONSE;

    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
    REQUIRE(client.validate().is_ok());

    // "fallback" means the server could not sign a full lease — a key-rotation
    // or signing incident. Degrading is correct; locking a paying customer out
    // over a server-side incident is not.
    CHECK(client.state() == State::Limited);
}

TEST_CASE("Client: a fallback lease cached from a previous launch is still Limited offline") {
    // resolve_from_lease_ (the online validate() path, covered above) is not
    // the only place that decides State from a lease's "fallback" status.
    // derive_state_from_verify_ recomputes state from whatever is persisted
    // in the store on construction — the relaunch path — and must reach the
    // same answer with NO server round-trip, or a client that degraded to
    // Limited during validate() would silently re-lock to Expired the next
    // time the app starts.
    auto cfg = make_config();
    FailingTransport transport; // must not be called: state must come from cache
    MemoryStore      store;

    seed_store_with_fallback_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });

    CHECK(client.state() == State::Limited);
    CHECK(transport.call_count == 0);
}

TEST_CASE("Client: a lease older than maxOfflineDays does not survive a relaunch") {
    auto cfg = make_config();
    cfg.maxOfflineDays = 2;

    FakeTransport transport;
    MemoryStore   store;

    // First run: activate and go offline.
    {
        transport.next_status = 200;
        transport.next_body   = ACTIVATE_RESPONSE;
        Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
        REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
        REQUIRE(client.state() == State::Licensed);
    }

    // Relaunch three days later with no network. The lease itself is still
    // within its 7-day TTL, so only the offline bound can catch this.
    const int64_t three_days_later = VALID_ACTIVE_NOW + 3 * 86400;
    FailingTransport offline;
    Client relaunched(cfg, offline, store, [&]{ return three_days_later; });

    // Exactly Expired: `!= Licensed` would also pass for Invalid, Trial or
    // FreeTier, and a lease aged out of the offline bound is not the same
    // outcome as a lease that never verified.
    CHECK(relaunched.state() == State::Expired);
}

TEST_CASE("Client: a lease within maxOfflineDays survives a relaunch") {
    auto cfg = make_config();
    cfg.maxOfflineDays = 7;

    FakeTransport transport;
    MemoryStore   store;
    {
        transport.next_status = 200;
        transport.next_body   = ACTIVATE_RESPONSE;
        Client client(cfg, transport, store, []{ return VALID_ACTIVE_NOW; });
        REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    }

    const int64_t one_day_later = VALID_ACTIVE_NOW + 86400;
    FailingTransport offline;
    Client relaunched(cfg, offline, store, [&]{ return one_day_later; });

    CHECK(relaunched.state() == State::Licensed);
}

// The two relaunch tests above only exercise refresh_state_from_store_'s
// construction-time offline bound. apply_offline_grace_ (used by both
// checkOnLaunch() and refreshIfNeeded() when a network call fails mid-
// session) has its own, separate Licensed->Expired deny branch that only
// fires when state_ is Licensed on entry — and every relaunch test starts
// a NEW Client whose construction has already resolved to Expired before
// that branch could ever run. These two tests keep a single Client alive
// across a clock advance so the deny branch gets its own coverage.
TEST_CASE("Client: checkOnLaunch's offline grace denies Licensed after maxOfflineDays elapses mid-session") {
    auto cfg = make_config();
    cfg.maxOfflineDays = 2;

    int64_t now = VALID_ACTIVE_NOW;
    GoesOfflineTransport transport;
    MemoryStore          store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    // Proves the entry condition apply_offline_grace_'s deny branch requires:
    // state_ is Licensed before the clock moves or the network goes away.
    REQUIRE(client.state() == State::Licensed);

    // Advance past maxOfflineDays (2 days) but stay within the lease's own
    // ~7-day TTL, so it is the offline bound doing the work, not raw lease
    // expiry (apply_offline_grace_'s lease_raw_expired short-circuit is
    // deliberately not what this test exercises).
    now += 3 * 86400;
    transport.offline = true;

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Expired);
    CHECK(client.state() == State::Expired);
}

TEST_CASE("Client: refreshIfNeeded's offline grace denies Licensed after maxOfflineDays elapses mid-session") {
    auto cfg = make_config();
    cfg.maxOfflineDays = 2;

    int64_t now = VALID_ACTIVE_NOW;
    GoesOfflineTransport transport;
    MemoryStore          store;

    transport.next_status = 200;
    transport.next_body   = ACTIVATE_RESPONSE;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    REQUIRE(client.state() == State::Licensed);

    // Advance past both maxOfflineDays (2 days) and refreshIfNeeded's own
    // 6h staleness timer, so it actually attempts a network call, while
    // staying within the lease's own ~7-day TTL.
    now += 3 * 86400;
    transport.offline = true;

    auto r = client.refreshIfNeeded();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Expired);
    CHECK(client.state() == State::Expired);
}

// ---------------------------------------------------------------------------
// Clock-rollback guard — every read point, not just state()
// ---------------------------------------------------------------------------

TEST_CASE("Client: a rolled-back clock denies state, entitlements and checkOnLaunch alike") {
    // The guard has to answer the same way at every read point. If state()
    // fails closed while hasEntitlement() keeps returning true, the paywall
    // and the feature gate disagree and the feature gate fails OPEN — the app
    // shows "not licensed" and hands out the paid features anyway.
    auto cfg = make_config();

    int64_t          now = VALID_ACTIVE_NOW;
    FailingTransport offline;  // this whole scenario is offline
    MemoryStore      store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ return now; });

    // Honest clock: a trusted, unexpired, entitled lease.
    REQUIRE(client.state() == State::Licensed);
    REQUIRE(client.hasEntitlement("pro") == true);

    // The machine booted with a wrong clock and NTP has just corrected it
    // backward past the tolerance. Nothing about the lease changed; what
    // changed is that nothing can be aged against this clock any more.
    now = VALID_ACTIVE_NOW - 2 * 3600;

    CHECK(client.state() == State::Invalid);
    CHECK(client.hasEntitlement("pro") == false);

    // checkOnLaunch() must report what state() reports. Offline, its grace
    // window would otherwise hand back Licensed against the moved clock.
    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Invalid);
}

TEST_CASE("Client: a rolled-back clock denies refreshIfNeeded and validate too") {
    // checkOnLaunch() is not the only path that reports a State. A JUCE or
    // Unreal host polls refreshIfNeeded() on focus/resume for the whole
    // session, and its debounce and staleness short-circuits return the
    // cached state without any server contact. Guarding only the launch path
    // leaves the long-running case -- the one that lasts hours -- reporting
    // Licensed against a clock state() calls Invalid.
    auto cfg = make_config();

    int64_t          now = VALID_ACTIVE_NOW;
    FailingTransport offline;
    MemoryStore      store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ return now; });
    REQUIRE(client.state() == State::Licensed);

    // Honest clock, inside the 5-minute debounce: the short-circuit hands
    // back the cached Licensed, and that is correct here.
    auto honest = client.refreshIfNeeded();
    REQUIRE(honest.is_ok());
    REQUIRE(honest.value() == State::Licensed);

    // NTP corrects the clock backward past the tolerance. `now - anchor` is
    // now negative, so the debounce short-circuit still fires -- it just must
    // not report Licensed any more.
    now = VALID_ACTIVE_NOW - 2 * 3600;

    auto refreshed = client.refreshIfNeeded();
    REQUIRE(refreshed.is_ok());
    CHECK(refreshed.value() == State::Invalid);

    // validate()'s network-failure path keeps the existing state; offline on
    // a moved clock that state is no longer reportable either.
    auto validated = client.validate();
    REQUIRE(validated.is_ok());
    CHECK(validated.value() == State::Invalid);

    // The guard clears on its own once the clock is honest again -- it denies
    // a moved clock, it does not brick the install.
    now = VALID_ACTIVE_NOW;
    CHECK(client.refreshIfNeeded().value() == State::Licensed);
}

TEST_CASE("Client: subscribers are told what state() reports, rollback included") {
    // subscribe() is the documented way an integrator drives a paywall, and
    // the JUCE adapter caches the callback's value in an audio-thread atomic.
    // If the event channel hands out the raw state while state() applies the
    // guard, the paywall and the gate disagree -- the same fail-open split
    // the guard exists to close, just on the event path.
    //
    // A rolled-back clock also changes no RAW state, so a raw-value dedupe
    // would emit nothing at all and a cached-last-event host would sit on a
    // stale Licensed for the rest of the session.
    auto cfg = make_config();

    int64_t          now = VALID_ACTIVE_NOW;
    FailingTransport offline;
    MemoryStore      store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ return now; });
    REQUIRE(client.state() == State::Licensed);

    std::vector<State> seen;
    auto sub = client.subscribe([&](State s){ seen.push_back(s); });

    // Booting does not re-announce the state we started in.
    client.refreshIfNeeded();
    CHECK(seen.empty());

    // NTP corrects the clock backward past the tolerance. No raw state
    // changes -- the lease is untouched -- but what we may report does.
    now = VALID_ACTIVE_NOW - 2 * 3600;
    client.refreshIfNeeded();

    REQUIRE(seen.size() == 1);
    CHECK(seen.back() == State::Invalid);
    CHECK(seen.back() == client.state());   // the two channels agree

    // Polling again while still rolled back is not a new event.
    client.refreshIfNeeded();
    CHECK(seen.size() == 1);

    // The clock comes back. The guard releases, and the subscriber is told --
    // otherwise a host caching the last event stays locked forever.
    now = VALID_ACTIVE_NOW;
    client.refreshIfNeeded();

    REQUIRE(seen.size() == 2);
    CHECK(seen.back() == State::Licensed);
    CHECK(seen.back() == client.state());
}

TEST_CASE("Client: concurrent notifiers cannot deliver events out of order") {
    // Publishing the dedupe and THEN delivering lets two notifiers interleave:
    // both pass the dedupe, then deliver in whatever order the scheduler
    // picks, and because the dedupe is already satisfied no later poll ever
    // corrects it. The subscriber is left permanently contradicting state().
    //
    // Not hypothetical for the shipped adapters: JUCE's callback does a
    // blocking HTTP POST and an ed25519 verify before returning, while the
    // auto-validation thread ticks refreshIfNeeded() concurrently with any
    // dispatched validate().
    auto cfg = make_config();

    std::atomic<int64_t> now{VALID_ACTIVE_NOW};
    FailingTransport     offline;
    MemoryStore          store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ return now.load(); });
    REQUIRE(client.state() == State::Licensed);

    std::mutex         seen_mutex;
    std::vector<State> seen;

    // Set once A is provably inside its callback, so B starts from a known
    // point instead of a sleep the CI scheduler is free to ignore.
    std::atomic<bool> a_delivering{false};

    // A deliberately slow subscriber, slower on the state the FIRST notifier
    // carries, so an unordered delivery inverts: the second notifier
    // overtakes the first and the last event seen is the STALE one.
    auto sub = client.subscribe([&](State s) {
        a_delivering.store(true);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(s == State::Invalid ? 200 : 10));
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(s);
    });

    // Notifier A sees a rolled-back clock.
    now.store(VALID_ACTIVE_NOW - 2 * 3600);
    std::thread a([&]{ client.refreshIfNeeded(); });

    // Notifier B sees the clock corrected, and starts only once A is actually
    // delivering. Without this the two can both dedupe to nothing and the
    // test would pass vacuously on a heavily loaded machine.
    while (!a_delivering.load()) std::this_thread::yield();
    now.store(VALID_ACTIVE_NOW);
    std::thread b([&]{ client.refreshIfNeeded(); });

    a.join();
    b.join();

    std::lock_guard<std::mutex> lock(seen_mutex);
    REQUIRE_FALSE(seen.empty());

    // The contract that matters: whatever the interleaving, the LAST thing a
    // subscriber was told matches what state() answers. Anything else is a
    // paywall and a feature gate that disagree, with no event left to fix it.
    CHECK(seen.back() == client.state());
    CHECK(seen.back() == State::Licensed);
}

// Run `body` with a watchdog. A regression here is a DEADLOCK, and a test
// that hangs forever is worse than one that fails: it takes CI down with it
// and tells you nothing. On timeout we fail loudly and deliberately leak the
// scenario rather than tearing down objects threads are still parked in.
// Spin until `pred`, but never forever — an unbounded wait in a test hangs the
// whole suite and tells you nothing about which assertion was going to fail.
static bool spin_until(std::chrono::milliseconds limit, std::function<bool()> pred)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

static bool completes_within(std::chrono::milliseconds limit,
                             std::function<void()>     body)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread runner([done, body]{ body(); done->store(true); });

    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (done->load()) { runner.join(); return true; }
    runner.detach();   // parked on a lock; joining would hang the suite
    return false;
}

TEST_CASE("Client: a listener may hold the application's own locks") {
    // Holding an SDK lock across a listener puts that lock into the
    // APPLICATION's lock-order graph, and the cycle needs no contract
    // violation to close:
    //
    //   listener (SDK thread): holds SDK lock -> wants app_mutex
    //   app thread:            holds app_mutex -> calls refreshIfNeeded()
    //                                             -> wants SDK lock
    //
    // Taking your own mutex in a state-change handler is the most ordinary
    // thing a handler does, and README recommends refreshIfNeeded() on
    // focus/resume. So delivery must happen with no SDK lock held.
    //
    // Leaked on timeout, hence the raw new: a deadlocked run has threads
    // parked inside these objects.
    auto* store = new MemoryStore();
    auto* offline = new FailingTransport();
    auto* nowv = new std::atomic<int64_t>(VALID_ACTIVE_NOW);

    seed_store_with_valid_lease(*store, VALID_ACTIVE_NOW);

    auto  cfg    = make_config();
    auto* client = new Client(cfg, *offline, *store, [nowv]{ return nowv->load(); });
    REQUIRE(client->state() == State::Licensed);

    auto* app_mutex   = new std::mutex();
    auto* in_callback = new std::atomic<bool>(false);

    auto* sub = new Subscription(client->subscribe([app_mutex, in_callback](State) {
        in_callback->store(true);
        std::lock_guard<std::mutex> lock(*app_mutex);   // the app's own lock
    }));

    const bool ok = completes_within(std::chrono::seconds(5), [=] {
        // The SDK thread: transition while the app thread holds its mutex.
        nowv->store(VALID_ACTIVE_NOW - 2 * 3600);
        std::thread sdk([=]{ client->refreshIfNeeded(); });

        {
            std::lock_guard<std::mutex> lock(*app_mutex);
            while (!in_callback->load()) std::this_thread::yield();
            // The app thread, holding its own lock, calls into the SDK.
            client->refreshIfNeeded();
        }

        sdk.join();
    });

    CHECK(ok);

    if (ok) {
        delete sub; delete client; delete offline;
        delete store; delete nowv; delete app_mutex; delete in_callback;
    }
}

TEST_CASE("Client: a listener may call back into the Client") {
    // Delivery holds no lock, so re-entry queues rather than recursing or
    // hanging. An integrator should not have to hand off to their own thread
    // just to act on an event.
    auto* store   = new MemoryStore();
    auto* offline = new FailingTransport();
    auto* nowv    = new std::atomic<int64_t>(VALID_ACTIVE_NOW);

    seed_store_with_valid_lease(*store, VALID_ACTIVE_NOW);

    auto  cfg    = make_config();
    auto* client = new Client(cfg, *offline, *store, [nowv]{ return nowv->load(); });
    REQUIRE(client->state() == State::Licensed);

    auto* calls = new std::atomic<int>(0);
    auto* sub   = new Subscription(client->subscribe([client, calls](State) {
        if (calls->fetch_add(1) == 0) {
            (void)client->validate();          // re-enter
            (void)client->refreshIfNeeded();   // and again
        }
    }));

    const bool ok = completes_within(std::chrono::seconds(5), [=] {
        nowv->store(VALID_ACTIVE_NOW - 2 * 3600);
        client->refreshIfNeeded();
    });

    CHECK(ok);
    if (ok) {
        CHECK(calls->load() >= 1);
        delete sub; delete client; delete offline;
        delete store; delete nowv; delete calls;
    }
}

TEST_CASE("Client: a listener that throws does not kill the event channel") {
    // The delivery baton is a plain bool, not a lock_guard, so it does not
    // release on unwind. An exception escaping a listener would leave it stuck
    // and every later notify_() would take the "someone else is draining"
    // exit — the channel dead for the life of the Client while state() kept
    // moving. A caching subscriber (JUCE's audio-thread snapshot) would then
    // hold its last value forever, which on a rolled-back clock is a
    // permanent fail-open.
    auto cfg = make_config();

    std::atomic<int64_t> now{VALID_ACTIVE_NOW};
    FailingTransport     offline;
    MemoryStore          store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ return now.load(); });
    REQUIRE(client.state() == State::Licensed);

    // A listener that throws once, and a well-behaved one after it.
    std::atomic<int> thrower_calls{0};
    auto bad = client.subscribe([&](State) {
        if (thrower_calls.fetch_add(1) == 0) throw std::runtime_error("listener");
    });

    std::vector<State> seen;
    std::mutex         seen_mutex;
    auto good = client.subscribe([&](State s) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(s);
    });

    // The event the bad listener throws on must still reach the good one.
    now.store(VALID_ACTIVE_NOW - 2 * 3600);
    REQUIRE_NOTHROW(client.refreshIfNeeded());

    {
        std::lock_guard<std::mutex> lock(seen_mutex);
        REQUIRE(seen.size() == 1);
        CHECK(seen.back() == State::Invalid);
    }

    // And the channel must still be alive afterwards.
    now.store(VALID_ACTIVE_NOW);
    client.refreshIfNeeded();

    std::lock_guard<std::mutex> lock(seen_mutex);
    REQUIRE(seen.size() == 2);
    CHECK(seen.back() == State::Licensed);
    CHECK(seen.back() == client.state());
}

TEST_CASE("Client: auto-validation restarts after a listener stopped it") {
    // "Stop polling when the licence goes invalid, restart when the user
    // activates" is an ordinary integration pattern. The listener here is
    // delivered ON the auto-validation thread, so stopAutoValidation() cannot
    // join itself: it signals and returns, leaving av_thread_ joinable. A
    // finished thread is still joinable(), so a later start must REAP it
    // rather than mistake it for a live worker and no-op forever.
    auto cfg = make_config();
    cfg.autoValidationIntervalMs = 20;

    std::atomic<int64_t> now{VALID_ACTIVE_NOW};
    std::atomic<int>     ticks{0};
    FailingTransport     offline;
    MemoryStore          store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ ticks.fetch_add(1); return now.load(); });

    std::atomic<bool> self_stopped{false};
    auto sub = client.subscribe([&](State s) {
        if (s == State::Invalid && !self_stopped.exchange(true)) {
            client.stopAutoValidation();   // from the worker thread itself
        }
    });

    auto count_ticks = [&](std::chrono::milliseconds window) {
        const int before = ticks.load();
        std::this_thread::sleep_for(window);
        return ticks.load() - before;
    };

    client.startAutoValidation();
    REQUIRE(count_ticks(std::chrono::milliseconds(200)) > 0);

    // Roll the clock back: the worker's next tick raises the guard's event,
    // delivers it on its own thread, and the listener stops it from there.
    now.store(VALID_ACTIVE_NOW - 2 * 3600);
    for (int i = 0; i < 100 && !self_stopped.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(self_stopped.load());

    // Settle: stop() retires the worker rather than joining it, so a cycle
    // already in flight finishes. It must not start another.
    (void)count_ticks(std::chrono::milliseconds(150));   // let it wind down
    REQUIRE(count_ticks(std::chrono::milliseconds(200)) == 0);

    // The user activates; the host restarts polling. This must actually
    // restart — not silently no-op on an unreaped thread.
    now.store(VALID_ACTIVE_NOW);
    client.startAutoValidation();
    CHECK(count_ticks(std::chrono::milliseconds(200)) > 0);

    client.stopAutoValidation();
}

TEST_CASE("Client: delivery quiesces in agreement with state() under contention") {
    // Review caught the earlier version of this test producing exactly ONE
    // event per round — the second thread always deduped, so there was no
    // order for the channel to get wrong and the assertion was trivial. This
    // version toggles the clock from four threads so events genuinely queue
    // behind a delivery in progress, which is the path that has to stay
    // ordered.
    //
    // Honest limitation, unchanged: this does NOT reliably catch the baton
    // HAND-BACK ordering (clearing delivering_ in a second critical section).
    // That window is a few instructions wide and needs instrumentation to see.
    // What it does catch is a delivery that is dropped or left stale once the
    // notifiers have quiesced. Read notify_()'s comment for the ordering
    // argument; do not read a green run here as proof of it.
    auto cfg = make_config();

    std::atomic<int64_t> now{VALID_ACTIVE_NOW};
    FailingTransport     offline;
    MemoryStore          store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ return now.load(); });
    REQUIRE(client.state() == State::Licensed);

    std::mutex         seen_mutex;
    std::vector<State> seen;
    auto sub = client.subscribe([&](State s) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(s);
    });

    constexpr int kThreads = 4;
    constexpr int kRounds  = 5000;

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]{
            for (int r = 0; r < kRounds; ++r) {
                now.store(((r + t) % 2) ? VALID_ACTIVE_NOW - 2 * 3600
                                        : VALID_ACTIVE_NOW);
                client.refreshIfNeeded();
            }
        });
    }
    for (auto& w : workers) w.join();

    // Quiesced. Settle the clock and poll once so the final state is
    // unambiguous, then require the subscriber to have been told it.
    now.store(VALID_ACTIVE_NOW);
    client.refreshIfNeeded();

    std::lock_guard<std::mutex> lock(seen_mutex);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.back() == State::Licensed);
    CHECK(seen.back() == client.state());
}

TEST_CASE("Client: a start racing a stop leaves exactly one worker") {
    // Under the previous design both start and stop released av_mutex_ to
    // join, with the thread moved out. A start walking into that window saw
    // "no worker", skipped the reap and spawned a SECOND one while the first
    // had not observed the stop: two pollers for the session, plus a hang in
    // the stopper's join(). The epoch model has no such window — neither call
    // releases the lock mid-body — and this is the regression guard.
    //
    // Asserted by counting the DISTINCT threads that call the clock, not by
    // watching a tick counter go quiet. Under the new contract stop() retires
    // rather than joins, so "quiet" is inherently timing-dependent and made
    // this test flaky; "how many workers are alive" is not.
    auto  cfg = make_config();
    cfg.autoValidationIntervalMs = 10;

    // Heap-allocated and leaked on timeout: a hung run has threads parked
    // inside these objects.
    auto* nowv    = new std::atomic<int64_t>(VALID_ACTIVE_NOW);
    auto* offline = new FailingTransport();
    auto* store   = new MemoryStore();

    auto* callers_mutex = new std::mutex();
    auto* callers       = new std::set<std::thread::id>();
    auto* ticks         = new std::atomic<int>(0);

    seed_store_with_valid_lease(*store, VALID_ACTIVE_NOW);

    auto* client = new Client(cfg, *offline, *store,
        [nowv, callers_mutex, callers, ticks] {
            ticks->fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(*callers_mutex);
                callers->insert(std::this_thread::get_id());
            }
            return nowv->load();
        });

    auto* in_callback = new std::atomic<bool>(false);
    auto* sub = new Subscription(client->subscribe([in_callback](State) {
        in_callback->store(true);
        // Still inside the callback when the starts race below (they wait 30 ms).
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }));

    client->startAutoValidation();

    // Force a transition so the worker is parked inside the slow listener.
    nowv->store(VALID_ACTIVE_NOW - 2 * 3600);
    REQUIRE(spin_until(std::chrono::seconds(5), [in_callback]{ return in_callback->load(); }));

    const bool ok = completes_within(std::chrono::seconds(10), [=] {
        std::thread s([=]{ client->stopAutoValidation(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        std::thread t1([=]{ client->startAutoValidation(); });
        std::thread t2([=]{ client->startAutoValidation(); });
        s.join(); t1.join(); t2.join();
    });

    REQUIRE(ok);

    // Let any retired worker finish its in-flight cycle and exit, then look at
    // who is still polling. Two live workers both tick every 10 ms, so a
    // 300 ms window sees both; one worker contributes exactly one id.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lock(*callers_mutex);
        callers->clear();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    size_t distinct = 0;
    {
        std::lock_guard<std::mutex> lock(*callers_mutex);
        distinct = callers->size();
    }
    CHECK(distinct <= 1);   // never two pollers

    // ~Client() joins, so nothing can still be ticking once it returns. That
    // is the safety property the epoch model must not give up, and unlike
    // "quiet after stop" it is deterministic.
    client->stopAutoValidation();
    delete sub;
    delete client;

    const int after_destruction = ticks->load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(ticks->load() == after_destruction);

    delete store; delete offline; delete nowv;
    delete in_callback; delete ticks; delete callers; delete callers_mutex;
}

TEST_CASE("Client: a listener may stop or restart auto-validation from the worker thread") {
    // A listener is delivered on the worker thread and the contract permits it
    // to call back in. Under the epoch model neither start nor stop joins or
    // waits, so this cannot deadlock by construction — but it is exactly the
    // shape that hung under the two previous designs (first by self-joining,
    // then by blocking on a transition flag while an external stop was parked
    // in join() waiting for this very thread). Kept as the regression guard
    // for both.
    //
    // Still driven with a concurrent external stop, because that is what made
    // the window reachable before.
    auto  cfg = make_config();
    cfg.autoValidationIntervalMs = 10;

    // Heap-allocated and leaked on timeout: a hung run has threads parked
    // inside these objects.
    auto* nowv    = new std::atomic<int64_t>(VALID_ACTIVE_NOW);
    auto* offline = new FailingTransport();
    auto* store   = new MemoryStore();

    seed_store_with_valid_lease(*store, VALID_ACTIVE_NOW);

    auto* client = new Client(cfg, *offline, *store, [nowv]{ return nowv->load(); });

    auto* in_callback = new std::atomic<bool>(false);
    auto* released    = new std::atomic<bool>(false);
    auto* did_call    = new std::atomic<bool>(false);

    SUBCASE("the listener stops") {
        auto* sub = new Subscription(client->subscribe(
            [client, in_callback, released, did_call](State) {
                in_callback->store(true);
                // Stay inside the callback long enough for an external stop to
                // reach join(), then call back in from the worker thread.
                spin_until(std::chrono::seconds(5), [released]{ return released->load(); });
                client->stopAutoValidation();
                did_call->store(true);
            }));

        client->startAutoValidation();
        nowv->store(VALID_ACTIVE_NOW - 2 * 3600);
        REQUIRE(spin_until(std::chrono::seconds(5), [in_callback]{ return in_callback->load(); }));

        const bool ok = completes_within(std::chrono::seconds(10), [=] {
            std::thread ext([=]{ client->stopAutoValidation(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            released->store(true);   // let the listener call back in
            ext.join();
        });

        REQUIRE(ok);
        // stop() no longer joins, so the external stop returns without waiting
        // for the listener — the test has to wait for it explicitly rather
        // than lean on a join that the epoch model deliberately removed.
        CHECK(spin_until(std::chrono::seconds(5), [did_call]{ return did_call->load(); }));
        if (ok) delete sub;
    }

    SUBCASE("the listener restarts") {
        auto* sub = new Subscription(client->subscribe(
            [client, in_callback, released, did_call](State) {
                in_callback->store(true);
                spin_until(std::chrono::seconds(5), [released]{ return released->load(); });
                client->startAutoValidation();
                did_call->store(true);
            }));

        client->startAutoValidation();
        nowv->store(VALID_ACTIVE_NOW - 2 * 3600);
        REQUIRE(spin_until(std::chrono::seconds(5), [in_callback]{ return in_callback->load(); }));

        const bool ok = completes_within(std::chrono::seconds(10), [=] {
            std::thread ext([=]{ client->stopAutoValidation(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            released->store(true);
            ext.join();
        });

        REQUIRE(ok);
        // stop() no longer joins, so the external stop returns without waiting
        // for the listener — the test has to wait for it explicitly rather
        // than lean on a join that the epoch model deliberately removed.
        CHECK(spin_until(std::chrono::seconds(5), [did_call]{ return did_call->load(); }));
        if (ok) delete sub;
    }

    client->stopAutoValidation();
    delete client; delete store; delete offline;
    delete nowv; delete in_callback; delete released; delete did_call;
}

TEST_CASE("Client: stop returns without waiting on an in-flight cycle") {
    // The epoch model's headline trade: stop() retires the worker instead of
    // joining it, so it returns promptly even while the worker is stuck inside
    // a slow listener. Under the previous designs this call blocked for the
    // whole callback — measured at 703 ms with a 700 ms listener — which is
    // what put an SDK lock in the caller's path and produced two rounds of
    // deadlocks.
    auto  cfg = make_config();
    cfg.autoValidationIntervalMs = 10;

    std::atomic<int64_t> now{VALID_ACTIVE_NOW};
    FailingTransport     offline;
    MemoryStore          store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ return now.load(); });

    std::atomic<bool> in_callback{false};
    auto sub = client.subscribe([&](State) {
        in_callback.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    });

    client.startAutoValidation();
    now.store(VALID_ACTIVE_NOW - 2 * 3600);
    REQUIRE(spin_until(std::chrono::seconds(5), [&]{ return in_callback.load(); }));

    const auto before = std::chrono::steady_clock::now();
    client.stopAutoValidation();
    const auto elapsed = std::chrono::steady_clock::now() - before;

    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 200);

    // ~Client() is what joins, so the worker still cannot outlive the Client.
}

TEST_CASE("Client: repeated stop/start does not accumulate worker threads") {
    // Retired workers are held until a reaper can join them without blocking.
    // If nothing ever reaps, an unjoined pthread keeps its stack — hundreds of
    // MB across a few hundred cycles — so start/stop must drain finished ones.
    // Observable proxy: the cycle stays fast and the process stays healthy.
    auto  cfg = make_config();
    cfg.autoValidationIntervalMs = 1000;   // long, so workers park immediately

    std::atomic<int64_t> now{VALID_ACTIVE_NOW};
    FailingTransport     offline;
    MemoryStore          store;

    seed_store_with_valid_lease(store, VALID_ACTIVE_NOW);

    Client client(cfg, offline, store, [&]{ return now.load(); });

    for (int i = 0; i < 200; ++i) {
        client.startAutoValidation();
        client.stopAutoValidation();
    }

    // Idempotence still holds at the end of all that.
    client.startAutoValidation();
    client.startAutoValidation();
    client.stopAutoValidation();
    client.stopAutoValidation();

    CHECK(true);   // reaching here without exhausting threads is the assertion
}

TEST_CASE("Client: an anchor ahead of the clock does not pass the maxOfflineDays bound") {
    // A clock pushed forward across a validate leaves the persisted anchor
    // ahead of real time. `now - anchor` is then NEGATIVE, so a bare
    // `> max_age` comparison can never fire and the offline bound is silently
    // disabled for as long as the anchor stays in the future.
    auto cfg = make_config();
    cfg.maxOfflineDays = 2;

    int64_t          now = VALID_ACTIVE_NOW;
    FailingTransport offline;
    MemoryStore      store;

    const int64_t future_anchor = VALID_ACTIVE_NOW + 2 * 86400;
    seed_store_with_valid_lease(store, now, 1781681046LL, future_anchor);

    Client client(cfg, offline, store, [&]{ return now; });

    // Read it back once the wall clock has caught up past the anchor, so the
    // rollback guard is no longer the one answering and we observe the state
    // refresh_state_from_store_ actually resolved at construction. The lease's
    // own ~7-day TTL has not run out, so only the offline bound can catch it.
    now = future_anchor + 3600;
    CHECK(client.state() == State::Expired);
}
