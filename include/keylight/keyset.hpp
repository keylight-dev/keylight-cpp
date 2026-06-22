#pragma once
// keylight/keyset.hpp — fetchKeyset: transport-agnostic keyset fetcher
//
// Fetches the Ed25519 public keys published by a Keylight tenant at:
//   GET {baseUrl}/{tenantId}/.well-known/keylight-keys
//
// Response shape:
//   { "primary_kid": "...", "keys": [ { "kid": "...", "alg": "ed25519",
//                                       "public_key": "<base64>" } ] }
//
// Returns a map kid → public_key (base64) for all entries in the keys array.
// Non-200 or JSON parse error → Result::err.

#include "transport.hpp"
#include "result.hpp"
#include "json.hpp"

#include <map>
#include <string>

namespace keylight {

/// Fetch the Ed25519 public keys for a tenant.
///
/// @param transport  Any Transport implementation (HttplibTransport in prod,
///                   FakeTransport in unit tests).
/// @param baseUrl    Root API URL, e.g. "https://api.keylight.dev"
/// @param tenantId   Tenant slug, e.g. "keylight-notes-demo"
/// @returns          Map of kid → base64 public_key, or an error.
inline Result<std::map<std::string, std::string>>
fetchKeyset(Transport& transport,
            const std::string& baseUrl,
            const std::string& tenantId)
{
    // Build URL: strip trailing slash from baseUrl
    std::string base = baseUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string url = base + "/" + tenantId + "/.well-known/keylight-keys";

    auto hr = transport.request("GET", url, {}, "");
    if (!hr.is_ok()) {
        return Result<std::map<std::string, std::string>>::err(hr.error());
    }
    if (hr.value().status != 200) {
        return Result<std::map<std::string, std::string>>::err(
            {ErrorCode::Http,
             "fetchKeyset HTTP " + std::to_string(hr.value().status)});
    }

    auto jr = Json::parse(hr.value().body);
    if (!jr.is_ok()) {
        return Result<std::map<std::string, std::string>>::err(
            {ErrorCode::BadResponse, "fetchKeyset: invalid JSON"});
    }
    const Json& j = jr.value();

    auto keys_node = j["keys"];
    if (!keys_node.is_array()) {
        return Result<std::map<std::string, std::string>>::err(
            {ErrorCode::BadResponse, "fetchKeyset: missing 'keys' array"});
    }

    std::map<std::string, std::string> result;
    for (size_t i = 0; i < keys_node.size(); ++i) {
        const Json& entry = keys_node.at(i);
        std::string kid    = entry["kid"].as_string();
        std::string pubkey = entry["public_key"].as_string();
        if (kid.empty() || pubkey.empty()) continue;
        result[kid] = pubkey;
    }

    if (result.empty()) {
        return Result<std::map<std::string, std::string>>::err(
            {ErrorCode::BadResponse, "fetchKeyset: no valid keys found"});
    }

    return Result<std::map<std::string, std::string>>::ok(std::move(result));
}

} // namespace keylight
