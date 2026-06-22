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
#include "lease.hpp"
#include "result.hpp"
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
};

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
                 []{ return static_cast<int64_t>(std::time(nullptr)); })
    {}

    // Testable constructor — inject a deterministic clock.
    // now_fn() must return Unix epoch seconds as int64_t.
    Client(Config                   cfg,
           Transport&               transport,
           LicenseStore&            store,
           std::function<int64_t()> now_fn)
        : cfg_(std::move(cfg))
        , transport_(transport)
        , store_(store)
        , now_fn_(std::move(now_fn))
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
        std::string body = build_json_({
            {"license_key",   json_str(key)},
            {"instance_name", json_str("device")},
        }, true /*include telemetry*/);

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

        set_state_(new_state);
        return Result<State>::ok(new_state);
    }

    /// Validate the stored license online.  Returns the resulting State.
    Result<State> validate() {
        // Need license_key and instance_id from cache (Worker requires both)
        std::string license_key  = load_license_key_();
        std::string instance_id  = load_instance_id_();

        std::string body = build_json_({
            {"license_key", json_str(license_key)},
            {"instance_id", json_str(instance_id)},
        }, true /*include telemetry*/);

        std::string url = api_url_("validate");
        auto hr = transport_.request("POST", url, json_headers_(), body);
        if (!hr.is_ok()) {
            // Network failure: keep existing state
            return Result<State>::ok(state_.load());
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
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

        State new_state = resolve_from_lease_(lease);
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

        // Clear store
        auto cr = store_.clear();
        if (!cr.is_ok()) {
            return cr;
        }

        // Clear in-memory cache
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_lease_                  = std::nullopt;
            cached_expires_at_             = std::nullopt;
            cached_instance_id_            = std::nullopt;
            cached_license_key_            = std::nullopt;
            cached_last_validated_online_  = 0;
        }
        set_state_(State::Invalid);
        return Result<void>::ok();
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
    /// then call refreshIfNeeded() (which may hit the network if stale/near-expiry).
    /// If there is no cached lease, state stays as-is (Invalid/initial).
    /// Ported from keylight-rust check_on_launch() and keylight-csharp CheckOnLaunchAsync().
    Result<State> checkOnLaunch() {
        // The cache is already primed on construction via refresh_state_from_store_().
        // Call refreshIfNeeded to make a network call if the cached data is stale.
        if (has_stored_license_()) {
            auto r = refreshIfNeeded();
            if (!r.is_ok()) {
                // If refreshIfNeeded fails hard (non-network), propagate.
                // Network failures are handled inside refreshIfNeeded (grace).
                return r;
            }
        }
        return Result<State>::ok(state_.load());
    }

    /// Apply the timer model: refresh debounce 5min, stale 6h, near-expiry 24h.
    /// If a refresh is due, calls validate(); otherwise returns current state.
    /// On a network failure within maxOfflineDays grace window, keeps Licensed.
    /// Ported from keylight-rust refresh_if_needed() and keylight-csharp RefreshIfNeededAsync().
    Result<State> refreshIfNeeded() {
        if (!has_stored_license_()) {
            return Result<State>::ok(state_.load());
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
            // HTTP error — treat like network failure for grace purposes
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

        State new_state = resolve_from_lease_(lease);
        set_state_(new_state);
        return Result<State>::ok(new_state);
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

    static std::map<std::string, std::string> json_headers_() {
        return {
            {"Content-Type", "application/json"},
        };
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
    // If include_telemetry is true, appends sdk_version and platform.
    std::string build_json_(
        std::vector<std::pair<std::string, std::string>> fields,
        bool include_telemetry) const
    {
        if (include_telemetry) {
            fields.push_back({"sdk_version", json_str(KEYLIGHT_SDK_VERSION)});
            fields.push_back({"platform",    json_str(detail::current_platform())});
            if (!cfg_.appVersion.empty()) {
                fields.push_back({"app_version", json_str(cfg_.appVersion)});
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
        // "lease", "expiresAt", "instanceId" fields.
        auto jr = Json::parse(lr.value());
        if (!jr.is_ok()) {
            state_.store(State::Invalid);
            return;
        }
        const Json& j = jr.value();

        // Decode lease
        auto lease_node = j["lease"];
        if (lease_node.size() == 0) {
            state_.store(State::Invalid);
            return;
        }
        auto lease_r = Lease::from_json(lease_node);
        if (!lease_r.is_ok()) {
            state_.store(State::Invalid);
            return;
        }

        std::lock_guard<std::mutex> lock(cache_mutex_);
        cached_lease_ = lease_r.value();

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

        // Don't hold mutex while computing state
        Lease lease_copy = *cached_lease_;
        // release lock before verify (not needed but cleaner)
        // Actually we already hold it — that's fine, verifier doesn't touch mutex
        auto vr = verifier_.verify(lease_copy, now_fn_());
        State s = derive_state_from_verify_(lease_copy, vr);
        state_.store(s);
    }

    static State derive_state_from_verify_(const Lease& l, const VerifyResult& vr) {
        if (!vr.is_trusted()) return State::Invalid;
        if (l.status == "active") return vr.expired ? State::Expired : State::Licensed;
        return State::Expired;
    }

    // ── Persist helpers ───────────────────────────────────────────────────

    struct PersistData {
        // nullopt means "no lease string to write" (keep as-is)
        std::optional<std::string>       lease_json;
        std::optional<int64_t>           expires_at;
        std::optional<std::string>       instance_id;
        std::optional<std::string>       license_key;
    };

    /// Write a blob to the store in the format we read back.
    /// Blob format: {"lease":{...},"expiresAt":N,"instanceId":"..."}
    void persist_(const PersistData& d) {
        // Build the storage blob
        std::string blob = "{";

        if (d.lease_json.has_value()) {
            blob += "\"lease\":" + *d.lease_json;
        }

        if (d.expires_at.has_value()) {
            if (blob.size() > 1) blob += ",";
            blob += "\"expiresAt\":" + std::to_string(*d.expires_at);
        }

        if (d.instance_id.has_value()) {
            if (blob.size() > 1) blob += ",";
            blob += "\"instanceId\":" + json_str(*d.instance_id);
        }

        if (d.license_key.has_value()) {
            if (blob.size() > 1) blob += ",";
            blob += "\"licenseKey\":" + json_str(*d.license_key);
        }

        blob += "}";

        store_.save(blob);

        // Update in-memory cache
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
        // Also write into the store blob. We reload the existing blob and patch it.
        // This is a best-effort update; failures are non-fatal.
        auto lr = store_.load();
        if (!lr.is_ok() || lr.value().empty()) return;
        // Append/overwrite the lastValidatedOnline field by rebuilding the blob.
        // Simple approach: strip trailing '}' and append the key.
        std::string blob = lr.value();
        if (!blob.empty() && blob.back() == '}') {
            blob.pop_back();
            blob += ",\"lastValidatedOnline\":" + std::to_string(t) + "}";
            store_.save(blob);
        }
    }

    /// Build the JSON body for a validate request.
    std::string build_validate_body_() const {
        return build_json_({
            {"license_key", json_str(load_license_key_())},
            {"instance_id", json_str(load_instance_id_())},
        }, true);
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
