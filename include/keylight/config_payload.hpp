#pragma once
// keylight/config_payload.hpp — canonical signing payload for GET /config.
//
// WHY THIS EXISTS
//
// The v3 lease is Ed25519-signed, so pointing an app at a fake server mints no
// licence. `GET /config` carried no signature, which left the same trick able
// to mint an unlimited TRIAL: redirect the API host with a hosts entry or a
// local proxy and answer with {"trial_duration_days": 3650}. No binary
// patching, no reverse engineering. Signing the config closes that.
//
// It does NOT defeat a patched binary — that binary can drop the check like
// any other. It raises the cheapest attack from editing a hosts file to
// patching and re-signing an app. Stopping the patched case needs the server
// to refuse (a server-side trial ledger keyed to machine_hash), not this.
//
// FORMAT — this is a cross-SDK protocol contract
//
//   cfg1|{kid}|{tenantId}|{productId}|{issuedAt}|{expiresAt}|{trialDays}|{freeTier}
//
// Pipe-delimited and fixed-order, exactly like lease.hpp's v3 payload, so
// there is no JSON canonicalisation for five SDKs to disagree about.
// `freeTier` renders as the literal `true` or `false`.
//
// tenantId and productId are inside the signature deliberately. Without them a
// config legitimately signed for one product replays against another under the
// same tenant keyset — including a product whose trial is meant to be zero.
//
// Changing these bytes is a protocol break: the worker and the Rust, Swift, JS
// and C# SDKs must all produce and accept the same string, or they will
// disagree about the same tenant. The literal expectations in
// tests/test_client.cpp are the guard.

#include <cstdint>
#include <string>

namespace keylight {

inline std::string config_canonical_payload(const std::string& kid,
                                            const std::string& tenantId,
                                            const std::string& productId,
                                            int64_t            issuedAt,
                                            int64_t            expiresAt,
                                            int                trialDurationDays,
                                            bool               freeTierEnabled)
{
    return "cfg1|" + kid + "|" + tenantId + "|" + productId
         + "|" + std::to_string(issuedAt)
         + "|" + std::to_string(expiresAt)
         + "|" + std::to_string(trialDurationDays)
         + "|" + (freeTierEnabled ? "true" : "false");
}

} // namespace keylight
