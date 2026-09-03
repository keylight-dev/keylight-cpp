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
#  include <unistd.h>
#elif defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/utsname.h>
#endif

#include <cctype>
#include <string>

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

// Server cap for os_version (zod `z.string().max(32)`).
inline constexpr size_t OS_VERSION_MAX = 32;

// Extract the first dotted-numeric run (\d+(\.\d+)*) from a raw OS string.
//
// Handles every shape the per-OS reads produce: the macOS sysctl is already
// clean ("15.5"), a Linux kernel release carries a suffix ("6.8.0-45-generic"),
// and the Windows version is assembled from three numbers. A trailing dot is
// dropped; a run over the server's cap is REJECTED rather than truncated —
// truncation would mint a fake version bucket out of a client bug.
//
// Pure. Mirrors keylight-rust telemetry::dotted_numeric.
inline std::string dotted_numeric(const std::string& raw) {
    size_t start = raw.find_first_of("0123456789");
    if (start == std::string::npos) return {};

    size_t end = start;
    while (end < raw.size()) {
        unsigned char c = static_cast<unsigned char>(raw[end]);
        if (!std::isdigit(c) && raw[end] != '.') break;
        ++end;
    }

    std::string v = raw.substr(start, end - start);
    while (!v.empty() && v.back() == '.') v.pop_back();

    // Drop a trailing ".0" for parity with the Swift SDK, which does this
    // before sending. The server does NOT normalize it away, so without this
    // the same Mac reports "15.5" from Swift and "15.5.0" from C++ and the
    // dashboard's OS breakdown splits one population across two buckets.
    if (v.size() >= 2 && v.compare(v.size() - 2, 2, ".0") == 0) {
        v.erase(v.size() - 2);
    }

    if (v.empty() || v.size() > OS_VERSION_MAX) return {};

    // Every dot-separated component must be non-empty. The scan above already
    // guarantees each character is a digit or a dot, so this is the only
    // remaining way to be malformed ("1..2").
    size_t i = 0;
    while (i <= v.size()) {
        size_t dot  = v.find('.', i);
        size_t stop = (dot == std::string::npos) ? v.size() : dot;
        if (stop == i) return {};
        if (dot == std::string::npos) break;
        i = dot + 1;
    }
    return v;
}

// Raw per-platform OS version read. NEVER spawns a process: this SDK ships
// inside JUCE plugins and Unreal, and a sandboxed AU/VST3 host may block
// process spawn. keylight-rust shells out to `sw_vers` / `cmd /c ver`; this is
// the same output by a different mechanism. Returns "" on any failure.
inline std::string read_os_version_raw() {
#if defined(__APPLE__)
    char   buf[64];
    size_t len = sizeof(buf);
    if (::sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) != 0) {
        return {};
    }
    // sysctlbyname reports the length INCLUDING the NUL terminator.
    return std::string(buf, len > 0 ? len - 1 : 0);
#elif defined(_WIN32) || defined(_WIN64)
    // GetVersionEx lies for unmanifested apps; RtlGetVersion does not. The
    // struct is declared locally so this header needs no <winternl.h> — the
    // layout is RTL_OSVERSIONINFOW's, which is ABI-stable.
    struct KlOsVersionInfoW {
        ULONG dwOSVersionInfoSize;
        ULONG dwMajorVersion;
        ULONG dwMinorVersion;
        ULONG dwBuildNumber;
        ULONG dwPlatformId;
        WCHAR szCSDVersion[128];
    };
    using RtlGetVersionFn = LONG(WINAPI*)(KlOsVersionInfoW*);

    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return {};
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
    if (!fn) return {};

    KlOsVersionInfoW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return {};
    return std::to_string(vi.dwMajorVersion) + "." +
           std::to_string(vi.dwMinorVersion) + "." +
           std::to_string(vi.dwBuildNumber);
#else
    // Kernel release ("6.8.0-45-generic") — the one version every Linux has.
    // Distro versions live in /etc/os-release but are not comparable across
    // distros, and the kernel is what OS-level behavior actually tracks.
    struct utsname u{};
    if (::uname(&u) != 0) return {};
    return std::string(u.release);
#endif
}

// Normalized OS version, or "" when the platform read fails or yields nothing
// dotted-numeric. Read once per process — the value cannot change while the
// app runs, and on Windows this costs a GetProcAddress.
inline std::string detect_os_version() {
    static const std::string cached = dotted_numeric(read_os_version_raw());
    return cached;
}

// Longest instance_name this SDK will send. The server schema is
// `z.string().min(1)` with no maximum, so this cap is ours: a hostname is
// user-controlled input and an unbounded one has no business on the wire.
inline constexpr size_t INSTANCE_NAME_MAX = 64;

// Strip control characters, trim trailing spaces, cap the length. Pure.
inline std::string sanitize_instance_name(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u != 0x7f) out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > INSTANCE_NAME_MAX) out.resize(INSTANCE_NAME_MAX);
    return out;
}

// Best-effort human-readable machine name for the activation's instance_name.
// This is what a customer sees in their device list, so a real hostname beats
// a constant. Returns "" on failure; the caller supplies the fallback.
inline std::string detect_machine_name() {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD   len = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    if (!::GetComputerNameW(buf, &len)) return {};
    // Hostnames are ASCII in practice; anything outside it is dropped rather
    // than mangled into a lossy multi-byte guess.
    std::string out;
    for (DWORD i = 0; i < len; ++i) {
        wchar_t c = buf[i];
        if (c > 0 && c < 128) out += static_cast<char>(c);
    }
    return sanitize_instance_name(out);
#else
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) != 0) return {};
    buf[sizeof(buf) - 1] = '\0';
    return sanitize_instance_name(std::string(buf));
#endif
}

} // namespace detail
} // namespace keylight
