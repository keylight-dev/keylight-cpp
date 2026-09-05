// tests/test_retry.cpp — retry policy, ported from keylight-rust
// keylight/src/http/retry.rs. Pure logic: the policy is tested without
// sleeping or networking.

#include "doctest.h"
#include "keylight/retry.hpp"

using namespace keylight;

TEST_CASE("retry: backoff is exponential and capped") {
    CHECK(backoff_ms(1) == 500);
    CHECK(backoff_ms(2) == 1000);
    CHECK(backoff_ms(3) == 2000);
    CHECK(backoff_ms(10) == 4000);   // capped, never unbounded
}

TEST_CASE("retry: only transient statuses are retryable") {
    CHECK(status_retryable(408) == true);
    CHECK(status_retryable(429) == true);
    CHECK(status_retryable(500) == true);
    CHECK(status_retryable(503) == true);
    CHECK(status_retryable(599) == true);

    // A 4xx that is not 408/429 is the server's final answer. Retrying an
    // "Activation limit reached" just burns the user's time.
    CHECK(status_retryable(400) == false);
    CHECK(status_retryable(401) == false);
    CHECK(status_retryable(404) == false);
    CHECK(status_retryable(422) == false);
    CHECK(status_retryable(200) == false);
}

TEST_CASE("retry: stops at the attempt ceiling") {
    auto d = retry_decide(500, RETRY_MAX_ATTEMPTS, std::nullopt);
    CHECK(d.retry == false);
}

TEST_CASE("retry: a 429 honors Retry-After over the backoff curve") {
    auto d = retry_decide(429, 1, std::optional<uint64_t>(30));
    REQUIRE(d.retry == true);
    CHECK(d.delay_ms == 30000);
}

TEST_CASE("retry: a 429 without Retry-After falls back to the backoff curve") {
    auto d = retry_decide(429, 1, std::nullopt);
    REQUIRE(d.retry == true);
    CHECK(d.delay_ms == 500);
}

TEST_CASE("retry: a hostile Retry-After is clamped, not obeyed") {
    // A server (or a proxy) asking us to sleep for a year must not park the
    // caller's thread for a year.
    auto d = retry_decide(429, 1, std::optional<uint64_t>(60ULL * 60 * 24 * 365));
    REQUIRE(d.retry == true);
    CHECK(d.delay_ms == 3600000);
}

TEST_CASE("retry: parse_retry_after accepts delay-seconds and rejects dates") {
    CHECK(parse_retry_after("30").value() == 30);
    CHECK(parse_retry_after("0").value() == 0);
    CHECK(parse_retry_after("  30  ").value() == 30);

    // The HTTP-date form is legal but the worker never sends it; parsing
    // dates would be a lot of surface for a case that does not occur.
    CHECK(parse_retry_after("Wed, 21 Oct 2015 07:28:00 GMT").has_value() == false);
    CHECK(parse_retry_after("").has_value() == false);
    CHECK(parse_retry_after("-5").has_value() == false);
    CHECK(parse_retry_after("30s").has_value() == false);
}
