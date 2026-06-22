#pragma once
// keylight/lease.hpp — the signed v3 Lease and its canonical payload.
// Ported from keylight-rust keylight/src/lease.rs

#include "result.hpp"
#include "json.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace keylight {

struct Lease {
    std::string kid;
    std::string licenseKeyHash;
    std::string instanceId;
    int64_t     issuedAt  = 0;
    int64_t     expiresAt = 0;
    std::string status;
    std::string signature;
    std::vector<std::string> entitlements;

    /// Parse a Lease from a JSON object (as it appears in the vectors file).
    static Result<Lease> from_json(const Json& j) {
        Lease l;
        l.kid            = j["kid"].as_string();
        l.licenseKeyHash = j["licenseKeyHash"].as_string();
        l.instanceId     = j["instanceId"].as_string();
        l.issuedAt       = j["issuedAt"].as_int();
        l.expiresAt      = j["expiresAt"].as_int();
        l.status         = j["status"].as_string();
        l.signature      = j["signature"].as_string();
        // entitlements is an array of strings
        auto ents = j["entitlements"];
        size_t n = ents.size();
        for (size_t i = 0; i < n; ++i) {
            l.entitlements.push_back(ents.at(i).as_string());
        }
        if (l.kid.empty() || l.status.empty()) {
            return Result<Lease>::err({ErrorCode::BadResponse, "missing required lease fields"});
        }
        return Result<Lease>::ok(std::move(l));
    }
};

/// The exact UTF-8 payload that was signed.
/// Format: v3|kid|licenseKeyHash|instanceId|issuedAt|expiresAt|status|entitlements_csv
/// entitlements_csv = entitlements sorted ascending (lexicographic), comma-joined.
/// Empty entitlements → trailing empty string (e.g. "...active|").
inline std::string canonical_payload(const Lease& l) {
    // Sort a copy of entitlements ascending
    std::vector<std::string> ents = l.entitlements;
    std::sort(ents.begin(), ents.end());

    std::string csv;
    for (size_t i = 0; i < ents.size(); ++i) {
        if (i > 0) csv.push_back(',');
        csv += ents[i];
    }

    return "v3|" + l.kid + "|" + l.licenseKeyHash + "|" + l.instanceId
         + "|" + std::to_string(l.issuedAt) + "|" + std::to_string(l.expiresAt)
         + "|" + l.status + "|" + csv;
}

} // namespace keylight
