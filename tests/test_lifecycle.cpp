// tests/test_lifecycle.cpp — ported verbatim from keylight-rust
// keylight/src/state.rs::lifecycle_event. The transition table is cross-SDK;
// changing an arm here silently diverges C++ from Rust and Swift.

#include "doctest.h"
#include "keylight/lifecycle.hpp"

using namespace keylight;

TEST_CASE("lifecycle: a later expiry on a live license is a renewal") {
    auto e = lifecycle_event(State::Licensed, State::Licensed, true);
    REQUIRE(e.has_value());
    CHECK(*e == LifecycleEvent::Renewed);
}

TEST_CASE("lifecycle: an unchanged expiry is not an event") {
    CHECK(lifecycle_event(State::Licensed, State::Licensed, false).has_value() == false);
}

TEST_CASE("lifecycle: leaving Licensed for Expired or Limited is a cancellation") {
    auto a = lifecycle_event(State::Licensed, State::Expired, false);
    REQUIRE(a.has_value());
    CHECK(*a == LifecycleEvent::Cancelled);

    auto b = lifecycle_event(State::Licensed, State::Limited, false);
    REQUIRE(b.has_value());
    CHECK(*b == LifecycleEvent::Cancelled);
}

TEST_CASE("lifecycle: arriving at Licensed from anywhere is a restore") {
    for (State prev : {State::Expired, State::Limited, State::Invalid}) {
        auto e = lifecycle_event(prev, State::Licensed, false);
        REQUIRE(e.has_value());
        CHECK(*e == LifecycleEvent::Restored);
    }
}

TEST_CASE("lifecycle: reaching Expired from a non-Expired state is an expiry") {
    auto e = lifecycle_event(State::Trial, State::Expired, false);
    REQUIRE(e.has_value());
    CHECK(*e == LifecycleEvent::Expired);

    // Already expired: no repeat event on every poll.
    CHECK(lifecycle_event(State::Expired, State::Expired, false).has_value() == false);
}

TEST_CASE("lifecycle: an unremarkable transition emits nothing") {
    CHECK(lifecycle_event(State::Invalid, State::Trial, false).has_value() == false);
    CHECK(lifecycle_event(State::Trial, State::FreeTier, false).has_value() == false);
}

TEST_CASE("lifecycle: Cancelled outranks Expired when leaving Licensed") {
    // Licensed -> Expired matches two arms. Order matters: a customer whose
    // subscription was cancelled should be told it was cancelled, not that
    // something merely expired, and Rust resolves it the same way.
    auto e = lifecycle_event(State::Licensed, State::Expired, false);
    REQUIRE(e.has_value());
    CHECK(*e == LifecycleEvent::Cancelled);
}

TEST_CASE("lifecycle: a renewal is not reported when leaving Licensed") {
    // expiry_moved_later is only meaningful while still Licensed. A lease that
    // degrades must not read as a renewal just because the new expiry is later.
    auto e = lifecycle_event(State::Licensed, State::Limited, true);
    REQUIRE(e.has_value());
    CHECK(*e == LifecycleEvent::Cancelled);
}
