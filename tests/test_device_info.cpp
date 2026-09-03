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

TEST_CASE("device_info: current_arch is a server-allowed token or empty") {
    std::string a = keylight::detail::current_arch();
    // The server allow-lists exactly two spellings and canonicalizes aliases.
    // A target outside that vocabulary omits the field rather than inventing
    // a one-off bucket the server would drop anyway.
    CHECK((a == "arm64" || a == "x86_64" || a.empty()));
}

TEST_CASE("device_info: current_arch is non-empty on the CI architectures") {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__x86_64__) || defined(_M_X64)
    CHECK(std::string(keylight::detail::current_arch()).empty() == false);
#endif
}

TEST_CASE("device_info: dotted_numeric extracts the first well-formed run") {
    using keylight::detail::dotted_numeric;

    // macOS sysctl is already clean.
    CHECK(dotted_numeric("15.5") == "15.5");
    // A Linux kernel release carries a suffix. The trailing ".0" is then
    // dropped by the patch-zero rule below, so this lands on "6.8".
    CHECK(dotted_numeric("6.8.0-45-generic") == "6.8");
    // Windows wraps the number in prose that may be localized.
    CHECK(dotted_numeric("Microsoft Windows [Version 10.0.22631.3737]")
          == "10.0.22631.3737");
    // A trailing dot is dropped rather than sent.
    CHECK(dotted_numeric("15.") == "15");
}

TEST_CASE("device_info: a trailing patch zero is dropped to match the Swift SDK") {
    using keylight::detail::dotted_numeric;

    // The SERVER does not strip a patch zero — "15.5.0" stays "15.5.0". The
    // Swift SDK strips it before sending. If C++ did not, the same Mac would
    // land in two different os_version buckets depending on which SDK the app
    // was built with, which makes the dashboard's OS breakdown meaningless.
    CHECK(dotted_numeric("15.5.0")  == "15.5");
    CHECK(dotted_numeric("14.0")    == "14");
    // Only a trailing zero, and only one: 10.0.19045 is a real Windows build.
    CHECK(dotted_numeric("10.0.19045") == "10.0.19045");
    CHECK(dotted_numeric("6.8.0-45-generic") == "6.8");
}

TEST_CASE("device_info: dotted_numeric rejects rather than repairs") {
    using keylight::detail::dotted_numeric;

    CHECK(dotted_numeric("").empty());
    CHECK(dotted_numeric("no digits here").empty());
    // An empty component is malformed, not something to patch up.
    CHECK(dotted_numeric("1..2").empty());
    // Over the server's 32-char cap: rejected, never truncated. Truncating
    // would mint a fake version bucket out of a client bug.
    CHECK(dotted_numeric("1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16").empty());
}

TEST_CASE("device_info: detect_os_version is dotted-numeric or empty") {
    std::string v = keylight::detail::detect_os_version();
    CHECK(v == keylight::detail::dotted_numeric(v));
    CHECK(v.size() <= 32);
}

TEST_CASE("device_info: sanitize_instance_name strips control chars and caps length") {
    using keylight::detail::sanitize_instance_name;

    CHECK(sanitize_instance_name("studio-imac") == "studio-imac");
    CHECK(sanitize_instance_name("name\twith\nctl") == "namewithctl");
    CHECK(sanitize_instance_name("trailing   ") == "trailing");
    CHECK(sanitize_instance_name(std::string(200, 'x')).size() == 64);
    CHECK(sanitize_instance_name("").empty());
}
