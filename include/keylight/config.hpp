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

    int         maxOfflineDays     = 7;
    std::string keyPrefix;
    int         trialDurationDays  = 0;
    std::string apiBaseUrl         = "https://api.keylight.dev";
};

} // namespace keylight
