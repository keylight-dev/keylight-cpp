// tests/test_free_tier.cpp — the free-tier state, the anonymous keyless beacon
// and the cross-SDK machine_hash attribution fields.
//
// Helpers live in an anonymous namespace so they cannot collide with the
// identically-named fakes in test_client.cpp / test_auth_trial.cpp (all three
// TUs link into one binary).

#include "doctest.h"
#include "keylight/client.hpp"
#include "keylight/config.hpp"
#include "keylight/json.hpp"
#include "keylight/store.hpp"
#include "keylight/transport.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace keylight;

namespace {

// ---------------------------------------------------------------------------
// RecordingTransport — captures every request (headers included) and replays a
// canned response.  Per-call responses can be queued; otherwise the default is
// reused for every call.
// ---------------------------------------------------------------------------
class RecordingTransport : public Transport {
public:
    struct Call {
        std::string                        method;
        std::string                        url;
        std::map<std::string, std::string> headers;
        std::string                        body;
    };

    int         next_status = 200;
    std::string next_body   = "{}";
    bool        fail        = false;   // simulate a network error

    std::vector<Call> calls;

    Result<HttpResponse> request(
        const std::string&                        method,
        const std::string&                        url,
        const std::map<std::string, std::string>& headers,
        const std::string&                        body) override
    {
        calls.push_back({method, url, headers, body});
        if (fail) {
            return Result<HttpResponse>::err({ErrorCode::Network, "offline"});
        }
        HttpResponse r;
        r.status = next_status;
        r.body   = next_body;
        return Result<HttpResponse>::ok(r);
    }

    // Convenience: the last call whose URL ends with /<action>.
    const Call* last_call_for(const std::string& action) const {
        for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
            const std::string suffix = "/" + action;
            if (it->url.size() >= suffix.size() &&
                it->url.compare(it->url.size() - suffix.size(),
                                suffix.size(), suffix) == 0) {
                return &*it;
            }
        }
        return nullptr;
    }

    static std::string header(const Call& c, const std::string& name) {
        auto it = c.headers.find(name);
        return it == c.headers.end() ? std::string{} : it->second;
    }
};

// ---------------------------------------------------------------------------
// MemStore — in-memory LicenseStore
// ---------------------------------------------------------------------------
class MemStore : public LicenseStore {
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

    // Read one persisted field back out of the blob.
    int64_t int_field(const std::string& key) const {
        auto jr = Json::parse(data_);
        if (!jr.is_ok()) return 0;
        return jr.value()[key].as_int();
    }
    std::string str_field(const std::string& key) const {
        auto jr = Json::parse(data_);
        if (!jr.is_ok()) return {};
        return jr.value()[key].as_string();
    }
    bool has_field(const std::string& key) const {
        auto jr = Json::parse(data_);
        if (!jr.is_ok()) return false;
        auto keys = jr.value().keys();
        for (const auto& k : keys) if (k == key) return true;
        return false;
    }
};

// ---------------------------------------------------------------------------
// Conformance vector[0] "valid-active" — same fixture test_client.cpp uses.
// ---------------------------------------------------------------------------
const char*   PUBKEY_K1 = "8QkyJGwaIqAuN0jdsCnBtv3D9fylv4PHqCVufx7xje0=";
const int64_t T0        = 1781076256LL; // lease is active at this instant
const char*   SDK_KEY   = "sdk_live_test_abc123";

const std::string ACTIVATE_OK = R"({
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

const std::string VALIDATE_OK = R"({
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

// Production worker's definitive rejection: 422, an error string, no lease.
const std::string REVOKE_422 = R"({"error":"Instance not found or deactivated"})";

Config make_config(int trial_days = 0) {
    Config cfg;
    cfg.tenantId          = "testco";
    cfg.productId         = "testapp";
    cfg.sdkKey            = SDK_KEY;
    cfg.trustedKeys["k1"] = PUBKEY_K1;
    cfg.trialDurationDays = trial_days;
    return cfg;
}

constexpr int64_t DAY = 86400;

} // namespace


namespace {
// tenantId/productId are already "testco"/"testapp", which is exactly the
// tenant/product of the cross-SDK canonical machine_hash vector.
Config canonical_config() {
    Config cfg = make_config();
    cfg.freeTierEnabled = true;
    return cfg;
}

const char* CANONICAL_HASH =
    "8e8871112f28cabda180ada131d0b4f4f07c72fb47c5d884edbe32812885b22a";
} // namespace

// ===========================================================================
// Free tier — state resolution
// ===========================================================================

TEST_CASE("Wire strings for KeylessState match the other SDKs") {
    CHECK(std::string(keyless_state_wire(KeylessState::Trial))    == "trial");
    CHECK(std::string(keyless_state_wire(KeylessState::FreeTier)) == "free_tier");
    CHECK(std::string(keyless_state_wire(KeylessState::Expired))  == "expired");
}

TEST_CASE("FreeTier: disabled by default — no license resolves Invalid") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    CHECK(client.checkOnLaunch().value() == State::Invalid);
}

TEST_CASE("FreeTier: enabled and no trial resolves FreeTier offline") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.fail = true;             // any request would fail — there must be none
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value()      == State::FreeTier);
    CHECK(client.state() == State::FreeTier);
    CHECK(transport.calls.empty());
}

TEST_CASE("FreeTier: an active trial still outranks the free tier") {
    auto cfg = make_config(14);
    cfg.freeTierEnabled = true;
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());
    CHECK(client.state() == State::Trial);
}

TEST_CASE("FreeTier: an ELAPSED trial drops to FreeTier, not Expired") {
    // keylight-rust resolve_state(): the `free_tier_enabled` arm sits after the
    // trial match, so an elapsed trial falls through to FreeTier.  Without free
    // tier the same client resolves Expired.
    auto cfg = make_config(14);
    cfg.freeTierEnabled = true;
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    {
        Client client(cfg, transport, store, [&]{ return now; });
        REQUIRE(client.startTrial().is_ok());
    }

    now = T0 + 14 * DAY;
    Client elapsed(cfg, transport, store, [&]{ return now; });
    CHECK(elapsed.checkOnLaunch().value() == State::FreeTier);

    auto no_ft = make_config(14);
    Client without(no_ft, transport, store, [&]{ return now; });
    CHECK(without.checkOnLaunch().value() == State::Expired);
}

TEST_CASE("FreeTier: a paid license still wins") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.activate("KL-TEST-KEY").is_ok());
    CHECK(client.state() == State::Licensed);
}

TEST_CASE("FreeTier: deactivate lands on FreeTier rather than the paywall") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.activate("KL-TEST-KEY").is_ok());
    REQUIRE(client.state() == State::Licensed);

    transport.next_body = "{}";
    REQUIRE(client.deactivate().is_ok());
    CHECK(client.state() == State::FreeTier);
}
