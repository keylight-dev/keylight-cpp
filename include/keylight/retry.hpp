#pragma once
// keylight/retry.hpp — retry policy for transient HTTP failures.
// Ported from keylight-rust keylight/src/http/retry.rs.
//
// Pure logic, deliberately separated from the transport so the policy is
// unit-tested without sleeping or networking.

#include <cstdint>
#include <optional>
#include <string>

namespace keylight {

inline constexpr uint32_t RETRY_MAX_ATTEMPTS = 3;
inline constexpr uint64_t RETRY_BASE_MS      = 500;
inline constexpr uint64_t RETRY_CAP_MS       = 4000;
// Ceiling for any sleep we will perform, including a server-supplied one.
inline constexpr uint64_t RETRY_MAX_SLEEP_MS = 3600000; // 1h

struct RetryDecision {
    bool     retry    = false;
    uint64_t delay_ms = 0;
};

// Backoff for a 1-based attempt number, without jitter (the caller adds it).
inline uint64_t backoff_ms(uint32_t attempt) {
    if (attempt == 0) return RETRY_BASE_MS;
    // Shift is bounded before it is applied: 1u << 64 is undefined behavior,
    // and the value saturates at the cap long before that anyway.
    const uint32_t shift = attempt - 1 > 20 ? 20 : attempt - 1;
    const uint64_t exp   = RETRY_BASE_MS << shift;
    return exp > RETRY_CAP_MS ? RETRY_CAP_MS : exp;
}

// Is this HTTP status worth trying again?
inline bool status_retryable(int status) {
    return status == 408 || status == 429 || (status >= 500 && status <= 599);
}

// Parse a Retry-After header value. Only the delay-seconds form is accepted:
// the HTTP-date form is legal but the worker never sends it, and a date
// parser is a lot of surface for a case that does not occur.
inline std::optional<uint64_t> parse_retry_after(const std::string& value) {
    size_t b = value.find_first_not_of(" \t");
    if (b == std::string::npos) return std::nullopt;
    size_t e = value.find_last_not_of(" \t");
    const std::string v = value.substr(b, e - b + 1);
    if (v.empty()) return std::nullopt;

    uint64_t out = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return std::nullopt;
        if (out > (UINT64_MAX - static_cast<uint64_t>(c - '0')) / 10) {
            return std::nullopt; // absurd value; treat as unparseable
        }
        out = out * 10 + static_cast<uint64_t>(c - '0');
    }
    return out;
}

// Decide the next step for a status on a given 1-based attempt.
inline RetryDecision retry_decide(int                      status,
                                  uint32_t                 attempt,
                                  std::optional<uint64_t>  retry_after_secs) {
    if (attempt >= RETRY_MAX_ATTEMPTS || !status_retryable(status)) {
        return RetryDecision{false, 0};
    }
    uint64_t raw = backoff_ms(attempt);
    if (status == 429 && retry_after_secs.has_value()) {
        const uint64_t secs = *retry_after_secs;
        // Multiply carefully: a huge Retry-After must clamp, not wrap.
        raw = secs > RETRY_MAX_SLEEP_MS / 1000 ? RETRY_MAX_SLEEP_MS : secs * 1000;
    }
    return RetryDecision{true, raw > RETRY_MAX_SLEEP_MS ? RETRY_MAX_SLEEP_MS : raw};
}

} // namespace keylight
