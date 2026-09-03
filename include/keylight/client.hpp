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
// Thread-safety: state() reads a std::atomic<State> — audio-thread safe.
//                hasEntitlement / cachedLicenseExpiresAt / listener list are
//                guarded by a mutex.

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
#include <ctime>
#include <future>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
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
    Expired,    // trusted lease expired, or license status "expired"/"fallback"
    Invalid,    // no trusted lease, no active trial
    FreeTier,   // no license and no trial, but the product offers a free tier.
                // Appended last on purpose: renumbering the values above would
                // break any integrator that persisted a State as an integer.
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
    }

    // Destructor: stops and joins any running auto-validation thread so the
    // thread cannot outlive the Client (no detached threads, no std::terminate).
    ~Client() {
        stopAutoValidation();
    }

    // ── Sync API ──────────────────────────────────────────────────────────

    /// Activate a license key.  Returns the resulting State.
    /// On an unrecognised/invalid lease the store is NOT updated and
    /// State::Invalid is returned (no exception thrown).
    Result<State> activate(const std::string& key) {
        // Build activate request body
        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key",   json_str(key)},
            {"instance_name", json_str("device")},
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
                "activate HTTP " + std::to_string(resp.status)});
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
            return Result<State>::ok(state_.load());
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
        return Result<State>::ok(new_state);
    }

    /// Validate the stored license online.  Returns the resulting State.
    Result<State> validate() {
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
            return Result<State>::ok(state_.load());
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
                    return Result<State>::ok(*rejected);
                }
            }
            return Result<State>::ok(state_.load());
        }

        auto jr = Json::parse(resp.body);
        if (!jr.is_ok()) {
            return Result<State>::ok(state_.load());
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
        return Result<State>::ok(new_state);
    }

    /// Deactivate this device.  Clears the store regardless of network outcome.
    Result<void> deactivate() {
        std::string instance_id = load_instance_id_();

        if (!instance_id.empty()) {
            std::string body = build_json_({
                {"instance_id", json_str(instance_id)},
            }, false);
            std::string url = api_url_("deactivate");
            // Best-effort: ignore network errors (mirror Rust/C# behaviour)
            transport_.request("POST", url, json_headers_(), body);
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
            return Result<State>::ok(state_.load());
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
        return Result<State>::ok(new_state);
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
    /// Idempotent: calling startAutoValidation() while a thread is already
    /// running is a no-op (the existing thread continues).
    void startAutoValidation() {
        std::lock_guard<std::mutex> lock(av_mutex_);
        if (av_thread_.joinable()) return; // already running — no-op

        av_stop_ = false;
        av_thread_ = std::thread([this] {
            auto interval = std::chrono::milliseconds(cfg_.autoValidationIntervalMs);
            std::unique_lock<std::mutex> lk(av_mutex_);
            while (!av_stop_) {
                // Interruptible wait: wakes immediately on stopAutoValidation().
                av_cv_.wait_for(lk, interval, [this]{ return av_stop_; });
                if (av_stop_) break;
                // Release the mutex while calling refreshIfNeeded so it can
                // acquire cache_mutex_ / listeners_mutex_ without deadlock.
                lk.unlock();
                refreshIfNeeded();
                lk.lock();
            }
        });
    }

    /// Signal the background thread to stop and join it.
    /// Idempotent: safe to call when no thread is running.
    /// Returns promptly — the thread wakes up via the condition variable
    /// instead of blocking for the full interval.
    void stopAutoValidation() {
        std::thread to_join;
        {
            std::lock_guard<std::mutex> lock(av_mutex_);
            if (!av_thread_.joinable()) return; // not running — no-op
            av_stop_ = true;
            av_cv_.notify_all();
            to_join = std::move(av_thread_); // move out before unlocking
        }
        // Join outside the lock so the worker can re-acquire av_mutex_ to exit.
        if (to_join.joinable()) to_join.join();
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
            return validate_and_reconcile_();
        }
        // No paid license: resolve the persisted local trial offline. This
        // never *starts* a trial — a DAW scanning or instantiating a plugin
        // must not consume the user's trial window; only startTrial() does
        // that, and only when the user asks for it.
        State new_state = resolve_current_state_();
        set_state_(new_state);
        return Result<State>::ok(new_state);
    }

    /// Apply the timer model: refresh debounce 5min, stale 6h, near-expiry 24h.
    /// If a refresh is due, calls validate(); otherwise returns current state.
    /// On a network failure within maxOfflineDays grace window, keeps Licensed.
    /// Ported from keylight-rust refresh_if_needed() and keylight-csharp RefreshIfNeededAsync().
    /// This in-session cadence is unchanged by the always-validate-on-launch
    /// fix: it still governs long-running hosts between launches.
    Result<State> refreshIfNeeded() {
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
            return Result<State>::ok(new_state);
        }

        int64_t now          = now_fn_();
        int64_t last_lvo     = load_last_validated_online_();
        bool    has_lvo      = (last_lvo > 0);

        // Debounce: skip if validated within the last 5 minutes
        if (has_lvo && (now - last_lvo) < REFRESH_DEBOUNCE) {
            return Result<State>::ok(state_.load());
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
            return Result<State>::ok(state_.load());
        }

        return validate_and_reconcile_();
    }

    // ── Events API ────────────────────────────────────────────────────────

    /// Register a callback for state-transition events.
    /// event: currently only "change" is defined (fires on every state transition).
    /// Returns a Subscription RAII handle; when the handle is destroyed or
    /// unsubscribe() is called, the callback is removed.
    /// Callbacks are dispatched on the calling thread; UI/audio hosts must
    /// marshal to their own thread if required.
    Subscription on(const std::string& /*event*/,
                    std::function<void(State)> cb)
    {
        return subscribe(std::move(cb));
    }

    /// Subscribe to all state transitions. Returns a Subscription RAII handle.
    Subscription subscribe(std::function<void(State)> cb) {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        uint64_t id = ++next_listener_id_;
        listeners_.push_back({id, std::move(cb)});
        return Subscription(this, id);
    }

    // ── Query API ─────────────────────────────────────────────────────────

    /// Current state — reads atomic; audio-thread safe.
    State state() const noexcept {
        return state_.load();
    }

    /// True iff the cached, verified lease contains the named entitlement.
    bool hasEntitlement(const std::string& feature) const {
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
    // ── Dependencies ──────────────────────────────────────────────────────
    Config                   cfg_;
    Transport&               transport_;
    LicenseStore&            store_;
    std::function<int64_t()> now_fn_;
    std::function<std::optional<std::string>()> hardware_id_fn_;
    Verifier                 verifier_;

    // ── State ─────────────────────────────────────────────────────────────
    std::atomic<State>       state_;

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

    // ── Background auto-validation ────────────────────────────────────────
    // av_mutex_ guards av_stop_ and av_thread_.
    // The worker holds a unique_lock<av_mutex_> for its wait/flag check,
    // then RELEASES it before calling refreshIfNeeded() (which acquires
    // cache_mutex_ / listeners_mutex_) to avoid deadlock.
    std::mutex              av_mutex_;
    std::condition_variable av_cv_;
    bool                    av_stop_  = false;
    std::thread             av_thread_;

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
        return headers;
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
        // "expired", "fallback", or anything else from a trusted lease → Expired
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
                if (v != 0) cached_last_validated_online_ = v;
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
        // Paid licensing wins; the persisted local trial only fills the gap.
        state_.store(resolve_with_trial_(paid));
    }

    static State derive_state_from_verify_(const Lease& l, const VerifyResult& vr) {
        if (!vr.is_trusted()) return State::Invalid;
        if (l.status == "active") return vr.expired ? State::Expired : State::Licensed;
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

    /// Set state_ and fire event listeners if the state changed.
    void set_state_(State new_state) {
        State old_state = state_.exchange(new_state);
        if (old_state == new_state) return; // no transition — no event

        // Collect callbacks under the lock, fire outside it to avoid re-entrancy.
        std::vector<std::function<void(State)>> cbs;
        {
            std::lock_guard<std::mutex> lock(listeners_mutex_);
            cbs.reserve(listeners_.size());
            for (const auto& l : listeners_) {
                cbs.push_back(l.cb);
            }
        }
        for (const auto& cb : cbs) {
            cb(new_state);
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
