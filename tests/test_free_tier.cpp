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

// ===========================================================================
// The anonymous free-tier instance id
// ===========================================================================

TEST_CASE("Instance id: minted once and stable across calls") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    const std::string a = client.freeTierInstanceId();
    const std::string b = client.freeTierInstanceId();

    CHECK(a.size() == 36);
    CHECK(a == b);
    CHECK(store.str_field("freeTierInstanceId") == a);
}

TEST_CASE("Instance id: restored by a new Client on the same store") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    std::string first;
    {
        Client client(cfg, transport, store, [&]{ return now; });
        first = client.freeTierInstanceId();
    }
    Client relaunched(cfg, transport, store, [&]{ return now; });
    CHECK(relaunched.freeTierInstanceId() == first);
}

TEST_CASE("Instance id: startTrial mints one for conversion attribution") {
    // keylight-rust start_trial() writes FREE_TIER_INSTANCE_ID alongside
    // TRIAL_START so a trial that converts can be attributed.
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    CHECK(store.has_field("trialStart"));
    CHECK(store.str_field("freeTierInstanceId").size() == 36);
}

TEST_CASE("Instance id: trials disabled means startTrial mints nothing") {
    auto cfg = make_config(0);
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());
    CHECK_FALSE(store.has_field("freeTierInstanceId"));
}

TEST_CASE("Instance id: survives activate and validate") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;
    int64_t now = T0;

    Client client(cfg, transport, store, [&]{ return now; });
    const std::string id = client.freeTierInstanceId();

    REQUIRE(client.activate("KL-TEST-KEY").is_ok());
    CHECK(store.str_field("freeTierInstanceId") == id);

    REQUIRE(client.validate().is_ok());
    CHECK(store.str_field("freeTierInstanceId") == id);
}

// ===========================================================================
// Hardware id, machine_hash and the keyless beacon
// ===========================================================================

TEST_CASE("Hardware id: cached after the first successful read") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now   = T0;
    int     reads = 0;

    // Succeeds once, then fails — models a transient IOKit/registry failure.
    auto flaky = [&]() -> std::optional<std::string> {
        if (reads++ == 0) return std::string("hardware-1");
        return std::nullopt;
    };

    Client client(cfg, transport, store, [&]{ return now; }, flaky);
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(store.str_field("cachedHardwareId") == "hardware-1");

    // A later beacon must still carry the hash, from the cached id.
    transport.calls.clear();
    client.reportKeylessState(KeylessState::Expired);
    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->body.find(CANONICAL_HASH) != std::string::npos);
}

TEST_CASE("Hardware id: machine_hash omitted entirely when none is available") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);

    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->body.find("machine_hash") == std::string::npos);
}

TEST_CASE("Hardware id: an empty id is treated as absent") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto empty = []() -> std::optional<std::string> { return std::string(""); };

    Client client(cfg, transport, store, [&]{ return now; }, empty);
    client.reportKeylessState(KeylessState::FreeTier);

    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->body.find("machine_hash") == std::string::npos);
}

TEST_CASE("Beacon: posts instance_id, state and the SDK key to /keyless") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto hw = []() -> std::optional<std::string> { return std::string("hardware-1"); };

    Client client(cfg, transport, store, [&]{ return now; }, hw);
    const std::string id = client.freeTierInstanceId();
    client.reportKeylessState(KeylessState::FreeTier);

    const auto* call = transport.last_call_for("keyless");
    REQUIRE(call != nullptr);
    CHECK(call->method == "POST");
    CHECK(call->url    == "https://api.keylight.dev/testco/testapp/keyless");
    CHECK(RecordingTransport::header(*call, "X-Keylight-SDK-Key") == SDK_KEY);
    CHECK(call->body.find("\"instance_id\":\"" + id + "\"") != std::string::npos);
    CHECK(call->body.find("\"state\":\"free_tier\"")        != std::string::npos);
    CHECK(call->body.find(CANONICAL_HASH)                   != std::string::npos);
    CHECK(call->body.find("\"sdk\":\"cpp\"")                != std::string::npos);
}

TEST_CASE("Beacon: debounced — same state inside 24h sends nothing") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);
    REQUIRE(transport.calls.size() == 1);

    now = T0 + 23 * 3600;
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 1);
}

TEST_CASE("Beacon: a changed state defeats the debounce immediately") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::Trial);
    REQUIRE(transport.calls.size() == 1);

    now = T0 + 60;
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 2);
}

TEST_CASE("Beacon: the same state sends again once 24h have passed") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);
    REQUIRE(transport.calls.size() == 1);

    now = T0 + DAY + 1;
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 2);
}

TEST_CASE("Beacon: a non-200 response does NOT arm the debounce") {
    // keylight-rust records the debounce only on success, so a failed beacon
    // cannot suppress free-tier reporting for a full day.
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_status = 500;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);
    REQUIRE(transport.calls.size() == 1);
    CHECK_FALSE(store.has_field("keylessLastState"));

    now = T0 + 60;
    transport.next_status = 200;
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 2);
    CHECK(store.str_field("keylessLastState") == "free_tier");
    CHECK(store.int_field("lastKeylessPingAt") == now);
}

TEST_CASE("Beacon: a network error is swallowed and leaves the debounce unarmed") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    transport.fail = true;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    client.reportKeylessState(KeylessState::FreeTier);   // must not throw
    CHECK_FALSE(store.has_field("keylessLastState"));
}

TEST_CASE("Beacon: the debounce survives a relaunch") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    {
        Client client(cfg, transport, store, [&]{ return now; }, none);
        client.reportKeylessState(KeylessState::FreeTier);
    }
    REQUIRE(transport.calls.size() == 1);

    now = T0 + 3600;
    Client relaunched(cfg, transport, store, [&]{ return now; }, none);
    relaunched.reportKeylessState(KeylessState::FreeTier);
    CHECK(transport.calls.size() == 1);
}

TEST_CASE("Beacon: reporting never changes the resolved state") {
    auto cfg = canonical_config();
    RecordingTransport transport;
    MemStore           store;
    int64_t now = T0;
    auto none = []() -> std::optional<std::string> { return std::nullopt; };

    Client client(cfg, transport, store, [&]{ return now; }, none);
    REQUIRE(client.checkOnLaunch().value() == State::FreeTier);
    client.reportKeylessState(KeylessState::FreeTier);
    CHECK(client.state() == State::FreeTier);
}
