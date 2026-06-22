#pragma once
// keylight/verifier.hpp — Ed25519 lease verifier with clock-skew tolerance.
// Ported from keylight-rust keylight/src/verifier.rs

#include "result.hpp"
#include "lease.hpp"
#include "ed25519.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace keylight {

struct VerifyResult {
    bool kidKnown       = false;
    bool signatureValid = false;
    bool expired        = false;

    /// The lease is signed by a known, trusted key (independent of expiry).
    bool is_trusted() const { return kidKnown && signatureValid; }
};

/// Decode base64, tolerating both standard (+/) and URL-safe (-_) alphabets,
/// and tolerating missing padding. Returns empty string on failure.
inline std::string b64_decode_flexible(const std::string& s) {
    // Normalize to standard base64 with padding
    std::string norm;
    norm.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue; // skip whitespace
        if (c == '-') { norm.push_back('+'); continue; }
        if (c == '_') { norm.push_back('/'); continue; }
        norm.push_back(c);
    }
    // Add padding if needed
    size_t pad = norm.size() % 4;
    if (pad != 0) norm.append(4 - pad, '=');
    return base64_decode(norm);
}

class Verifier {
public:
    /// @param trustedKeys  map of kid → raw Ed25519 public key (base64-encoded 32 bytes)
    /// @param skewSeconds  clock-skew tolerance in seconds (default 300)
    Verifier(std::map<std::string, std::string> trustedKeys, int skewSeconds = 300)
        : trustedKeys_(std::move(trustedKeys)), skewSeconds_(skewSeconds) {}

    /// Verify a lease against the trusted key set.
    /// Semantics match keylight-rust verify_lease():
    ///   - expired    = now > expiresAt + skewSeconds  (computed regardless of kid)
    ///   - kidKnown   = trustedKeys.count(kid) > 0
    ///   - signatureValid = kidKnown && Ed25519-verify(canonical_payload, sig, pubkey)
    ///                      (false if kid unknown or crypto fails)
    VerifyResult verify(const Lease& lease, int64_t now) const {
        VerifyResult r;
        r.expired   = now > lease.expiresAt + static_cast<int64_t>(skewSeconds_);
        r.kidKnown  = trustedKeys_.count(lease.kid) > 0;

        if (!r.kidKnown) {
            r.signatureValid = false;
            return r;
        }

        // Attempt signature verification; any failure → signatureValid = false
        r.signatureValid = [&]() -> bool {
            const std::string& pubB64 = trustedKeys_.at(lease.kid);
            std::string pk_bytes = b64_decode_flexible(pubB64);
            if (pk_bytes.size() != 32) return false;

            std::string sig_bytes = b64_decode_flexible(lease.signature);
            if (sig_bytes.size() != 64) return false;

            // Build the typed arrays required by ed25519_verify
            std::array<uint8_t, 32> pubkey;
            for (int i = 0; i < 32; ++i)
                pubkey[i] = static_cast<uint8_t>(pk_bytes[i]);

            std::array<uint8_t, 64> sig;
            for (int i = 0; i < 64; ++i)
                sig[i] = static_cast<uint8_t>(sig_bytes[i]);

            // Build canonical payload
            std::string payload = canonical_payload(lease);
            const auto* msg     = reinterpret_cast<const uint8_t*>(payload.data());
            size_t      msg_len = payload.size();

            return ed25519_verify(msg, msg_len, sig, pubkey);
        }();

        return r;
    }

private:
    std::map<std::string, std::string> trustedKeys_;
    int                                skewSeconds_;
};

} // namespace keylight
