// tests/test_auth_trial.cpp — regression coverage for
//   1. SDK-key authentication: every API call must carry X-Keylight-SDK-Key.
//   2. The local, offline-first trial state machine (startTrial / checkTrial /
//      trialDaysLeft) and its persistence across every store-writing path.
//
// Helpers live in an anonymous namespace so they cannot collide with the
// identically-named fakes in test_client.cpp (both TUs link into one binary).

#include "doctest.h"
#include "keylight/client.hpp"
#include "keylight/config.hpp"
#include "keylight/json.hpp"
#include "keylight/store.hpp"
#include "keylight/transport.hpp"

#include <map>
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

// ===========================================================================
// 1. SDK-key authentication
// ===========================================================================

TEST_CASE("Auth: activate sends X-Keylight-SDK-Key") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;

    Client client(cfg, transport, store, []{ return T0; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    const auto* call = transport.last_call_for("activate");
    REQUIRE(call != nullptr);
    CHECK(RecordingTransport::header(*call, "X-Keylight-SDK-Key") == SDK_KEY);
    CHECK(RecordingTransport::header(*call, "Content-Type") == "application/json");
}

TEST_CASE("Auth: validate sends X-Keylight-SDK-Key") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;

    Client client(cfg, transport, store, []{ return T0; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    transport.next_body = VALIDATE_OK;
    REQUIRE(client.validate().is_ok());

    const auto* call = transport.last_call_for("validate");
    REQUIRE(call != nullptr);
    CHECK(RecordingTransport::header(*call, "X-Keylight-SDK-Key") == SDK_KEY);
}

TEST_CASE("Auth: deactivate sends X-Keylight-SDK-Key") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;

    Client client(cfg, transport, store, []{ return T0; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    transport.next_body = R"({"deactivated":true})";
    REQUIRE(client.deactivate().is_ok());

    const auto* call = transport.last_call_for("deactivate");
    REQUIRE(call != nullptr);
    CHECK(RecordingTransport::header(*call, "X-Keylight-SDK-Key") == SDK_KEY);
}

TEST_CASE("Auth: launch reconciliation validate sends X-Keylight-SDK-Key") {
    auto cfg = make_config();
    RecordingTransport transport;
    MemStore           store;

    // Prime the store with an activated license.
    {
        transport.next_body = ACTIVATE_OK;
        Client client(cfg, transport, store, []{ return T0; });
        REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    }

    // Fresh Client — checkOnLaunch() must revalidate, authenticated.
    transport.calls.clear();
    transport.next_body = VALIDATE_OK;
    Client relaunched(cfg, transport, store, []{ return T0 + 7200; });
    REQUIRE(relaunched.checkOnLaunch().is_ok());

    const auto* call = transport.last_call_for("validate");
    REQUIRE(call != nullptr);
    CHECK(RecordingTransport::header(*call, "X-Keylight-SDK-Key") == SDK_KEY);
}

TEST_CASE("Auth: no header when sdkKey is not configured") {
    auto cfg   = make_config();
    cfg.sdkKey = "";
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;

    Client client(cfg, transport, store, []{ return T0; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    const auto* call = transport.last_call_for("activate");
    REQUIRE(call != nullptr);
    CHECK(call->headers.count("X-Keylight-SDK-Key") == 0);
}

// ===========================================================================
// 2. Local trial state machine
// ===========================================================================

TEST_CASE("Trial: disabled when trialDurationDays == 0") {
    auto cfg = make_config(0);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });

    CHECK(client.checkTrial()    == TrialStatus::NotStarted);
    CHECK(client.trialDaysLeft() == 0);

    auto r = client.startTrial();
    REQUIRE(r.is_ok());
    CHECK(r.value()           == State::Invalid);
    CHECK(client.state()      == State::Invalid);
    CHECK(client.checkTrial() == TrialStatus::NotStarted);
    // Nothing was persisted — a trial start must never be written when
    // trials are disabled.
    CHECK(store.has_data_ == false);
}

TEST_CASE("Trial: first startTrial enters Trial and persists trialStart") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });

    // Construction alone must not start a trial.
    CHECK(client.state()      == State::Invalid);
    CHECK(client.checkTrial() == TrialStatus::NotStarted);

    auto r = client.startTrial();
    REQUIRE(r.is_ok());
    CHECK(r.value()              == State::Trial);
    CHECK(client.state()         == State::Trial);
    CHECK(client.checkTrial()    == TrialStatus::Active);
    CHECK(client.trialDaysLeft() == 14);
    CHECK(store.int_field("trialStart") == T0);
    CHECK(transport.calls.empty()); // starting a trial is purely local
}

TEST_CASE("Trial: repeated startTrial never resets the clock") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    now = T0 + 5 * DAY;
    auto r = client.startTrial();
    REQUIRE(r.is_ok());
    CHECK(r.value()                     == State::Trial);
    CHECK(store.int_field("trialStart") == T0);   // unchanged
    CHECK(client.trialDaysLeft()        == 9);
}

TEST_CASE("Trial: restored from the store by a new Client") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    {
        Client client(cfg, transport, store, [&]{ return now; });
        REQUIRE(client.startTrial().is_ok());
    }

    now = T0 + 3 * DAY;
    Client relaunched(cfg, transport, store, [&]{ return now; });
    CHECK(relaunched.state()         == State::Trial);
    CHECK(relaunched.checkTrial()    == TrialStatus::Active);
    CHECK(relaunched.trialDaysLeft() == 11);
}

TEST_CASE("Trial: duration boundary expires the trial") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    // One second before the boundary: still one day left.
    now = T0 + 14 * DAY - 1;
    CHECK(client.checkTrial()    == TrialStatus::Active);
    CHECK(client.trialDaysLeft() == 1);

    // Exactly at the boundary: elapsed == duration → expired.
    now = T0 + 14 * DAY;
    CHECK(client.checkTrial()    == TrialStatus::Expired);
    CHECK(client.trialDaysLeft() == 0);

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value()      == State::Expired);
    CHECK(client.state() == State::Expired);
}

TEST_CASE("Trial: an expired trial is never restarted") {
    auto cfg = make_config(7);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    now = T0 + 30 * DAY;
    auto r = client.startTrial();
    REQUIRE(r.is_ok());
    CHECK(r.value()                     == State::Expired);
    CHECK(client.checkTrial()           == TrialStatus::Expired);
    CHECK(store.int_field("trialStart") == T0); // still the original start

    // A brand-new Client on the same store sees the same expired trial.
    Client relaunched(cfg, transport, store, [&]{ return now; });
    CHECK(relaunched.state()      == State::Expired);
    CHECK(relaunched.checkTrial() == TrialStatus::Expired);
}

TEST_CASE("Trial: clock moving backwards does not extend the window") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    now = T0 - 100 * DAY; // wall clock jumped backwards
    CHECK(client.checkTrial()    == TrialStatus::Active);
    CHECK(client.trialDaysLeft() == 14); // clamped, never more than the duration
}

TEST_CASE("Trial: checkOnLaunch resolves a persisted trial without network") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;
    transport.fail = true; // any request would be a failure — there must be none

    int64_t now = T0;
    {
        Client client(cfg, transport, store, [&]{ return now; });
        REQUIRE(client.startTrial().is_ok());
    }

    now = T0 + 2 * DAY;
    Client relaunched(cfg, transport, store, [&]{ return now; });
    auto r = relaunched.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value()               == State::Trial);
    CHECK(relaunched.state()      == State::Trial);
    CHECK(transport.calls.empty());
}

TEST_CASE("Trial: checkOnLaunch never starts a trial by itself") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;
    transport.fail = true;

    Client client(cfg, transport, store, []{ return T0; });
    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value()           == State::Invalid);
    CHECK(client.checkTrial() == TrialStatus::NotStarted);
    CHECK(store.has_data_     == false);
    CHECK(transport.calls.empty());
}

TEST_CASE("Trial: paid activation overrides an active trial") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());
    REQUIRE(client.state() == State::Trial);

    transport.next_body = ACTIVATE_OK;
    auto r = client.activate("XXXX-YYYY-ZZZZ-0001");
    REQUIRE(r.is_ok());
    CHECK(r.value()           == State::Licensed);
    CHECK(client.state()      == State::Licensed);
    // The trial keeps running underneath — it is neither consumed nor reset.
    CHECK(client.checkTrial()           == TrialStatus::Active);
    CHECK(store.int_field("trialStart") == T0);
}

TEST_CASE("Trial: deactivation returns to the original trial state") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    transport.next_body = ACTIVATE_OK;
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    REQUIRE(client.state() == State::Licensed);

    // Two days later the user deactivates the seat.
    now = T0 + 2 * DAY;
    transport.next_body = R"({"deactivated":true})";
    REQUIRE(client.deactivate().is_ok());

    CHECK(client.state()                == State::Trial);
    CHECK(client.trialDaysLeft()        == 12); // clock never restarted
    CHECK(store.int_field("trialStart") == T0);
    CHECK(store.has_field("licenseKey") == false);
    CHECK(store.has_field("lease")      == false);
}

TEST_CASE("Trial: deactivation after the trial elapsed resolves Expired") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    transport.next_body = ACTIVATE_OK;
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    now = T0 + 40 * DAY;
    transport.next_body = R"({"deactivated":true})";
    REQUIRE(client.deactivate().is_ok());

    CHECK(client.state()      == State::Expired);
    CHECK(client.checkTrial() == TrialStatus::Expired);
    // Still not restartable.
    REQUIRE(client.startTrial().is_ok());
    CHECK(client.state()                == State::Expired);
    CHECK(store.int_field("trialStart") == T0);
}

TEST_CASE("Trial: deactivation without any trial clears the store entirely") {
    auto cfg = make_config(14); // trials enabled, but never started
    RecordingTransport transport;
    MemStore           store;
    transport.next_body = ACTIVATE_OK;

    Client client(cfg, transport, store, []{ return T0; });
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    transport.next_body = R"({"deactivated":true})";
    REQUIRE(client.deactivate().is_ok());

    auto loaded = store.load();
    REQUIRE(loaded.is_ok());
    CHECK(loaded.value().empty());
    CHECK(client.state()      == State::Invalid);
    CHECK(client.checkTrial() == TrialStatus::NotStarted);
}

// ===========================================================================
// 3. Persistence regressions — no store-writing path may drop trialStart
// ===========================================================================

TEST_CASE("Trial: trialStart survives activate, validate and lease refresh") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    transport.next_body = ACTIVATE_OK;
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    CHECK(store.int_field("trialStart") == T0);

    transport.next_body = VALIDATE_OK;
    REQUIRE(client.validate().is_ok());
    CHECK(store.int_field("trialStart") == T0);
    // The centralized serializer also keeps the fields validate() used to drop.
    CHECK(store.str_field("licenseKey") == "XXXX-YYYY-ZZZZ-0001");
    CHECK(store.str_field("instanceId") == "inst-abc");
    CHECK(store.int_field("lastValidatedOnline") == now);

    now = T0 + DAY;
    REQUIRE(client.refreshIfNeeded().is_ok());
    CHECK(store.int_field("trialStart") == T0);
    CHECK(store.str_field("licenseKey") == "XXXX-YYYY-ZZZZ-0001");
}

TEST_CASE("Trial: trialStart survives a revoke that clears the lease") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });
    REQUIRE(client.startTrial().is_ok());

    transport.next_body = ACTIVATE_OK;
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());

    // Dashboard revoke: HTTP 422 with an error and no lease.
    transport.next_status = 422;
    transport.next_body   = REVOKE_422;
    auto r = client.validate();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Invalid); // a revoked seat does not reopen the trial

    CHECK(store.has_field("lease")      == false);
    CHECK(store.int_field("trialStart") == T0);
    CHECK(store.str_field("licenseKey") == "XXXX-YYYY-ZZZZ-0001");
}

TEST_CASE("Trial: a corrupted store never grants a fresh trial") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;
    transport.fail = true;

    store.save("{not json at all");

    Client client(cfg, transport, store, []{ return T0; });
    CHECK(client.state()         == State::Invalid);
    CHECK(client.checkTrial()    == TrialStatus::NotStarted);
    CHECK(client.trialDaysLeft() == 0);

    auto r = client.checkOnLaunch();
    REQUIRE(r.is_ok());
    CHECK(r.value() == State::Invalid);
    CHECK(transport.calls.empty());
}

TEST_CASE("Trial: a store holding only a trialStart restores the trial") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;
    transport.fail = true;

    store.save("{\"trialStart\":" + std::to_string(T0) + "}");

    int64_t now = T0 + DAY;
    Client client(cfg, transport, store, [&]{ return now; });
    CHECK(client.state()         == State::Trial);
    CHECK(client.trialDaysLeft() == 13);
}

TEST_CASE("Trial: state transitions fire subscription events") {
    auto cfg = make_config(14);
    RecordingTransport transport;
    MemStore           store;

    int64_t now = T0;
    Client client(cfg, transport, store, [&]{ return now; });

    std::vector<State> seen;
    auto sub = client.subscribe([&](State s){ seen.push_back(s); });

    REQUIRE(client.startTrial().is_ok());
    REQUIRE(seen.size() == 1);
    CHECK(seen.back() == State::Trial);

    transport.next_body = ACTIVATE_OK;
    REQUIRE(client.activate("XXXX-YYYY-ZZZZ-0001").is_ok());
    REQUIRE(seen.size() == 2);
    CHECK(seen.back() == State::Licensed);
}
