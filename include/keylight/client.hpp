#pragma once
// keylight/client.hpp — Client state machine: activate/validate/deactivate.
// Ported from keylight-rust keylight/src/client.rs and
//            keylight-csharp src/Keylight/Keylight.cs
//
// URL pattern:  {baseUrl}/{tenantId}/{productId}/{action}
// Activate:     POST /{tenantId}/{productId}/activate
// Validate:     POST /{tenantId}/{productId}/validate
// Deactivate:   POST /{tenantId}/{productId}/deactivate
//
// Thread-safety: state() reads a std::atomic<State> — audio-thread safe.
//                hasEntitlement / cachedLicenseExpiresAt are guarded by a mutex.

#include "config.hpp"
#include "lease.hpp"
#include "result.hpp"
#include "store.hpp"
#include "transport.hpp"
#include "verifier.hpp"
#include "version.hpp"
#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <future>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
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
        } else if (!lease.has_value() && activated) {
            // Server said activated=true but sent no lease — treat as Licensed
            // without a local lease; persist what we have.
            persist_({std::nullopt, expires_at, instance_id, key});
            new_state = State::Licensed;
        }

        state_.store(new_state);
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
        }

        State new_state = resolve_from_lease_(lease);
        state_.store(new_state);
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
            cached_lease_        = std::nullopt;
            cached_expires_at_   = std::nullopt;
            cached_instance_id_  = std::nullopt;
            cached_license_key_  = std::nullopt;
        }
        state_.store(State::Invalid);
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
};

} // namespace keylight
