// tests/test_clock.cpp — heuristic detection of a backward system-clock jump.
// Ported from keylight-rust keylight/src/clock.rs.

#include "doctest.h"
#include "keylight/clock.hpp"

using keylight::clock_rolled_back;

TEST_CASE("clock: normal forward progress is not a rollback") {
    CHECK(clock_rolled_back(1000, 1100) == false);
}

TEST_CASE("clock: a long offline stretch is not a rollback") {
    // Sixty days forward is governed by maxOfflineDays, not by this guard.
    // Conflating the two would fail-close on every returning laptop user.
    CHECK(clock_rolled_back(0, 60 * 24 * 60 * 60) == false);
}

TEST_CASE("clock: a backward jump beyond tolerance is a rollback") {
    CHECK(clock_rolled_back(10000, 10000 - 4000) == true);
}

TEST_CASE("clock: small backward drift is tolerated") {
    // NTP corrections and suspend/resume routinely move the clock a little.
    CHECK(clock_rolled_back(10000, 10000 - 100) == false);
}

TEST_CASE("clock: the tolerance boundary is exclusive") {
    CHECK(clock_rolled_back(10000, 10000 - 3600) == false);
    CHECK(clock_rolled_back(10000, 10000 - 3601) == true);
}
