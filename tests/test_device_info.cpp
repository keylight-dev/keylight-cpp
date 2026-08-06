// tests/test_device_info.cpp — TDD for the coarse device-telemetry buckets.
//
// The bucket strings are a CROSS-SDK CONTRACT shared by the Worker and the
// Swift / Rust / JS / C# / C++ SDKs. A boundary that disagrees with the other
// SDKs splits one machine population across two buckets.
//
//   cpu_cores : "1-2" | "3-4" | "5-8" | "9-16" | "17+"     (both ends inclusive)
//   memory    : "<4GB" | "4-8GB" | "8-16GB" | "16-32GB"
//               | "32-64GB" | "64GB+"    (lower inclusive, upper EXCLUSIVE)
//
// Raw core counts / byte counts must NEVER reach the wire — only the bucket.

#include "doctest.h"
#include "keylight/device_info.hpp"

#include <cstdint>
#include <string>

using namespace keylight;

static constexpr uint64_t GiB = 1024ull * 1024ull * 1024ull;

// ---------------------------------------------------------------------------
// cpu_cores_bucket — pure, boundary-exact
// ---------------------------------------------------------------------------
TEST_CASE("device_info: cpu_cores_bucket covers every boundary") {
    CHECK(std::string(detail::cpu_cores_bucket(1))  == "1-2");
    CHECK(std::string(detail::cpu_cores_bucket(2))  == "1-2");
    CHECK(std::string(detail::cpu_cores_bucket(3))  == "3-4");
    CHECK(std::string(detail::cpu_cores_bucket(4))  == "3-4");   // 4 -> "3-4"
    CHECK(std::string(detail::cpu_cores_bucket(5))  == "5-8");   // 5 -> "5-8"
    CHECK(std::string(detail::cpu_cores_bucket(8))  == "5-8");
    CHECK(std::string(detail::cpu_cores_bucket(9))  == "9-16");
    CHECK(std::string(detail::cpu_cores_bucket(16)) == "9-16");
    CHECK(std::string(detail::cpu_cores_bucket(17)) == "17+");
}

TEST_CASE("device_info: cpu_cores_bucket returns empty for unknown (0)") {
    // hardware_concurrency() returns 0 when the count is indeterminable —
    // the field must then be omitted from the payload, never guessed.
    CHECK(std::string(detail::cpu_cores_bucket(0)).empty());
}

TEST_CASE("device_info: cpu_cores_bucket saturates far above the top bucket") {
    CHECK(std::string(detail::cpu_cores_bucket(64))   == "17+");
    CHECK(std::string(detail::cpu_cores_bucket(1024)) == "17+");
}

// ---------------------------------------------------------------------------
// memory_bucket — pure, boundary-exact, upper bound EXCLUSIVE
// ---------------------------------------------------------------------------
TEST_CASE("device_info: memory_bucket covers every boundary") {
    CHECK(std::string(detail::memory_bucket(static_cast<uint64_t>(3.9 * GiB)))  == "<4GB");
    CHECK(std::string(detail::memory_bucket(4 * GiB))                           == "4-8GB");
    CHECK(std::string(detail::memory_bucket(static_cast<uint64_t>(7.9 * GiB)))  == "4-8GB");
    CHECK(std::string(detail::memory_bucket(8 * GiB))                           == "8-16GB");
    CHECK(std::string(detail::memory_bucket(16 * GiB))                          == "16-32GB");
    CHECK(std::string(detail::memory_bucket(32 * GiB))                          == "32-64GB");
    CHECK(std::string(detail::memory_bucket(64 * GiB))                          == "64GB+");
    CHECK(std::string(detail::memory_bucket(128 * GiB))                         == "64GB+");
}

TEST_CASE("device_info: memory_bucket uses GiB (1024^3), not GB (1000^3)") {
    // A machine the OS reports as 8'000'000'000 bytes is 7.45 GiB -> "4-8GB".
    // Using 1000^3 would wrongly put it in "8-16GB" and split the population.
    CHECK(std::string(detail::memory_bucket(8000000000ull)) == "4-8GB");
    CHECK(std::string(detail::memory_bucket(4000000000ull)) == "<4GB");
}

TEST_CASE("device_info: memory_bucket returns empty for unknown (0)") {
    CHECK(std::string(detail::memory_bucket(0)).empty());
}

TEST_CASE("device_info: memory_bucket does not pre-round near a boundary") {
    // One byte under 8 GiB must still be "4-8GB"; exactly 8 GiB flips.
    CHECK(std::string(detail::memory_bucket(8 * GiB - 1)) == "4-8GB");
    CHECK(std::string(detail::memory_bucket(8 * GiB))     == "8-16GB");
}

// ---------------------------------------------------------------------------
// Detection on the host — must be one of the contract strings, or empty.
// ---------------------------------------------------------------------------
TEST_CASE("device_info: host detection yields a contract-legal bucket") {
    const std::string cores = detail::cpu_cores_bucket(detail::detect_cpu_cores());
    CHECK((cores.empty() || cores == "1-2" || cores == "3-4" || cores == "5-8"
           || cores == "9-16" || cores == "17+"));

    const std::string mem = detail::memory_bucket(detail::detect_physical_memory_bytes());
    CHECK((mem.empty() || mem == "<4GB" || mem == "4-8GB" || mem == "8-16GB"
           || mem == "16-32GB" || mem == "32-64GB" || mem == "64GB+"));
}
