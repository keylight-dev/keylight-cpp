#pragma once
// keylight/device_info.hpp — coarse device telemetry buckets (cpu_cores, memory).
//
// A licensing SDK must never report a machine's exact core count or exact RAM
// size: to a developer proxying their own app that reads as fingerprinting.
// Only the coarse bucket crosses the wire; the precise value stays local.
//
// BUCKET CONTRACT — identical in the Worker and in the Swift, Rust, JS, C# and
// C++ SDKs. These exact strings, nothing else:
//
//   cpu_cores : "1-2" | "3-4" | "5-8" | "9-16" | "17+"
//   memory    : "<4GB" | "4-8GB" | "8-16GB" | "16-32GB" | "32-64GB" | "64GB+"
//
// BOUNDARY CONVENTION (cross-SDK — do not change on one side):
//   * Core ranges are INCLUSIVE of both endpoints: 4 -> "3-4", 5 -> "5-8".
//   * Memory buckets are lower-inclusive / upper-EXCLUSIVE: "4-8GB" means
//     4 GiB <= x < 8 GiB, so exactly 8 GiB lands in "8-16GB".
//   * Sizes are computed in GiB (1024^3) against the raw byte count reported
//     by the OS. Physical RAM rarely lands exactly on a power of two, so the
//     byte count is never pre-rounded.
//
// Both bucketing functions are PURE. When the value is unknown they return an
// empty string and the caller omits the field entirely (never guesses).

#include <cstdint>
#include <thread>

#if defined(__APPLE__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#elif defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace keylight {
namespace detail {

// One GiB in bytes — the unit the memory buckets are defined in.
static constexpr uint64_t KL_GIB = 1024ull * 1024ull * 1024ull;

/// Pure: map a CPU core count to its contract bucket.
/// Returns "" when the count is unknown (0), so the field is omitted.
inline const char* cpu_cores_bucket(unsigned cores) {
    if (cores == 0)  return "";
    if (cores <= 2)  return "1-2";
    if (cores <= 4)  return "3-4";
    if (cores <= 8)  return "5-8";
    if (cores <= 16) return "9-16";
    return "17+";
}

/// Pure: map physical RAM in BYTES to its contract bucket.
/// Upper bound of each range is exclusive; returns "" when unknown (0).
inline const char* memory_bucket(uint64_t bytes) {
    if (bytes == 0)             return "";
    if (bytes <  4  * KL_GIB)   return "<4GB";
    if (bytes <  8  * KL_GIB)   return "4-8GB";
    if (bytes <  16 * KL_GIB)   return "8-16GB";
    if (bytes <  32 * KL_GIB)   return "16-32GB";
    if (bytes <  64 * KL_GIB)   return "32-64GB";
    return "64GB+";
}

/// Logical CPU count as reported by the runtime; 0 when indeterminable.
inline unsigned detect_cpu_cores() {
    return std::thread::hardware_concurrency();
}

/// Physical RAM in bytes; 0 when it cannot be determined.
inline uint64_t detect_physical_memory_bytes() {
#if defined(__APPLE__)
    uint64_t mem = 0;
    size_t   len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) != 0) return 0;
    return mem;
#elif defined(_WIN32) || defined(_WIN64)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (!GlobalMemoryStatusEx(&st)) return 0;
    return static_cast<uint64_t>(st.ullTotalPhys);
#else
    const long pages     = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || page_size <= 0) return 0;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
#endif
}

// Canonical CPU-architecture token for this build target.
//
// The server allow-lists exactly two spellings — "arm64" and "x86_64" — and
// canonicalizes aliases (aarch64 -> arm64) server-side, but we send the
// canonical spelling ourselves. Targets outside the vocabulary (32-bit,
// exotic ISAs) return "" and the caller omits the field: absent reads better
// server-side than a long tail of one-off buckets it would drop anyway.
inline const char* current_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "";
#endif
}

} // namespace detail
} // namespace keylight
