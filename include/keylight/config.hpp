#pragma once
#include <map>
#include <string>

namespace keylight {

struct Config {
    std::string tenantId;
    std::string productId;
    std::string sdkKey;

    // Map of keyId → base64-encoded Ed25519 public key (32 bytes)
    std::map<std::string, std::string> trustedKeys;

    int         maxOfflineDays     = 15;
    std::string keyPrefix;
    int         trialDurationDays  = 0;
    // Free tier: when true, a device with no license and no active trial
    // resolves State::FreeTier instead of Invalid/Expired.  Parity with
    // keylight-rust Config::free_tier_enabled.
    bool        freeTierEnabled    = false;
    std::string apiBaseUrl         = "https://api.keylight.dev";
    std::string appVersion;        // optional; sent as telemetry in activate/validate

    // Interval between background auto-validation ticks (milliseconds).
    // Default is 30 minutes (1 800 000 ms).  Set a smaller value in tests
    // as a deterministic seam — the background thread uses this as its
    // interruptible wait timeout.
    //
    // A non-positive value is clamped to 1 ms. Left unclamped it would make
    // the wait return immediately and turn the worker into a busy-spin over
    // refreshIfNeeded(); 1 ms is still a bad interval, but it is a rate.
    int autoValidationIntervalMs = 1'800'000; // 30 min

    // ── Fields below were added in 0.2.0, after the struct's shape shipped ─
    //
    // APPENDED, never inserted. Positional aggregate initialisation —
    // `Config cfg{"tenant", "product", ...}` — binds by position, so slotting
    // a new field into the middle silently shifts every value after it or
    // fails to compile. Same rule the State enum documents twice for the same
    // reason. Add new fields HERE, at the end.

    // Require the server-owned product settings (trial_duration_days,
    // free_tier_enabled) to arrive Ed25519-signed, on every route that
    // carries them: GET /config, validate and the keyless beacon.
    //
    //   false (default): the fields are applied as sent and the signature
    //                    fields, if present, are not consulted at all.
    //   true:            a body must carry both fields, a kid in trustedKeys
    //                    and a signature that verifies, inside its validity
    //                    window. Unsigned, partial or bad-signature bodies
    //                    are dropped (and fetchConfig() reports an error).
    //
    // Same rule as the Swift, JS, C# and Rust SDKs. The worker signs on all
    // three routes since 2026-09-06, but ONLY for products with a trial length
    // configured in the dashboard — so leave this off unless you know your
    // product is signed, or every install stops learning its settings.
    // Enabling it also freezes the settings on old builds when the signing
    // key rotates, because trustedKeys are compiled in.
    bool        requireSignedConfig = false;

    // Interval between anonymous keyless-beacon heartbeats (milliseconds).
    // 0 disables the heartbeat entirely.
    //
    // The beacon itself is debounced to one report per 24h per state, so this
    // interval only decides how promptly a state CHANGE is noticed, not how
    // much traffic is sent. Six hours means a device that converts or lapses
    // is reflected within a quarter day without the app being relaunched.
    //
    // The thread is not spawned by the constructor and its first tick is at
    // +interval, never immediate — an AU/VST3 host instantiating and
    // discarding plugin instances during a scan must not pay for either.
    int         keylessHeartbeatIntervalMs = 6 * 60 * 60 * 1000;  // 6h
};

} // namespace keylight
