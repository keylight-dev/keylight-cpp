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
    int autoValidationIntervalMs = 1'800'000; // 30 min
};

} // namespace keylight
