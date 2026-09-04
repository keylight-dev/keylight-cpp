#pragma once
// keylight/client.hpp — Client state machine: activate/validate/deactivate/
//                       checkOnLaunch/refreshIfNeeded + events + offline grace.
// Ported from keylight-rust keylight/src/client.rs and
//            keylight-csharp src/Keylight/Keylight.cs
//
// URL pattern:  {baseUrl}/{tenantId}/{productId}/{action}
// Activate:     POST /{tenantId}/{productId}/activate
// Validate:     POST /{tenantId}/{productId}/validate
// Deactivate:   POST /{tenantId}/{productId}/deactivate
//
// Thread-safety: state() reads std::atomics and the injected clock, and takes
//                no lock — audio-thread safe. See the NOW-FUNCTION CONTRACT
//                below for what that requires of the clock.
//                hasEntitlement / cachedLicenseExpiresAt / listener list are
//                guarded by a mutex.
//
// NOW-FUNCTION CONTRACT
// ─────────────────────
// Client takes an optional `now_fn` (a std::function<int64_t()>) so tests and
// integrators can supply the clock. state() calls it — to run the clock-
// rollback guard — and state() is `noexcept` and documented audio-thread safe.
// A caller-supplied `now_fn` MUST therefore be:
//   - non-throwing        — an exception escaping a noexcept function is
//                           std::terminate, and here that would happen on the
//                           audio thread;
//   - non-blocking        — no mutex, no I/O, no syscall that can wait;
//   - allocation-free     — no heap traffic on the audio thread.
// It must also be non-empty: invoking an empty std::function throws
// std::bad_function_call, which is the same std::terminate.
// The shipped default, std::time(nullptr), satisfies all of this. If your
// clock cannot, do not call state() from an audio callback — mirror it into
// your own std::atomic from a background thread instead (this is exactly what
// the JUCE adapter does).

#include "clock.hpp"
#include "config.hpp"
#include "device_info.hpp"
#include "lease.hpp"
#include "result.hpp"
#include "machine_id.hpp"
#include "store.hpp"
#include "transport.hpp"
#include "verifier.hpp"
#include "version.hpp"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <ctime>
#include <future>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace keylight {

// ---------------------------------------------------------------------------
// State — high-level license state (C++ subset of Rust/C# states)
// ---------------------------------------------------------------------------
enum class State {
    Licensed,   // trusted, unexpired active lease
    Trial,      // no license; within trial window
    Expired,    // trusted lease expired, or license status "expired"
    Invalid,    // no trusted lease, no active trial
    FreeTier,   // no license and no trial, but the product offers a free tier.
                // Appended last on purpose: renumbering the values above would
                // break any integrator that persisted a State as an integer.
    Limited,    // trusted lease with server status "fallback": the server could
                // not mint a full lease, so the app runs degraded rather than
                // locked. Appended last for the same reason as FreeTier —
                // renumbering would break any integrator that persisted a
                // State as an integer.
};

// ---------------------------------------------------------------------------
// TrialStatus — local, offline-first trial (mirrors keylight-rust TrialStatus)
//
// Trials are entirely local: the start timestamp is persisted next to the
// lease and the window is measured against the client's clock. No API call is
// involved — the free-tier / keyless beacon is a separate feature.
// ---------------------------------------------------------------------------
enum class TrialStatus {
    NotStarted, // no trial timestamp persisted (or trials disabled)
    Active,     // within trialDurationDays of the persisted start
    Expired,    // the trial window has elapsed
};

// ---------------------------------------------------------------------------
// KeylessState — what the anonymous keyless beacon reports (mirrors
// keylight-rust KeylessState).  The wire strings are fixed by the server and
// shared with every other Keylight SDK.
// ---------------------------------------------------------------------------
enum class KeylessState {
    Trial,
    FreeTier,
    Expired,
};

inline const char* keyless_state_wire(KeylessState s) {
    switch (s) {
        case KeylessState::Trial:    return "trial";
        case KeylessState::FreeTier: return "free_tier";
        case KeylessState::Expired:  return "expired";
    }
    return "expired";
}

// ---------------------------------------------------------------------------
// compile-time platform string (matches Rust telemetry.rs)
// ---------------------------------------------------------------------------
namespace detail {
inline const char* current_platform() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32) || defined(_WIN64)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

// 128 bits of hex for the X-Keylight-Request-Id correlation header. Not a
// security token — it exists so an app log line and a worker log line can be
// matched up during support, so a per-thread PRNG is sufficient.
inline std::string random_request_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* kHex = "0123456789abcdef";

    std::string out;
    out.reserve(32);
    for (int i = 0; i < 4; ++i) {
        uint64_t chunk = rng();
        for (int j = 0; j < 8; ++j) {
            out += kHex[chunk & 0xF];
            chunk >>= 4;
        }
    }
    return out;
}
} // namespace detail

// ---------------------------------------------------------------------------
// Timer-model constants (ported verbatim from keylight-rust client.rs)
// ---------------------------------------------------------------------------
static constexpr int64_t REFRESH_DEBOUNCE  =   300; // 5 min
static constexpr int64_t REFRESH_STALE     = 21600; // 6 h
static constexpr int64_t NEAR_EXPIRY_SECS  = 86400; // 24 h — refresh when lease < 24h away

// ---------------------------------------------------------------------------
// Subscription — RAII handle returned by on() / subscribe().
// Calling unsubscribe() (or letting the handle go out of scope / be moved-from)
// removes the callback from the client's listener list.
// ---------------------------------------------------------------------------
class Client; // forward

class Subscription {
public:
    // Default-constructed handle is a no-op.
    Subscription() = default;

    // Move-only.
    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& o) noexcept
        : client_(o.client_), id_(o.id_) { o.client_ = nullptr; }

    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) {
            unsubscribe();
            client_ = o.client_;
            id_     = o.id_;
            o.client_ = nullptr;
        }
        return *this;
    }

    ~Subscription() { unsubscribe(); }

    void unsubscribe();

private:
    friend class Client;
    explicit Subscription(Client* c, uint64_t id) : client_(c), id_(id) {}

    Client*  client_ = nullptr;
    uint64_t id_     = 0;
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
class Client {
public:
    // Production constructor — clock defaults to real wall clock.
    Client(Config cfg, Transport& transport, LicenseStore& store)
        : Client(std::move(cfg), transport, store,
                 []{ return static_cast<int64_t>(std::time(nullptr)); },
                 []{ return detail::read_hardware_id(); })
    {}

    // Testable constructor — inject a deterministic clock.
    // now_fn() must return Unix epoch seconds as int64_t.
    Client(Config                   cfg,
           Transport&               transport,
           LicenseStore&            store,
           std::function<int64_t()> now_fn)
        : Client(std::move(cfg), transport, store, std::move(now_fn),
                 []{ return detail::read_hardware_id(); })
    {}

    // Testable constructor — inject a deterministic clock AND hardware id.
    // hardware_id_fn() returns the true OS/hardware id, or nullopt when the
    // platform has none.  It must NEVER return a random per-install value:
    // machine_hash exists to dedupe a device across reinstalls, and a random
    // fallback would defeat exactly that.
    Client(Config                                      cfg,
           Transport&                                  transport,
           LicenseStore&                               store,
           std::function<int64_t()>                    now_fn,
           std::function<std::optional<std::string>()> hardware_id_fn)
        : cfg_(std::move(cfg))
        , transport_(transport)
        , store_(store)
        , now_fn_(std::move(now_fn))
        , hardware_id_fn_(std::move(hardware_id_fn))
        , verifier_(cfg_.trustedKeys)
        , state_(State::Invalid)
    {
        // Prime state from persisted store (if any) on construction.
        refresh_state_from_store_();
        // Seed the event dedupe with what we booted with. Nobody can have
        // subscribed yet, so this is not a suppressed event -- it stops the
        // first notify_() poll from reporting the initial state as a change.
        last_reported_.store(state());
    }

    // Destructor: stops and joins any running auto-validation thread so the
    // thread cannot outlive the Client (no detached threads, no std::terminate).
    //
    // It does NOT fence an event delivery already in flight. The thread
    // draining the event queue need not be the auto-validation worker — any
    // thread that calls refreshIfNeeded()/validate() can be holding the
    // delivery baton — so destroying a Client while another thread is inside
    // notify_() is a use-after-free. Own the Client for as long as any thread
    // can still call into it; the LISTENER CONTRACT's "do not destroy from a
    // callback" is the special case, not the whole rule.
    ~Client() {
        Reaper to_join;
        {
            std::lock_guard<std::mutex> lock(av_mutex_);
            ++av_epoch_;              // retire every worker, current or not
            av_cv_.notify_all();
            to_join.workers.reserve(av_retired_.size() + 1);
            if (av_current_.thread.joinable()) {
                to_join.workers.push_back(std::move(av_current_));
                av_current_ = AvWorker{};
            }
            for (auto& w : av_retired_) to_join.workers.push_back(std::move(w));
            av_retired_.clear();
        }
        // The only join in the lifecycle. Outside the lock, because a worker
        // mid-refresh needs av_mutex_ to notice the epoch moved.
        //
        // Destroying a Client from its own worker thread — i.e. from a
        // state-change listener — self-joins and terminates. That is
        // forbidden by the LISTENER CONTRACT and left loud on purpose: the
        // alternative is a silent use-after-free, since the thread returns
        // into av_loop_ and touches av_mutex_ after this destructor is done.
        //
        // This can block for a listener callback plus a network round trip —
        // the cost stopAutoValidation() used to pay is concentrated here now.
        // A JUCE ~Licensing() runs on the message thread, so budget for it.
    }

    // ── Sync API ──────────────────────────────────────────────────────────

    /// Activate a license key.  Returns the resulting State.
    /// On an unrecognised/invalid lease the store is NOT updated and
    /// State::Invalid is returned (no exception thrown).
    Result<State> activate(const std::string& key) {
        // Build activate request body
        // A real hostname, not a constant: this string is what the customer
        // sees in their device list. "device" survives only as the fallback
        // when the platform read fails.
        std::string instance_name = detail::detect_machine_name();
        if (instance_name.empty()) instance_name = "device";

        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key",   json_str(key)},
            {"instance_name", json_str(instance_name)},
        };
        append_attribution_fields_(fields, /*include_instance_id=*/true);
        std::string body = build_json_(std::move(fields), true /*telemetry*/);

        std::string url = api_url_("activate");
        auto hr = transport_.request("POST", url, json_headers_(), body);
        if (!hr.is_ok()) {
            return Result<State>::err(hr.error());
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
            return Result<State>::err({ErrorCode::Http,
                http_error_message_(resp.body, "activate", resp.status)});
        }

        // Parse activate response
        auto jr = Json::parse(resp.body);
        if (!jr.is_ok()) {
            return Result<State>::err({ErrorCode::BadResponse, "activate: invalid JSON"});
        }
        const Json& j = jr.value();

        bool activated = j["activated"].as_bool();
        if (!activated) {
            // Server declined — keep existing state
            return report_(state_.load());
        }

        // Parse optional lease (present when the object has sub-keys)
        std::optional<Lease> lease;
        auto lease_node = j["lease"];
        if (lease_node.size() > 0) {
            auto lr = Lease::from_json(lease_node);
            if (!lr.is_ok()) {
                return Result<State>::err(lr.error());
            }
            lease = lr.value();
        }

        // Parse optional license_expires_at (0 means absent/null)
        std::optional<int64_t> expires_at;
        {
            int64_t v = j["license_expires_at"].as_int();
            if (v != 0) expires_at = v;
        }

        // Parse optional instance_id
        std::optional<std::string> instance_id;
        {
            std::string v = j["instance_id"].as_string();
            if (!v.empty()) instance_id = v;
        }

        // Resolve state from the returned lease (verify but don't persist
        // on invalid signature)
        State new_state = resolve_from_lease_(lease);

        // Persist only trusted leases
        if (lease.has_value() && verifier_.verify(*lease, now_fn_()).is_trusted()) {
            std::string lease_json = lease_to_json_(*lease);
            persist_({lease_json, expires_at, instance_id, key});
            save_last_validated_online_(now_fn_());
        } else if (!lease.has_value() && activated) {
            // Server said activated=true but sent no lease — treat as Licensed
            // without a local lease; persist what we have.
            persist_({std::nullopt, expires_at, instance_id, key});
            new_state = State::Licensed;
        }

        new_state = resolve_with_trial_(new_state);
        set_state_(new_state);
        return report_(new_state);
    }

    /// Validate the stored license online.  Returns the resulting State.
    Result<State> validate() {
        // Poll the clock guard, for the same reason refreshIfNeeded() does:
        // a host that polls validate() on its own timer would otherwise get
        // the guard in state() and never in its callback.
        notify_();

        // Need license_key and instance_id from cache (Worker requires both)
        std::string license_key  = load_license_key_();
        std::string instance_id  = load_instance_id_();

        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key", json_str(license_key)},
            {"instance_id", json_str(instance_id)},
        };
        // machine_hash only — the free-tier id belongs on activate, which is
        // where a conversion is actually recorded (keylight-rust does the same;
        // deactivate gets neither, it already identifies the device).
        append_attribution_fields_(fields, /*include_instance_id=*/false);
        std::string body = build_json_(std::move(fields), true /*telemetry*/);

        std::string url = api_url_("validate");
        auto hr = transport_.request("POST", url, json_headers_(), body);
        if (!hr.is_ok()) {
            // Network failure: keep existing state
            return report_(state_.load());
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
            // 422 is the worker's definitive-rejection status for /validate
            // (revoke, deactivated instance, expired/fallback lease) — parse
            // the body instead of treating it as transient. Any other
            // non-200 status keeps the existing state (unchanged behavior).
            if (resp.status == 422) {
                auto rejected = handle_validate_rejection_(resp.body, now_fn_());
                if (rejected.has_value()) {
                    return report_(*rejected);
                }
            }
            return report_(state_.load());
        }

        auto jr = Json::parse(resp.body);
        if (!jr.is_ok()) {
            return report_(state_.load());
        }
        const Json& j = jr.value();

        // Parse optional lease
        std::optional<Lease> lease;
        auto lease_node = j["lease"];
        if (lease_node.size() > 0) {
            auto lr = Lease::from_json(lease_node);
            if (lr.is_ok()) {
                lease = lr.value();
            }
        }

        // Parse optional license_expires_at (0 means absent/null)
        std::optional<int64_t> expires_at;
        {
            int64_t v = j["license_expires_at"].as_int();
            if (v != 0) expires_at = v;
        }

        // Update cached lease if server returned one
        if (lease.has_value() && verifier_.verify(*lease, now_fn_()).is_trusted()) {
            std::string lease_json = lease_to_json_(*lease);
            // Keep existing instance_id
            std::optional<std::string> iid;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (cached_instance_id_.has_value()) {
                    iid = cached_instance_id_;
                }
            }
            persist_({lease_json, expires_at, iid});
            save_last_validated_online_(now_fn_());
        }

        // Paid licensing wins; an unusable result falls back to the local
        // trial instead of dropping a trialling user to Invalid.
        State new_state = resolve_with_trial_(resolve_from_lease_(lease));
        set_state_(new_state);
        return report_(new_state);
    }

    /// Deactivate this device.  Clears the local cache regardless of the
    /// network outcome, but no longer hides a server rejection: a 4xx here
    /// means the seat is still consumed, and only the caller can decide to
    /// retry.
    Result<void> deactivate() {
        std::string instance_id = load_instance_id_();
        std::string license_key = load_license_key_();

        std::optional<Error> server_error;
        if (!instance_id.empty()) {
            // The worker requires BOTH fields (DeactivateBodySchema); sending
            // instance_id alone is rejected by zod and frees nothing.
            std::string body = build_json_({
                {"license_key", json_str(license_key)},
                {"instance_id", json_str(instance_id)},
            }, false);
            std::string url = api_url_("deactivate");
            auto hr = transport_.request("POST", url, json_headers_(), body);
            if (!hr.is_ok()) {
                server_error = hr.error();
            } else if (hr.value().status != 200) {
                server_error = Error{ErrorCode::Http,
                    http_error_message_(hr.value().body, "deactivate",
                                        hr.value().status)};
            }
        }

        // Clear the paid-licensing half of the cache. The trial start survives:
        // deactivating a paid license must never hand the user a fresh trial
        // (nor restart an expired one) — see startTrial().
        bool keep_trial = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_lease_                  = std::nullopt;
            cached_expires_at_             = std::nullopt;
            cached_instance_id_            = std::nullopt;
            cached_license_key_            = std::nullopt;
            cached_last_validated_online_  = 0;
            last_validated_online_atomic_.store(0);
            keep_trial                     = cached_trial_start_.has_value();
        }

        if (keep_trial) {
            // Rewrite the blob with the trial start (and nothing else) instead
            // of dropping the file — clear() would restart the trial clock.
            auto sr = save_cache_();
            if (!sr.is_ok()) {
                return sr;
            }
        } else {
            auto cr = store_.clear();
            if (!cr.is_ok()) {
                return cr;
            }
        }

        // No paid license left: the persisted trial (if any) decides the state.
        set_state_(resolve_with_trial_(State::Invalid));

        if (server_error.has_value()) {
            return Result<void>::err(*server_error);
        }
        return Result<void>::ok();
    }

    // ── Trial API (local, offline-first) ──────────────────────────────────

    /// Explicitly begin the local trial. Idempotent: an existing trial start
    /// is never overwritten, so an expired trial cannot be restarted by
    /// calling this again (or by deactivating a paid license and re-calling).
    /// No-op when trials are disabled (Config::trialDurationDays <= 0).
    /// Performs store I/O — never call this from an audio thread.
    /// Anonymous, per-install identifier for keyless/free-tier reporting.
    /// Minted on first use and persisted; never derived from a licence or from
    /// hardware.  Returns an empty string only when the store write fails.
    /// Mirrors keylight-rust free_tier_instance_id().
    std::string freeTierInstanceId() {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_free_tier_instance_id_.has_value()) {
                return *cached_free_tier_instance_id_;
            }
            cached_free_tier_instance_id_ = detail::uuid_v4();
        }
        if (!save_cache_().is_ok()) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_free_tier_instance_id_.reset();
            return {};
        }
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return *cached_free_tier_instance_id_;
    }

    /// Anonymous keyless/free-tier beacon.  Fire-and-forget: every error is
    /// swallowed, nothing is thrown, and the resolved state never changes.
    /// Debounced to once per 24h per state — a state *change* always sends.
    ///
    /// Nothing calls this for you.  keylight-rust behaves the same way: the
    /// core never emits network traffic the integrator did not ask for, which
    /// is what keeps checkOnLaunch() free of network I/O while a DAW scans the
    /// plugin.  The JUCE adapter wires it to state transitions for you.
    ///
    /// Blocking network call — never invoke it from an audio thread.
    void reportKeylessState(KeylessState state) {
        const std::string wire = keyless_state_wire(state);

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            const bool changed =
                !cached_keyless_last_state_.has_value() ||
                *cached_keyless_last_state_ != wire;
            const bool within_24h =
                cached_last_keyless_ping_at_ != 0 &&
                (now_fn_() - cached_last_keyless_ping_at_) < 86400;
            if (!changed && within_24h) {
                return;
            }
        }

        const std::string instance = freeTierInstanceId();
        if (instance.empty()) {
            return;   // could not persist an id; nothing to report under
        }

        std::vector<std::pair<std::string, std::string>> fields{
            {"instance_id", json_str(instance)},
            {"state",       json_str(wire)},
        };
        if (auto hash = machine_hash_()) {
            fields.push_back({"machine_hash", json_str(*hash)});
        }

        auto hr = transport_.request("POST", api_url_("keyless"),
                                     json_headers_(),
                                     build_json_(std::move(fields), true));
        // Arm the debounce ONLY on a real 200.  A failed beacon must not
        // suppress reporting for a day (keylight-rust does the same).
        if (!hr.is_ok() || hr.value().status != 200) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_keyless_last_state_   = wire;
            cached_last_keyless_ping_at_ = now_fn_();
        }
        (void)save_cache_();
    }

    Result<State> startTrial() {
        if (cfg_.trialDurationDays <= 0) {
            // Trials disabled — nothing is persisted and no state changes.
            return report_(state_.load());
        }

        bool started = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (!cached_trial_start_.has_value()) {
                cached_trial_start_ = now_fn_();
                started             = true;
            }
        }
        if (started) {
            save_cache_();
        }
        // Attribution: a trial that later converts must carry the same
        // anonymous id the keyless beacon reported it under.
        freeTierInstanceId();

        State new_state = resolve_current_state_();
        set_state_(new_state);
        return report_(new_state);
    }

    /// Current local trial status. Never performs I/O beyond reading the
    /// in-memory cache primed from the store.
    TrialStatus checkTrial() const {
        if (cfg_.trialDurationDays <= 0) {
            return TrialStatus::NotStarted;
        }
        std::optional<int64_t> start;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            start = cached_trial_start_;
        }
        if (!start.has_value()) {
            return TrialStatus::NotStarted;
        }
        return days_left_from_(*start) > 0 ? TrialStatus::Active
                                           : TrialStatus::Expired;
    }

    /// Whole days remaining in the local trial; 0 when disabled, not started,
    /// or elapsed. Matches keylight-rust's `days_left` (seconds / 86400).
    int trialDaysLeft() const {
        if (cfg_.trialDurationDays <= 0) {
            return 0;
        }
        std::optional<int64_t> start;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            start = cached_trial_start_;
        }
        if (!start.has_value()) {
            return 0;
        }
        int64_t left = days_left_from_(*start);
        return left > 0 ? static_cast<int>(left) : 0;
    }

    // ── Async wrappers ────────────────────────────────────────────────────

    std::future<Result<State>> activateAsync(const std::string& key) {
        return std::async(std::launch::async,
                          [this, key]{ return activate(key); });
    }

    std::future<Result<State>> validateAsync() {
        return std::async(std::launch::async,
                          [this]{ return validate(); });
    }

    std::future<Result<void>> deactivateAsync() {
        return std::async(std::launch::async,
                          [this]{ return deactivate(); });
    }

    // ── Background auto-validation ────────────────────────────────────────

    /// Spawn a single background thread that periodically calls
    /// refreshIfNeeded() on the schedule configured by
    /// cfg_.autoValidationIntervalMs.  Never started implicitly — the host
    /// application must call this explicitly.
    ///
    /// Idempotent: a second call while a worker is running is a no-op.
    /// Restartable: stop-then-start works from any thread, including from a
    /// state-change listener delivered on the worker thread itself.
    ///
    /// LIFECYCLE MODEL — read this before changing anything here.
    ///
    /// Workers are retired by EPOCH, and start/stop NEVER join. A worker
    /// captures av_epoch_ when it is spawned and exits the first time it sees
    /// a different one; stop simply bumps the epoch and wakes it. Neither
    /// function ever releases av_mutex_ mid-body, so there is no window for a
    /// concurrent caller to act on a half-changed state, and no caller ever
    /// blocks on another thread.
    ///
    /// That is the entire point. The previous model had start and stop join
    /// the worker, which meant releasing av_mutex_ mid-body, which needed a
    /// transition flag to cover the gap, which needed "am I the worker?"
    /// special cases so a listener calling back in did not block on that flag.
    /// Four rounds of review found a hang in each layer. Joining was the root
    /// cause; this removes it rather than guarding it.
    ///
    /// THE COST, stated plainly: stopAutoValidation() no longer guarantees the
    /// worker has EXITED when it returns — only that it will not run another
    /// cycle. A worker already inside refreshIfNeeded() finishes that call
    /// first. ~Client() is what joins, so the thread can never outlive the
    /// Client.
    void startAutoValidation() {
        // Reaper, not a bare vector: it holds JOINABLE threads, and destroying
        // a joinable std::thread is std::terminate. Anything that throws below
        // — make_shared, or the std::thread constructor on EAGAIN, which a DAW
        // hosting many plugin instances near RLIMIT_NPROC can really hit —
        // would otherwise abort the host uncatchably from inside a licensing
        // SDK. This is the job the deleted TransitionGuard used to do.
        Reaper reap;
        {
            std::lock_guard<std::mutex> lock(av_mutex_);
            reap.workers = take_exited_();

            // A joinable current worker means one is running the current
            // epoch. Retired workers live in av_retired_, so this cannot be
            // confused by a finished-but-unjoined thread.
            if (!av_current_.thread.joinable()) {
                const uint64_t epoch = ++av_epoch_;
                auto done = std::make_shared<std::atomic<bool>>(false);
                av_current_.done   = done;
                av_current_.thread = std::thread([this, epoch, done] {
                    av_loop_(epoch);
                    // Last act: publish that the loop is over, so a later
                    // reaper can join without blocking on a round trip.
                    done->store(true);
                });
            }
        }
        // ~Reaper joins outside the lock. Every worker it holds has already
        // left av_loop_, so join() is bounded by thread teardown — it cannot
        // wait on the network and it cannot wait on a listener.
    }

    /// Retire the auto-validation worker. Safe from any thread, including
    /// from a state-change listener delivered on the worker thread itself.
    /// Idempotent: safe when nothing is running.
    ///
    /// Returns immediately. It does NOT join — see the lifecycle model on
    /// startAutoValidation(). The worker will not begin another cycle, but one
    /// already inside refreshIfNeeded() finishes that call first, so a tick or
    /// a state-change event can still land shortly after this returns.
    /// ~Client() joins, so no worker outlives the Client.
    void stopAutoValidation() {
        Reaper reap;   // joins on unwind — see startAutoValidation()
        {
            std::lock_guard<std::mutex> lock(av_mutex_);
            reap.workers = take_exited_();

            if (av_current_.thread.joinable()) {
                // Reserve BEFORE the epoch bump. push_back reallocates
                // whenever take_exited_() reaped nothing, and a bad_alloc
                // there would leave the epoch retired with the worker still
                // in av_current_ and joinable — so startAutoValidation()
                // would no-op forever and auto-validation would be silently
                // dead for the life of the process.
                av_retired_.reserve(av_retired_.size() + 1);
                ++av_epoch_;              // retires the current worker
                av_cv_.notify_all();      // wake it out of its interval wait
                av_retired_.push_back(std::move(av_current_));
                av_current_ = AvWorker{};
            }
        }
    }

    // ── Launch / refresh API ──────────────────────────────────────────────

    /// Load the cached lease from the store, verify it offline, set state;
    /// then ALWAYS perform a server validate() round-trip — never gated by
    /// the in-session staleness timer. A revoke/expiry on the dashboard must
    /// land on the very next launch, not lag behind the 5min/6h/24h cadence
    /// that refreshIfNeeded() uses for long-running hosts between launches.
    /// If there is no cached lease, state stays as-is (Invalid/initial) and
    /// no network call is made (nothing to revalidate).
    /// A transient/network failure does not mutate state beyond the existing
    /// offline-grace bound (see apply_offline_grace_).
    /// Ported from keylight-rust check_on_launch() and keylight-csharp CheckOnLaunchAsync(),
    /// updated per the cross-SDK revocation/offline-bound parity design (2026-07-08).
    Result<State> checkOnLaunch() {
        // The cache is already primed on construction via refresh_state_from_store_().
        if (has_stored_license_()) {
            // Report what state() reports. Offline, the grace window would
            // otherwise hand back Licensed against a clock that has moved
            // backward — the launch path and the paywall must not disagree.
            return report_(validate_and_reconcile_());
        }
        // No paid license: resolve the persisted local trial offline. This
        // never *starts* a trial — a DAW scanning or instantiating a plugin
        // must not consume the user's trial window; only startTrial() does
        // that, and only when the user asks for it.
        State new_state = resolve_current_state_();
        set_state_(new_state);
        return report_(new_state);
    }

    /// Apply the timer model: refresh debounce 5min, stale 6h, near-expiry 24h.
    /// If a refresh is due, calls validate(); otherwise returns current state.
    /// On a network failure within maxOfflineDays grace window, keeps Licensed.
    /// Ported from keylight-rust refresh_if_needed() and keylight-csharp RefreshIfNeededAsync().
    /// This in-session cadence is unchanged by the always-validate-on-launch
    /// fix: it still governs long-running hosts between launches.
    Result<State> refreshIfNeeded() {
        // Poll the clock guard first. Every return below this line can
        // short-circuit without touching state_, and a clock that moved
        // changes no raw state — so without this, a rollback would reach
        // state() and never reach a single subscriber.
        notify_();

        if (!has_stored_license_()) {
            // No paid license — nothing to revalidate online, but the local
            // trial may have elapsed since the last resolve. keylight-rust and
            // keylight-js recompute check_trial() inside state() on every call;
            // C++ state() reads an atomic (audio-thread contract) and cannot,
            // so the trial is re-resolved here instead. Hosts already call this
            // on focus/resume, and startAutoValidation() ticks it, so a trial
            // that runs out mid-session downgrades on its own and the change
            // reaches subscribers. Still purely local — no network call.
            State new_state = resolve_current_state_();
            set_state_(new_state);
            return report_(new_state);
        }

        int64_t now          = now_fn_();
        int64_t last_lvo     = load_last_validated_online_();
        bool    has_lvo      = (last_lvo > 0);

        // Debounce: skip if validated within the last 5 minutes
        if (has_lvo && (now - last_lvo) < REFRESH_DEBOUNCE) {
            return report_(state_.load());
        }

        // Near-expiry check: refresh if lease expires within 24h
        bool near_expiry = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_lease_.has_value()) {
                near_expiry = (cached_lease_->expiresAt - now) < NEAR_EXPIRY_SECS;
            }
        }

        // Refresh if no prior validated_online, stale (>=6h), or near expiry
        bool do_refresh = !has_lvo
            || (now - last_lvo) >= REFRESH_STALE
            || near_expiry;

        if (!do_refresh) {
            return report_(state_.load());
        }

        return report_(validate_and_reconcile_());
    }

    // ── Events API ────────────────────────────────────────────────────────

    /// Register a callback for state-transition events.
    /// event: currently only "change" is defined (fires on every state transition).
    /// Returns a Subscription RAII handle; when the handle is destroyed or
    /// unsubscribe() is called, the callback is removed.
    /// Callbacks are dispatched on whichever thread happens to be draining the
    /// event queue, which is NOT necessarily the thread that caused the
    /// transition. UI/audio hosts must marshal to their own thread.
    Subscription on(const std::string& /*event*/,
                    std::function<void(State)> cb)
    {
        return subscribe(std::move(cb));
    }

    /// Subscribe to all state transitions. Returns a Subscription RAII handle.
    ///
    /// The callback receives what state() would return, so an event-driven
    /// paywall and a query-driven one cannot disagree.
    ///
    /// LISTENER CONTRACT:
    ///   - No lock is held while your callback runs, so it may call back into
    ///     this Client (validate(), refreshIfNeeded(), …) and may take your
    ///     own locks. A re-entrant call queues its event rather than
    ///     recursing; it may be delivered by a different thread.
    ///   - Unsubscribing from inside your own callback is supported.
    ///   - Do NOT destroy this Client from a callback. ~Client() joins the
    ///     auto-validation thread, and a listener delivered on that thread
    ///     cannot join itself. stopAutoValidation() handles that case (it
    ///     signals and returns); the destructor cannot.
    ///   - Events are delivered in order, but not synchronously: the call
    ///     that caused a transition may return before the event has been
    ///     delivered by whichever thread holds the delivery baton.
    ///
    ///   - A listener MUST NOT throw. An exception cannot be reported from
    ///     here — delivery runs on whatever thread moved the state — so it is
    ///     caught and swallowed, and the remaining listeners still get the
    ///     event.
    ///   - unsubscribe() does not fence a delivery already in flight on
    ///     another thread. Keep whatever your listener captures alive across
    ///     that window (the JUCE adapter uses a shared alive_ flag).
    ///
    /// The callback runs on whichever thread is draining the queue. That is
    /// usually the thread that caused the transition, but under concurrency it
    /// can be another one — a thread already delivering picks up your event
    /// rather than handing it back. Never the audio thread. Marshal to your UI
    /// thread yourself.
    Subscription subscribe(std::function<void(State)> cb) {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        uint64_t id = ++next_listener_id_;
        listeners_.push_back({id, std::move(cb)});
        return Subscription(this, id);
    }

    // ── Query API ─────────────────────────────────────────────────────────

    /// Current state — reads atomics only; audio-thread safe, never blocks.
    ///
    /// A clock rolled back beyond tolerance since the last recorded server
    /// contact invalidates any offline reasoning we could do, so this fails
    /// closed rather than trusting a lease against a moved clock.
    ///
    /// This method is `noexcept` and documented audio-thread safe, and it
    /// calls the caller-supplied `now_fn_`. See the NOW-FUNCTION CONTRACT at
    /// the top of this header: a `now_fn` that throws terminates the process,
    /// and one that locks or allocates breaks the audio-thread guarantee.
    State state() const noexcept {
        if (clock_untrusted_()) return State::Invalid;
        return state_.load();
    }

    /// True iff the cached, verified lease contains the named entitlement.
    ///
    /// Applies the same clock-rollback guard as state(). A feature gate that
    /// kept answering true while state() answered Invalid would fail OPEN —
    /// the paywall and the gate must agree.
    bool hasEntitlement(const std::string& feature) const {
        if (clock_untrusted_()) return false;
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (!cached_lease_.has_value()) return false;
        const auto& l = *cached_lease_;
        // Only count if still trusted + not expired at current clock
        auto vr = verifier_.verify(l, now_fn_());
        if (!vr.is_trusted() || vr.expired || l.status == "expired") return false;
        for (const auto& e : l.entitlements) {
            if (e == feature) return true;
        }
        return false;
    }

    /// Cached license expiry (epoch seconds) from the last activate/validate.
    std::optional<int64_t> cachedLicenseExpiresAt() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cached_expires_at_;
    }

private:
    // ── Clock trust ───────────────────────────────────────────────────────

    // state() promises the audio thread "atomics only, no lock". On a target
    // where these are mutex-backed that promise is silently false, and the
    // failure mode is a priority-inverted audio dropout, not a test failure.
    static_assert(std::atomic<int64_t>::is_always_lock_free,
                  "keylight::Client::state() is documented audio-thread safe; "
                  "std::atomic<int64_t> is not lock-free on this target");
    static_assert(std::atomic<State>::is_always_lock_free,
                  "keylight::Client::state() is documented audio-thread safe; "
                  "std::atomic<State> is not lock-free on this target");

    /// True when the system clock has moved backward, beyond tolerance, since
    /// the last recorded server contact. Every state read point consults this
    /// so they cannot disagree: state(), hasEntitlement() and — through
    /// report_() — every State a public method hands back all fail closed
    /// together.
    ///
    /// Reads the ATOMIC anchor mirror, never the mutex-guarded field, so it
    /// stays usable from noexcept, lock-free, audio-thread-safe state().
    /// An anchor of 0 means "never validated online" — there is nothing to
    /// compare against, and the offline bound in refresh_state_from_store_()
    /// already fails that case closed.
    bool clock_untrusted_() const noexcept {
        const int64_t anchor = last_validated_online_atomic_.load();
        return anchor != 0 && clock_rolled_back(anchor, now_fn_());
    }

    /// Every public entry point hands its State back through here, so no
    /// caller can be told something state() would contradict. Without it the
    /// guard covers only the paywall: refreshIfNeeded() is what long-running
    /// hosts poll between launches, and it would keep reporting Licensed from
    /// the debounce and staleness short-circuits — no server contact, cached
    /// state, moved clock — while state() answered Invalid.
    ///
    /// A successful round-trip re-anchors the clock before returning, so on
    /// the online paths this is a no-op; it bites exactly on the offline and
    /// short-circuit returns, which is where it must.
    ///
    /// Errors pass through untouched: an error is not a state claim, and
    /// rewriting it to Invalid would lose the failure the caller needs.
    Result<State> report_(State s) const {
        if (clock_untrusted_()) return Result<State>::ok(State::Invalid);
        return Result<State>::ok(s);
    }

    Result<State> report_(Result<State> r) const {
        if (r.is_ok() && clock_untrusted_()) {
            return Result<State>::ok(State::Invalid);
        }
        return r;
    }

    // ── Dependencies ──────────────────────────────────────────────────────
    Config                   cfg_;
    Transport&               transport_;
    LicenseStore&            store_;
    std::function<int64_t()> now_fn_;
    std::function<std::optional<std::string>()> hardware_id_fn_;
    Verifier                 verifier_;

    // ── State ─────────────────────────────────────────────────────────────
    std::atomic<State>       state_;
    // Last value handed to subscribers. Distinct from state_ because the
    // clock guard can change what we report without state_ changing at all,
    // and because two raw states can report as the same guarded one.
    std::atomic<State>       last_reported_{State::Invalid};
    // Atomic mirror of cached_last_validated_online_, so the clock guard can
    // run inside noexcept, lock-free state(). The mutex-protected field stays
    // the source of truth for persistence; this is written alongside it.
    std::atomic<int64_t>     last_validated_online_atomic_{0};

    // Mutex-guarded cache of the decoded lease + extras
    mutable std::mutex               cache_mutex_;
    std::optional<Lease>             cached_lease_;
    std::optional<int64_t>           cached_expires_at_;
    std::optional<std::string>       cached_instance_id_;
    std::optional<std::string>       cached_license_key_;
    // Epoch seconds of last successful online validation (0 = never).
    int64_t                          cached_last_validated_online_ = 0;
    // Epoch seconds when the local trial was started (nullopt = never started).
    std::optional<int64_t>           cached_trial_start_;
    std::optional<std::string>       cached_free_tier_instance_id_;
    std::optional<std::string>       cached_hardware_id_;
    std::optional<std::string>       cached_keyless_last_state_;
    int64_t                          cached_last_keyless_ping_at_ = 0;

    // ── Event listeners ───────────────────────────────────────────────────
    struct Listener {
        uint64_t                   id;
        std::function<void(State)> cb;
    };
    mutable std::mutex        listeners_mutex_;
    std::vector<Listener>     listeners_;
    uint64_t                  next_listener_id_ = 0;
    // Guards the event ORDER (pending_ + delivering_ + the dedupe), never the
    // delivery itself — see notify_(). Never nested with listeners_mutex_,
    // cache_mutex_ or av_mutex_ — each is taken and released on its own — and
    // never held across a callback, so it cannot join an application's lock
    // cycle.
    std::mutex                notify_mutex_;
    std::vector<State>        pending_;              // events in delivery order
    bool                      delivering_ = false;   // the delivery baton

    // ── Background auto-validation ────────────────────────────────────────
    // av_mutex_ guards every field below. Neither startAutoValidation() nor
    // stopAutoValidation() releases it mid-body, so there is no half-changed
    // state for a concurrent caller to observe.
    //
    // The worker holds a unique_lock<av_mutex_> for its epoch check and
    // interval wait, then RELEASES it before calling refreshIfNeeded() (which
    // acquires cache_mutex_ / notify_mutex_ / listeners_mutex_) to avoid
    // deadlock and to keep start/stop responsive during a round trip.
    mutable std::mutex      av_mutex_;
    std::condition_variable av_cv_;

    // Monotone. A worker captures this at spawn and exits when it changes.
    // Bumping it is how stopAutoValidation() and ~Client() retire a worker
    // without joining.
    uint64_t                av_epoch_ = 0;

    // A spawned worker. `done` is set by the thread as its very last act, so a
    // reaper can distinguish "already left av_loop_" (join returns at once)
    // from "still inside a round trip" (join would block on the network).
    struct AvWorker {
        std::thread                        thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    // Holds workers on their way to a join, and joins them however the scope
    // exits. A bare vector of joinable std::threads is a std::terminate
    // waiting for an exception.
    struct Reaper {
        std::vector<AvWorker> workers;
        ~Reaper() {
            for (auto& w : workers) if (w.thread.joinable()) w.thread.join();
        }
    };

    // The worker running the CURRENT epoch. A joinable thread here is the
    // definition of "auto-validation is running".
    AvWorker                av_current_;
    // Retired workers, awaiting a reap. Entries are moved out by take_exited_()
    // on the next start or stop once they have finished, and ~Client() joins
    // whatever is left. Not bounded by a constant — the bound is (in-flight
    // refresh duration / stop-start period), so a program that cycles faster
    // than its round trips holds more. Never unbounded growth: a worker that
    // has finished is reaped by the very next start or stop.
    std::vector<AvWorker>   av_retired_;

    // ── Private helpers ───────────────────────────────────────────────────

    std::string api_url_(const std::string& action) const {
        std::string base = cfg_.apiBaseUrl;
        // Strip trailing slash
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/" + cfg_.tenantId + "/" + cfg_.productId + "/" + action;
    }

    /// Headers for every Keylight API call. The tenant SDK key authenticates
    /// the request; without it the worker answers 401 to activate/validate/
    /// deactivate. Every call site goes through this helper so no endpoint can
    /// be added later that forgets to authenticate.
    std::map<std::string, std::string> json_headers_() const {
        std::map<std::string, std::string> headers{
            {"Content-Type", "application/json"},
        };
        if (!cfg_.sdkKey.empty()) {
            headers["X-Keylight-SDK-Key"] = cfg_.sdkKey;
        }
        headers["X-Keylight-Request-Id"] = detail::random_request_id();
        return headers;
    }

    // The worker's human-readable rejection reason, e.g. "License key not
    // found" or "Activation limit reached". This is the string an integrator's
    // UI shows the customer, so a status line is the fallback, not the default.
    // `message` is accepted alongside `error` because the two are used
    // interchangeably across worker routes.
    static std::string http_error_message_(const std::string& body,
                                           const std::string& action,
                                           int                status)
    {
        const std::string fallback = action + " HTTP " + std::to_string(status);
        if (body.empty()) return fallback;

        auto jr = Json::parse(body);
        if (!jr.is_ok()) return fallback;

        std::string msg = jr.value()["error"].as_string();
        if (msg.empty()) msg = jr.value()["message"].as_string();
        return msg.empty() ? fallback : msg;
    }

    // Tiny JSON string escaping (no control chars expected in these values)
    static std::string json_str(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if      (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else                out += c;
        }
        out += "\"";
        return out;
    }

    // Build JSON object string from key→pre-encoded-value pairs.
    // If include_telemetry is true, appends sdk_version, platform, sdk,
    // app_version and the coarse cpu_cores / memory buckets.
    std::string build_json_(
        std::vector<std::pair<std::string, std::string>> fields,
        bool include_telemetry) const
    {
        if (include_telemetry) {
            fields.push_back({"sdk_version", json_str(KEYLIGHT_SDK_VERSION)});
            fields.push_back({"platform",    json_str(detail::current_platform())});
            // `platform` cannot identify the SDK: this one, Rust and C# all send
            // the same canonical macos/windows/linux tokens, so the server used
            // to label every C++ device "Rust". Identify ourselves explicitly.
            fields.push_back({"sdk",         json_str(KEYLIGHT_SDK_ID)});
            if (!cfg_.appVersion.empty()) {
                fields.push_back({"app_version", json_str(cfg_.appVersion)});
            }
            // Coarse device buckets. Never the raw core count or byte count —
            // see device_info.hpp for the cross-SDK bucket contract. Omitted
            // entirely when the platform cannot report the value.
            const char* cores = detail::cpu_cores_bucket(detail::detect_cpu_cores());
            if (cores[0] != '\0') {
                fields.push_back({"cpu_cores", json_str(cores)});
            }
            const char* mem = detail::memory_bucket(detail::detect_physical_memory_bytes());
            if (mem[0] != '\0') {
                fields.push_back({"memory", json_str(mem)});
            }
            // Phase-3 device dimensions. Both are omitted entirely when the
            // platform cannot report them — never a placeholder. device_class
            // is deliberately absent: the server derives it, and inventing one
            // here would fight that.
            std::string osv = detail::detect_os_version();
            if (!osv.empty()) {
                fields.push_back({"os_version", json_str(osv)});
            }
            const char* arch = detail::current_arch();
            if (arch[0] != '\0') {
                fields.push_back({"arch", json_str(arch)});
            }
        }

        std::string out = "{";
        bool first = true;
        for (const auto& [k, v] : fields) {
            if (!first) out += ",";
            out += json_str(k) + ":" + v;
            first = false;
        }
        out += "}";
        return out;
    }

    // Serialize a Lease to JSON (camelCase keys — wire format).
    static std::string lease_to_json_(const Lease& l) {
        std::string ents = "[";
        for (size_t i = 0; i < l.entitlements.size(); ++i) {
            if (i > 0) ents += ",";
            ents += json_str(l.entitlements[i]);
        }
        ents += "]";

        // clang-format off
        return "{"
            "\"kid\":"            + json_str(l.kid)            + ","
            "\"licenseKeyHash\":" + json_str(l.licenseKeyHash) + ","
            "\"instanceId\":"     + json_str(l.instanceId)     + ","
            "\"issuedAt\":"       + std::to_string(l.issuedAt)  + ","
            "\"expiresAt\":"      + std::to_string(l.expiresAt) + ","
            "\"status\":"         + json_str(l.status)          + ","
            "\"entitlements\":"   + ents                        + ","
            "\"signature\":"      + json_str(l.signature)       +
            "}";
        // clang-format on
    }

    /// Handle a 422 response body from the /validate endpoint — the worker's
    /// status for a definitive rejection (revoke, deactivated instance, or a
    /// genuinely stale license). Real 422 payloads take two shapes:
    ///   - `{"lease": {...}, ...}` — the license itself is stale (lease
    ///     status "expired"/"fallback"); the lease is still signed and must
    ///     be trusted/persisted so state() resolves Expired/Limited off it,
    ///     exactly like a 200 response with a non-"active" lease.
    ///   - `{"error": "..."}` with NO `lease` field at all — a genuine
    ///     revoke / "instance not found or deactivated". There is nothing
    ///     to trust here: the previously-cached "active" lease must be
    ///     cleared so state() can no longer resolve Licensed off stale data
    ///     (leaving it in place is exactly the bug this method fixes).
    /// Returns std::nullopt if the body could not be decoded at all — the
    /// caller then falls back to treating this like a transient failure
    /// (network hiccup / non-JSON body), never mutating stored state.
    /// Mirrors keylight-js Client.validate()'s 422-decodable handling.
    std::optional<State> handle_validate_rejection_(const std::string& body, int64_t now) {
        auto jr = Json::parse(body);
        if (!jr.is_ok()) {
            return std::nullopt; // undecodable — treat as transient
        }
        const Json& j = jr.value();

        auto lease_node = j["lease"];
        if (lease_node.size() > 0) {
            auto lr = Lease::from_json(lease_node);
            if (lr.is_ok()) {
                const Lease& lease = lr.value();
                if (verifier_.verify(lease, now).is_trusted()) {
                    std::string lease_json = lease_to_json_(lease);
                    std::optional<std::string> iid;
                    {
                        std::lock_guard<std::mutex> lock(cache_mutex_);
                        iid = cached_instance_id_;
                    }
                    std::optional<int64_t> expires_at;
                    {
                        int64_t v = j["license_expires_at"].as_int();
                        if (v != 0) expires_at = v;
                    }
                    persist_({lease_json, expires_at, iid});
                    save_last_validated_online_(now);
                }
                State new_state = resolve_with_trial_(resolve_from_lease_(lease));
                set_state_(new_state);
                return new_state;
            }
            // Malformed lease payload inside a definitive-rejection response —
            // cannot be trusted either way; fall through and treat it like the
            // no-lease deny path rather than silently keeping stale state.
        }

        // Definitive rejection with no (usable) lease — a real revoke. This
        // deliberately does NOT fall back to the local trial: the stored
        // license key is still there and a revoked seat must not silently
        // reopen an old trial (keylight-rust resolves the same case off
        // `had_stored_license`, never off the trial).
        clear_lease_();
        set_state_(State::Invalid);
        return State::Invalid;
    }

    /// Clear the stored trusted lease (used when a 422 definitively rejects
    /// with no lease at all — a real revoke / deactivated instance). Keeps
    /// license_key/instance_id/lastValidatedOnline intact — only the lease
    /// itself is removed, so a later refresh_state_from_store_() (e.g. next
    /// process launch) sees no cached lease and resolves Invalid instead of
    /// trusting the last-known "active" data. Mirrors keylight-js's
    /// `del(ACCOUNT.LEASE)` in the no-lease rejection branch of validate().
    void clear_lease_() {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_lease_ = std::nullopt;
        }
        // Rewrite the blob from the cache: every other field (instanceId,
        // licenseKey, lastValidatedOnline, trialStart) is preserved because
        // serialization happens in exactly one place.
        save_cache_();
    }

    // Derive State from an optional (possibly-null) lease using current clock.
    State resolve_from_lease_(const std::optional<Lease>& lease) const {
        if (!lease.has_value()) {
            // No lease in response — stay at current state (caller may override)
            return state_.load();
        }
        const Lease& l = *lease;
        auto vr = verifier_.verify(l, now_fn_());
        if (!vr.is_trusted()) {
            return State::Invalid;
        }
        // Trusted: interpret status
        if (l.status == "active") {
            return vr.expired ? State::Expired : State::Licensed;
        }
        // Rust's resolve_state maps ("fallback", _) -> Limited BEFORE the
        // expired arm. Keeping fallback on Expired locks the app over a
        // server-side signing incident.
        if (l.status == "fallback") return State::Limited;
        // "expired", or anything else from a trusted lease → Expired
        return State::Expired;
    }

    // Reload state from the persistent store (called on construction).
    void refresh_state_from_store_() {
        auto lr = store_.load();
        if (!lr.is_ok() || lr.value().empty()) {
            state_.store(State::Invalid);
            return;
        }
        // Try to decode as our persisted blob: a JSON object with
        // "lease", "expiresAt", "instanceId", … fields.
        auto jr = Json::parse(lr.value());
        if (!jr.is_ok()) {
            // Unreadable blob: nothing is cached, so checkTrial() reports
            // NotStarted. A corrupted store must never look like a fresh
            // install that is entitled to a brand-new trial — only an
            // explicit startTrial() writes a trial start.
            state_.store(State::Invalid);
            return;
        }
        const Json& j = jr.value();

        std::optional<Lease> lease;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            // Decode lease (absent/undecodable → no cached lease; the trial
            // fallback below still applies).
            auto lease_node = j["lease"];
            if (lease_node.size() > 0) {
                auto lease_r = Lease::from_json(lease_node);
                if (lease_r.is_ok()) {
                    cached_lease_ = lease_r.value();
                    lease         = cached_lease_;
                }
            }
            {
                int64_t v = j["expiresAt"].as_int();
                if (v != 0) cached_expires_at_ = v;
            }
            {
                std::string v = j["instanceId"].as_string();
                if (!v.empty()) cached_instance_id_ = v;
            }
            {
                std::string v = j["licenseKey"].as_string();
                if (!v.empty()) cached_license_key_ = v;
            }
            {
                // Load lastValidatedOnline (written by save_last_validated_online_)
                int64_t v = j["lastValidatedOnline"].as_int();
                if (v != 0) {
                    cached_last_validated_online_ = v;
                    last_validated_online_atomic_.store(v);
                }
            }
            {
                // Load the local trial start written by startTrial().
                int64_t v = j["trialStart"].as_int();
                if (v != 0) cached_trial_start_ = v;
            }
            {
                // Anonymous free-tier instance id (see freeTierInstanceId()).
                std::string fid = j["freeTierInstanceId"].as_string();
                if (!fid.empty()) cached_free_tier_instance_id_ = fid;
            }
            {
                std::string hw = j["cachedHardwareId"].as_string();
                if (!hw.empty()) cached_hardware_id_ = hw;
            }
            {
                std::string kls = j["keylessLastState"].as_string();
                if (!kls.empty()) cached_keyless_last_state_ = kls;
                cached_last_keyless_ping_at_ = j["lastKeylessPingAt"].as_int();
            }
        }

        State paid = State::Invalid;
        if (lease.has_value()) {
            auto vr = verifier_.verify(*lease, now_fn_());
            paid    = derive_state_from_verify_(*lease, vr);
        }

        // Bound how long a cached lease may carry the app without server
        // contact. The lease's own 7-day TTL is the ceiling; maxOfflineDays is
        // the tenant's policy underneath it, and it was previously ignored on
        // this path — so a tenant setting 2 still got 7.
        //
        // Fail closed when the anchor is missing: a lease with no record of
        // ever having been validated online cannot be aged, and treating
        // "unknown" as "recent" is exactly the gap an attacker deletes a field
        // to create. Matches keylight-rust.
        //
        // Fail closed too when the anchor sits AHEAD of the clock beyond the
        // rollback tolerance: `now - anchor` is negative there, so the
        // `> max_age` test can never fire and the bound is silently disabled
        // for as long as the anchor stays in the future — push the clock
        // forward across one validate and the tenant's policy stops applying.
        // Within the tolerance the anchor is trusted, for the same reason
        // clock.hpp tolerates a small backward step: NTP corrections and
        // suspend/resume routinely move the clock a little, and locking out a
        // paying customer over a second of drift would be the worse bug.
        if (paid != State::Invalid && cfg_.maxOfflineDays > 0) {
            int64_t anchor;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                anchor = cached_last_validated_online_;
            }
            const int64_t now = now_fn_();
            const int64_t max_age =
                static_cast<int64_t>(cfg_.maxOfflineDays) * 86400;
            if (anchor == 0 || clock_rolled_back(anchor, now) ||
                (now - anchor) > max_age) {
                paid = State::Expired;
            }
        }

        // Paid licensing wins; the persisted local trial only fills the gap.
        state_.store(resolve_with_trial_(paid));
    }

    static State derive_state_from_verify_(const Lease& l, const VerifyResult& vr) {
        if (!vr.is_trusted()) return State::Invalid;
        if (l.status == "active") return vr.expired ? State::Expired : State::Licensed;
        // Mirrors resolve_from_lease_: a cached "fallback" lease must still
        // resolve to Limited after an offline relaunch, not re-lock to
        // Expired just because the store reload took a different path.
        if (l.status == "fallback") return State::Limited;
        return State::Expired;
    }

    // ── Trial helpers ─────────────────────────────────────────────────────

    /// Whole days remaining from a trial start timestamp; <= 0 means elapsed.
    /// Elapsed time is seconds / 86400 (matching keylight-rust check_trial),
    /// clamped at zero so a wall clock that moved backwards cannot extend the
    /// window past its configured length.
    int64_t days_left_from_(int64_t start) const {
        int64_t elapsed_secs = now_fn_() - start;
        if (elapsed_secs < 0) elapsed_secs = 0;
        int64_t days_elapsed = elapsed_secs / 86400;
        return static_cast<int64_t>(cfg_.trialDurationDays) - days_elapsed;
    }

    // ── Device identity helpers ───────────────────────────────────────────

    /// Append the anonymous free-tier id (only if one already exists — never
    /// mint one here) and machine_hash to an outgoing body.  Mirrors
    /// keylight-rust, which attaches both to activate and machine_hash to
    /// validate, so a device converting from free tier to paid is counted once
    /// rather than twice.
    void append_attribution_fields_(
        std::vector<std::pair<std::string, std::string>>& fields,
        bool include_instance_id)
    {
        if (include_instance_id) {
            std::optional<std::string> id;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                id = cached_free_tier_instance_id_;
            }
            if (id.has_value() && !id->empty()) {
                fields.push_back({"free_tier_instance_id", json_str(*id)});
            }
        }
        if (auto hash = machine_hash_()) {
            fields.push_back({"machine_hash", json_str(*hash)});
        }
    }


    /// The true hardware id: read live, written through to the store on
    /// success, falling back to the last cached value when a live read fails.
    /// Keeps machine_hash stable across a transient IOKit/registry failure.
    /// NO random fallback — nullopt means "omit machine_hash entirely".
    std::optional<std::string> cached_hardware_id_value_() {
        std::optional<std::string> live =
            hardware_id_fn_ ? hardware_id_fn_() : std::nullopt;
        if (live.has_value() && !live->empty()) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (cached_hardware_id_ != live) {
                    cached_hardware_id_ = live;
                    changed = true;
                }
            }
            if (changed) (void)save_cache_();
            return live;
        }
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cached_hardware_id_.has_value() && !cached_hardware_id_->empty()) {
            return cached_hardware_id_;
        }
        return std::nullopt;
    }

    /// Cross-SDK machine_hash from the cached hardware id, if any.
    std::optional<std::string> machine_hash_() {
        auto hw = cached_hardware_id_value_();
        if (!hw.has_value()) return std::nullopt;
        return detail::machine_hash(cfg_.tenantId, cfg_.productId, *hw);
    }

    /// Apply the local-trial and free-tier fallbacks to a state resolved from
    /// paid licensing.
    /// Priority: valid paid licence → active trial → free tier → elapsed trial
    /// → Invalid.  Only an otherwise-unusable (Invalid) paid state consults the
    /// trial, so paid licensing — including a paid Expired — always wins,
    /// mirroring keylight-rust's resolve_state() (`had_license`
    /// short-circuits the trial).
    /// Must NOT be called while holding cache_mutex_ (checkTrial() locks it).
    State resolve_with_trial_(State paid_state) const {
        if (paid_state != State::Invalid) {
            return paid_state;
        }
        switch (checkTrial()) {
            case TrialStatus::Active:
                return State::Trial;
            case TrialStatus::Expired:
                // Free tier outranks an elapsed trial: keylight-rust's
                // `_ if free_tier_enabled` arm sits AFTER the trial match, so a
                // lapsed trial drops to the free tier rather than the paywall.
                return cfg_.freeTierEnabled ? State::FreeTier : State::Expired;
            case TrialStatus::NotStarted:
                break;
        }
        return cfg_.freeTierEnabled ? State::FreeTier : State::Invalid;
    }

    /// Re-resolve the current state offline. When any paid-licensing material
    /// is cached the license flow owns the state and it is returned as-is;
    /// otherwise the persisted trial (or Invalid) decides. No network I/O.
    State resolve_current_state_() const {
        bool has_paid_material = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            has_paid_material = cached_lease_.has_value()
                || (cached_license_key_.has_value() && !cached_license_key_->empty());
        }
        if (has_paid_material) {
            return state_.load();
        }
        return resolve_with_trial_(State::Invalid);
    }

    // ── Persist helpers ───────────────────────────────────────────────────

    struct PersistData {
        // nullopt means "no lease string to write" (keep as-is)
        std::optional<std::string>       lease_json;
        std::optional<int64_t>           expires_at;
        std::optional<std::string>       instance_id;
        std::optional<std::string>       license_key;
    };

    /// Merge the supplied fields into the in-memory cache, then rewrite the
    /// whole store blob from that cache. Fields the caller did not supply keep
    /// their cached value — a partial update can no longer drop licenseKey,
    /// lastValidatedOnline or trialStart from disk.
    void persist_(const PersistData& d) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            if (d.lease_json.has_value()) {
                // Re-parse so we have the Lease struct
                auto jr = Json::parse(*d.lease_json);
                if (jr.is_ok()) {
                    auto lr = Lease::from_json(jr.value());
                    if (lr.is_ok()) {
                        cached_lease_ = lr.value();
                    }
                }
            }
            if (d.expires_at.has_value()) {
                cached_expires_at_ = *d.expires_at;
            }
            if (d.instance_id.has_value()) {
                cached_instance_id_ = *d.instance_id;
            }
            if (d.license_key.has_value()) {
                cached_license_key_ = *d.license_key;
            }
        }

        save_cache_();
    }

    /// THE serializer for the persisted blob. Every field the client keeps on
    /// disk is written here and nowhere else, so no code path (activate,
    /// validate, lease refresh, lease clearing, revoke, deactivate) can erase
    /// a field it never touched.
    /// Blob format:
    ///   {"lease":{…},"expiresAt":N,"instanceId":"…","licenseKey":"…",
    ///    "lastValidatedOnline":N,"trialStart":N}
    /// Caller must hold cache_mutex_.
    std::string build_blob_locked_() const {
        std::string blob  = "{";
        bool        first = true;
        auto append = [&](const std::string& kv) {
            if (!first) blob += ",";
            blob += kv;
            first = false;
        };

        if (cached_lease_.has_value()) {
            append("\"lease\":" + lease_to_json_(*cached_lease_));
        }
        if (cached_expires_at_.has_value()) {
            append("\"expiresAt\":" + std::to_string(*cached_expires_at_));
        }
        if (cached_instance_id_.has_value()) {
            append("\"instanceId\":" + json_str(*cached_instance_id_));
        }
        if (cached_license_key_.has_value()) {
            append("\"licenseKey\":" + json_str(*cached_license_key_));
        }
        if (cached_last_validated_online_ != 0) {
            append("\"lastValidatedOnline\":" +
                   std::to_string(cached_last_validated_online_));
        }
        if (cached_trial_start_.has_value()) {
            append("\"trialStart\":" + std::to_string(*cached_trial_start_));
        }
        if (cached_free_tier_instance_id_.has_value()) {
            append("\"freeTierInstanceId\":" +
                   json_str(*cached_free_tier_instance_id_));
        }
        if (cached_hardware_id_.has_value()) {
            append("\"cachedHardwareId\":" + json_str(*cached_hardware_id_));
        }
        if (cached_keyless_last_state_.has_value()) {
            append("\"keylessLastState\":" + json_str(*cached_keyless_last_state_));
        }
        if (cached_last_keyless_ping_at_ != 0) {
            append("\"lastKeylessPingAt\":" +
                   std::to_string(cached_last_keyless_ping_at_));
        }

        blob += "}";
        return blob;
    }

    /// Serialize the current cache and hand it to the store.
    Result<void> save_cache_() {
        std::string blob;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            blob = build_blob_locked_();
        }
        return store_.save(blob);
    }

    /// Load the stored instance_id from cache (or empty string if none).
    std::string load_instance_id_() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cached_instance_id_.has_value()) {
            return *cached_instance_id_;
        }
        return "";
    }

    /// Load the stored license key from cache (or empty string if none).
    std::string load_license_key_() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cached_license_key_.has_value()) {
            return *cached_license_key_;
        }
        return "";
    }

    // ── E2 helpers ────────────────────────────────────────────────────────

    /// True iff there is a stored license (license key in cache).
    bool has_stored_license_() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cached_license_key_.has_value() && !cached_license_key_->empty();
    }

    /// Load the last-validated-online timestamp (epoch seconds, 0 if absent).
    int64_t load_last_validated_online_() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cached_last_validated_online_;
    }

    /// Persist the last-validated-online timestamp (called after each successful validate).
    void save_last_validated_online_(int64_t t) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_last_validated_online_ = t;
        }
        last_validated_online_atomic_.store(t);
        // Rewrite the blob from the cache (best-effort; failures are non-fatal).
        save_cache_();
    }

    /// Build the JSON body for a validate request.
    /// Not const: machine_hash_() writes the cached hardware id through to the
    /// store on a successful live read.
    std::string build_validate_body_() {
        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key", json_str(load_license_key_())},
            {"instance_id", json_str(load_instance_id_())},
        };
        append_attribution_fields_(fields, /*include_instance_id=*/false);
        return build_json_(std::move(fields), true);
    }

    /// Perform a single live validate() round-trip against the server and
    /// reconcile the result, applying the offline-grace bound on any
    /// transient failure (network error, non-200, or unparseable body).
    /// Shared by refreshIfNeeded() (gated by the debounce/stale/near-expiry
    /// timer) and checkOnLaunch() (called unconditionally — no gating).
    /// Extracted verbatim from the former refreshIfNeeded() body so both
    /// call sites keep identical reconcile/grace semantics.
    Result<State> validate_and_reconcile_() {
        int64_t now      = now_fn_();
        int64_t last_lvo = load_last_validated_online_();

        // Attempt network refresh via validate()
        State before = state_.load();
        auto hr = transport_.request("POST", api_url_("validate"),
                                     json_headers_(),
                                     build_validate_body_());
        if (!hr.is_ok()) {
            // Network failure — apply offline grace
            return apply_offline_grace_(before, now, last_lvo);
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
            // 422 is the worker's definitive-rejection status for /validate
            // (revoke, deactivated instance, expired/fallback lease) — parse
            // the body instead of treating it as transient/offline-graceable.
            // Any other non-200 status (or an undecodable 422 body) falls
            // through to the existing offline-grace handling below.
            if (resp.status == 422) {
                auto rejected = handle_validate_rejection_(resp.body, now);
                if (rejected.has_value()) {
                    return Result<State>::ok(*rejected);
                }
            }
            return apply_offline_grace_(before, now, last_lvo);
        }

        // Parse and apply the validate response
        auto jr = Json::parse(resp.body);
        if (!jr.is_ok()) {
            return apply_offline_grace_(before, now, last_lvo);
        }
        const Json& j = jr.value();

        std::optional<Lease> lease;
        auto lease_node = j["lease"];
        if (lease_node.size() > 0) {
            auto lr = Lease::from_json(lease_node);
            if (lr.is_ok()) {
                lease = lr.value();
            }
        }

        std::optional<int64_t> expires_at;
        {
            int64_t v = j["license_expires_at"].as_int();
            if (v != 0) expires_at = v;
        }

        if (lease.has_value() && verifier_.verify(*lease, now_fn_()).is_trusted()) {
            std::string lease_json = lease_to_json_(*lease);
            std::optional<std::string> iid;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                iid = cached_instance_id_;
            }
            persist_({lease_json, expires_at, iid});
            // Update last_validated_online timestamp
            save_last_validated_online_(now);
        }

        // Paid licensing wins; an unusable result falls back to the local
        // trial instead of dropping a trialling user to Invalid.
        State new_state = resolve_with_trial_(resolve_from_lease_(lease));
        set_state_(new_state);
        return Result<State>::ok(new_state);
    }

    /// Apply offline grace logic when a network call fails.
    /// Grace only keeps Licensed when the cached lease is NOT yet expired (raw
    /// expiresAt, no skew tolerance) AND we are within maxOfflineDays of the
    /// last successful online validation.  If the lease has passed its own
    /// expiry timestamp the offline grace window is irrelevant — an expired
    /// lease must downgrade regardless.
    /// Ported from keylight-rust cached_lease() + state() and C# ResolveState():
    ///   - Rust:  cached_lease() returns None when r.expired; grace is checked
    ///            first, then expiry.  Absent cached_lease → Expired/Invalid.
    ///   - C#:    ResolveState "stale active lease: fall through to Expired"
    ///            — the offline-grace path must not override that.
    Result<State> apply_offline_grace_(State before, int64_t now, int64_t last_lvo) {
        // Check whether the cached lease has passed its own raw expiresAt.
        // Grace cannot rescue a genuinely expired lease.
        bool lease_raw_expired = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (!cached_lease_.has_value()) {
                // No cached lease — nothing to grace; fall through to downgrade.
                lease_raw_expired = true;
            } else {
                lease_raw_expired = (now > cached_lease_->expiresAt);
            }
        }

        if (lease_raw_expired) {
            // Lease is genuinely expired (or absent) — downgrade regardless of grace.
            State current = state_.load();
            if (current == State::Licensed) {
                set_state_(State::Expired);
                return Result<State>::ok(State::Expired);
            }
            return Result<State>::ok(current);
        }

        // Lease is not yet expired.  Only apply grace if maxOfflineDays > 0.
        if (cfg_.maxOfflineDays <= 0) {
            // No grace configured — keep existing state (mirrors C# MaxOfflineDays=0).
            return Result<State>::ok(state_.load());
        }

        // Check if within the offline grace window.
        if (last_lvo > 0) {
            int64_t offline_secs = now - last_lvo;
            int64_t grace_secs   = static_cast<int64_t>(cfg_.maxOfflineDays) * 86400LL;
            if (offline_secs <= grace_secs) {
                // Within grace — lease is valid + not expired → keep Licensed.
                return Result<State>::ok(state_.load());
            }
        }

        // Beyond grace (or never validated online): downgrade.
        // A Licensed state that has run out of grace degrades to Expired.
        State current = state_.load();
        if (current == State::Licensed) {
            set_state_(State::Expired);
            return Result<State>::ok(State::Expired);
        }
        return Result<State>::ok(current);
    }

#ifdef KEYLIGHT_ENABLE_TEST_SEAMS
public:
    /// TEST SEAM — compiled only for this repo's own test target, never in a
    /// shipped build. Number of retired workers awaiting a reap.
    ///
    /// It exists because without it take_exited_() has no coverage at all: a
    /// reaper that silently stops working leaks a thread stack per stop/start
    /// cycle, and every black-box symptom of that needs hundreds of cycles and
    /// hundreds of megabytes to observe. A review found the previous
    /// black-box attempt asserted literally nothing.
    std::size_t retiredWorkerCount_ForTest() const {
        std::lock_guard<std::mutex> lock(av_mutex_);
        return av_retired_.size();
    }
private:
#endif

    /// The auto-validation worker. Runs until the epoch it was spawned with
    /// is no longer current — which is how both stopAutoValidation() and
    /// ~Client() retire it, without either of them joining.
    void av_loop_(uint64_t epoch) {
        const auto interval =
            std::chrono::milliseconds(cfg_.autoValidationIntervalMs);
        std::unique_lock<std::mutex> lk(av_mutex_);
        while (av_epoch_ == epoch) {
            // Interruptible wait: wakes immediately when the epoch moves.
            av_cv_.wait_for(lk, interval, [this, epoch]{ return av_epoch_ != epoch; });
            if (av_epoch_ != epoch) break;
            // Release the mutex while calling refreshIfNeeded so it can
            // acquire cache_mutex_ / notify_mutex_ / listeners_mutex_ without
            // deadlock, and so start/stop stay responsive during a round trip.
            lk.unlock();
            // Contain it, for the same reason a listener is contained: an
            // exception escaping a thread's entry point is std::terminate, and
            // aborting a DAW from a licensing SDK's background thread is not a
            // failure mode we get to have. Nothing documents Transport or
            // LicenseStore as non-throwing — only now_fn is — and the SDK's own
            // JuceUrlTransport builds juce::String and std::string with no
            // guard, so bad_alloc alone reaches here through our code.
            //
            // The catch belongs HERE and not around av_loop_: catching outside
            // would leave av_current_.thread joinable with a dead thread behind
            // it, and startAutoValidation() would then no-op forever.
            try {
                refreshIfNeeded();
            } catch (...) {
                // Swallowed. There is nowhere to report it from a background
                // thread, and the next cycle retries.
            }
            lk.lock();
        }
    }

    /// Move out every retired worker that has finished its loop. Caller holds
    /// av_mutex_. Joining these outside the lock cannot block on anything a
    /// user controls, which is why the flag exists rather than just joining.
    std::vector<AvWorker> take_exited_() {
        std::vector<AvWorker> exited, still_running;
        // Reserve BEFORE moving anything: a bad_alloc partway through would
        // leave joinable threads in a vector that is about to unwind.
        exited.reserve(av_retired_.size());
        still_running.reserve(av_retired_.size());
        for (auto& w : av_retired_) {
            if (w.done && w.done->load()) exited.push_back(std::move(w));
            else                          still_running.push_back(std::move(w));
        }
        av_retired_ = std::move(still_running);
        return exited;
    }

    /// Set the raw resolved state, then let notify_() decide whether that is
    /// a change worth reporting. state_ stays the raw resolution — it is what
    /// gets persisted reasoning and what the guard is applied *to*.
    ///
    /// Anything that changes state_ AFTER construction must go through here.
    /// The bare state_.store() calls in refresh_state_from_store_() are safe
    /// only because its sole caller is the constructor, where nobody can have
    /// subscribed yet; a second caller would silently swallow a transition.
    void set_state_(State new_state) {
        state_.store(new_state);
        notify_();
    }

    /// Fire listeners when what state() reports has changed since the last
    /// event. This is the SDK's event channel; nothing else calls listeners.
    ///
    /// It reports the GUARDED state for the same reason every other read
    /// point does. A transition resolved while the clock is untrusted would
    /// otherwise deliver, say, Trial to a subscriber while state() answered
    /// Invalid — the paywall driven by subscribe() and the one driven by
    /// state() would disagree, which is the exact split the guard exists to
    /// close.
    ///
    /// Deduping on the REPORTED value, not the raw one, is what gives the
    /// guard an event of its own. A clock rolled back mid-session changes no
    /// raw state, so a raw-value dedupe would never fire and a host that
    /// caches the last event — JUCE's audio-thread snapshot does exactly
    /// that — would sit on a stale Licensed for the rest of the session.
    /// refreshIfNeeded() and validate() both poll this, and
    /// startAutoValidation() ticks refreshIfNeeded(), so both the rollback and
    /// the later correction reach subscribers without any new machinery.
    /// THREAD SAFETY: the ORDER of events is fixed under notify_mutex_; the
    /// DELIVERY of them happens with no lock held.
    ///
    /// Both halves are load-bearing, and each was a shipped bug at some point
    /// in this branch:
    ///
    /// Publishing the dedupe and then delivering outside any lock lets two
    /// notifiers interleave — both pass the dedupe, then deliver in whatever
    /// order the scheduler picks, and because the dedupe is already satisfied
    /// no later poll corrects it. The subscriber is left permanently
    /// contradicting state(), including stale-Licensed on a rolled-back clock,
    /// which is the very fail-open this guard exists to close.
    ///
    /// Holding the lock across the callbacks fixes that and buys a deadlock.
    /// It puts an SDK-internal lock into the APPLICATION's lock-order graph:
    /// a callback that takes the app's own model mutex — the most obvious
    /// thing a state-change handler does — deadlocks against any thread that
    /// holds that mutex across a Client call, and refreshIfNeeded() on
    /// focus/resume is exactly that. No contract can rescue it, because the
    /// rule would have to be "your callback must not touch any lock any
    /// thread holds across any Client call", which nobody can audit.
    ///
    /// So: append to pending_ under the lock, atomically with the dedupe, and
    /// let ONE thread hold the delivery baton and drain the queue with no lock
    /// held. Order is total, no application lock can invert against ours, and
    /// a callback may re-enter the Client freely — it just queues.
    ///
    /// Consequence to know: validate()/refreshIfNeeded() can return before an
    /// event queued concurrently has been delivered by the thread holding the
    /// baton. Delivery is ordered, not synchronous.
    void notify_() {
        {
            std::lock_guard<std::mutex> lock(notify_mutex_);
            const State reported = state();
            if (last_reported_.load() == reported) return;
            // Order is decided HERE, atomically with the dedupe. Everything
            // after this point may run in any order on any thread.
            //
            // Push BEFORE consuming the dedupe: a bad_alloc from push_back
            // must not leave last_reported_ claiming an event nobody queued,
            // which would lose that transition permanently.
            pending_.push_back(reported);
            last_reported_.store(reported);
            if (delivering_) return;   // another thread owns the baton
            delivering_ = true;
        }

        // The baton is a plain bool, so unlike a lock_guard it does NOT release
        // on unwind. Anything that throws past this point — a listener, or a
        // bad_alloc from the copies below — would leave delivering_ stuck true
        // and every later notify_() would take the "someone else is draining"
        // exit. The channel would be dead for the life of the Client while
        // state() kept moving: a caching subscriber like JUCE's audio-thread
        // snapshot would hold its last value forever, which on a rolled-back
        // clock is a permanent fail-open.
        //
        // It is ONLY the unwind net. The normal exit disarms it and hands the
        // baton back atomically with the emptiness check that makes doing so
        // safe — see the loop. Clearing delivering_ in a SECOND critical
        // section would open a window where the queue is empty but the baton
        // is still held: a notifier arriving there pushes, sees delivering_,
        // and returns believing we will drain it, and we then return having
        // already decided the queue was empty. That event is stranded, and
        // because the dedupe consumed its value no later poll re-queues it.
        struct BatonGuard {
            Client* self;
            bool    armed = true;
            ~BatonGuard() {
                if (!armed) return;
                std::lock_guard<std::mutex> lock(self->notify_mutex_);
                self->delivering_ = false;
            }
        } baton_guard{this};

        for (;;) {
            State ev;
            {
                std::lock_guard<std::mutex> lock(notify_mutex_);
                if (pending_.empty()) {
                    delivering_        = false;   // atomic with the check
                    baton_guard.armed  = false;
                    return;
                }
                ev = pending_.front();
                pending_.erase(pending_.begin());
            }

            // Copy the callbacks out so a listener can unsubscribe from inside
            // its own callback without invalidating the iteration. A bad_alloc
            // here strands whatever is left in pending_ — the guard hands the
            // baton back and nothing re-queues those events. OOM-only, and
            // strictly better than the wedged channel it replaced.
            std::vector<std::function<void(State)>> cbs;
            {
                std::lock_guard<std::mutex> lock(listeners_mutex_);
                cbs.reserve(listeners_.size());
                for (const auto& l : listeners_) {
                    cbs.push_back(l.cb);
                }
            }
            for (const auto& cb : cbs) {
                // Contain each listener. One that throws must not cost the
                // others their event, and must not propagate out of an SDK
                // call the integrator made for an unrelated reason.
                try {
                    cb(ev);   // NO lock held here. This is the whole point.
                } catch (...) {
                    // Swallowed deliberately: see the LISTENER CONTRACT on
                    // subscribe(). There is nowhere to report it — notify_()
                    // runs on whatever thread moved the state.
                }
            }
        }
    }

    /// Remove listener with the given id (called from Subscription::unsubscribe).
    void remove_listener_(uint64_t id) {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listeners_.erase(
            std::remove_if(listeners_.begin(), listeners_.end(),
                           [id](const Listener& l){ return l.id == id; }),
            listeners_.end());
    }

    // Allow Subscription to call remove_listener_
    friend class Subscription;
};

// ---------------------------------------------------------------------------
// Subscription::unsubscribe — defined after Client is complete
// ---------------------------------------------------------------------------
inline void Subscription::unsubscribe() {
    if (client_) {
        client_->remove_listener_(id_);
        client_ = nullptr;
    }
}

} // namespace keylight
