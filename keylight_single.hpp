// =============================================================================
// keylight_single.hpp — Keylight C++ SDK  (single-header amalgamation)
//
// AUTO-GENERATED — DO NOT EDIT
// Regenerate with:  python3 tools/amalgamate.py
//
// Include this single file in your project instead of the split headers.
// To use the cpp-httplib transport, define KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
// and add third_party/ to your include path before including this file.
//
// SPDX-License-Identifier: MIT
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>


// ──────────────────────────────────────────────────────────────────────────
// include/keylight/version.hpp
// ──────────────────────────────────────────────────────────────────────────

#define KEYLIGHT_SDK_VERSION "0.1.6"

// Identifies this SDK to the backend, sent as `sdk` alongside `platform`.
// Server cap is 16 characters (zod `z.string().max(16)`).
#define KEYLIGHT_SDK_ID "cpp"

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/result.hpp
// ──────────────────────────────────────────────────────────────────────────


namespace keylight {

// ---------------------------------------------------------------------------
// ErrorCode
// ---------------------------------------------------------------------------
enum class ErrorCode {
    None,
    Network,
    Http,
    InvalidKey,
    SeatLimit,
    NotActivated,
    BadResponse,
    Crypto,
    Config,
    Io,
};

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------
struct Error {
    ErrorCode   code    = ErrorCode::None;
    std::string message;
};

// ---------------------------------------------------------------------------
// Result<T>  — primary template
// ---------------------------------------------------------------------------
template <class T>
class Result {
public:
    static Result ok(T value) {
        Result r;
        r.ok_      = true;
        r.value_   = std::move(value);
        return r;
    }

    static Result err(Error e) {
        Result r;
        r.ok_    = false;
        r.error_ = std::move(e);
        return r;
    }

    bool is_ok() const { return ok_; }

    const T& value() const {
        assert(ok_ && "Result::value() called on an error result");
        return value_;
    }

    const Error& error() const {
        assert(!ok_ && "Result::error() called on an ok result");
        return error_;
    }

    const std::string& error_message() const {
        assert(!ok_ && "Result::error_message() called on an ok result");
        return error_.message;
    }

private:
    bool   ok_     = false;
    T      value_  = {};
    Error  error_  = {};
};

// ---------------------------------------------------------------------------
// Result<void>  — specialization
// ---------------------------------------------------------------------------
template <>
class Result<void> {
public:
    static Result ok() {
        Result r;
        r.ok_ = true;
        return r;
    }

    static Result err(Error e) {
        Result r;
        r.ok_    = false;
        r.error_ = std::move(e);
        return r;
    }

    bool is_ok() const { return ok_; }

    const Error& error() const {
        assert(!ok_ && "Result<void>::error() called on an ok result");
        return error_;
    }

    const std::string& error_message() const {
        assert(!ok_ && "Result<void>::error_message() called on an ok result");
        return error_.message;
    }

private:
    bool  ok_    = false;
    Error error_ = {};
};

// ---------------------------------------------------------------------------
// base64 — standard alphabet (RFC 4648), no line wrapping
// ---------------------------------------------------------------------------
namespace detail {

inline const char* b64_chars() {
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

// Returns 0-63 for valid base64 chars, -1 for padding '=', -2 for ignore/invalid
inline int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -1;  // padding
    return -2;                // ignore (whitespace etc.)
}

} // namespace detail

inline std::string base64_encode(const std::string& input) {
    const char* chars = detail::b64_chars();
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    const auto* data = reinterpret_cast<const uint8_t*>(input.data());
    std::size_t len  = input.size();

    for (std::size_t i = 0; i < len; i += 3) {
        uint32_t b  = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);

        out.push_back(chars[(b >> 18) & 0x3F]);
        out.push_back(chars[(b >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? chars[(b >>  6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? chars[(b      ) & 0x3F] : '=');
    }
    return out;
}

inline std::string base64_decode(const std::string& input) {
    std::string out;
    out.reserve((input.size() / 4) * 3);

    uint32_t    buf    = 0;
    int         bits   = 0;

    for (char c : input) {
        int v = detail::b64_val(c);
        if (v == -2) continue;   // skip whitespace / unknown
        if (v == -1) break;      // padding '=' — stop
        buf  = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/clock.hpp
// ──────────────────────────────────────────────────────────────────────────

// keylight/clock.hpp — heuristic detection of system-clock manipulation.
// Ported from keylight-rust keylight/src/clock.rs.


namespace keylight {

// How far the clock may move backward before we call it manipulation rather
// than drift. NTP corrections and suspend/resume routinely move it a little.
inline constexpr int64_t CLOCK_BACKWARD_TOLERANCE = 3600; // 1h

// True when `now` is more than the tolerance behind `last_seen` — the clock
// was rolled back since the last recorded contact.
//
// This deliberately OMITS the forward-jump component of Rust's
// clock_manipulated(), so it can gate the read-only state() resolver without
// governing offline duration — that stays with maxOfflineDays. Conflating the
// two would fail-close on every user who simply went offline for a while.
//
// Operates on UTC epoch seconds, so a timezone change never trips it.
inline bool clock_rolled_back(int64_t last_seen, int64_t now) {
    return last_seen - now > CLOCK_BACKWARD_TOLERANCE;
}

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/config.hpp
// ──────────────────────────────────────────────────────────────────────────


namespace keylight {

struct Config {
    std::string tenantId;
    std::string productId;
    std::string sdkKey;

    // Map of keyId → base64-encoded Ed25519 public key (32 bytes)
    std::map<std::string, std::string> trustedKeys;

    int         maxOfflineDays     = 15;
    std::string keyPrefix;
    int         trialDurationDays  = 0;
    // Free tier: when true, a device with no license and no active trial
    // resolves State::FreeTier instead of Invalid/Expired.  Parity with
    // keylight-rust Config::free_tier_enabled.
    bool        freeTierEnabled    = false;
    std::string apiBaseUrl         = "https://api.keylight.dev";
    std::string appVersion;        // optional; sent as telemetry in activate/validate

    // Interval between background auto-validation ticks (milliseconds).
    // Default is 30 minutes (1 800 000 ms).  Set a smaller value in tests
    // as a deterministic seam — the background thread uses this as its
    // interruptible wait timeout.
    int autoValidationIntervalMs = 1'800'000; // 30 min
};

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/device_info.hpp
// ──────────────────────────────────────────────────────────────────────────

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
// A patch zero is NOT stripped: "15.0" is sent as "15.0". keylight-rust sends
// `sw_vers -productVersion` verbatim, and keylight-swift drops the patch
// component only when it is zero AND there are three components — so macOS
// 15.0 is "15.0" in all three SDKs. Stripping it here would be the one client
// reporting "15" and would split a single population across two dashboard
// buckets on every x.0 release.
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
    if (out.size() > INSTANCE_NAME_MAX) {
        // Cap by BYTES, but never mid-character. High bytes are kept above, so
        // a non-ASCII hostname over the cap would otherwise be cut through the
        // middle of a UTF-8 sequence. json_str does not escape high bytes, so
        // the malformed tail would reach the wire verbatim and the worker can
        // reject the whole activate request over a machine name.
        //
        // Continuation bytes are 10xxxxxx; walk back off them to the lead byte
        // and drop the partial character entirely.
        size_t cut = INSTANCE_NAME_MAX;
        while (cut > 0 &&
               (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        out.resize(cut);
    }
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

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/json.hpp
// ──────────────────────────────────────────────────────────────────────────

// keylight/json.hpp — minimal header-only recursive-descent JSON reader
// Namespace keylight; internals in keylight::json_detail (named namespace so
// that JValue and Parser have external linkage and are the SAME type in every
// translation unit — avoiding an ODR violation that arises when a public class
// with external linkage stores a shared_ptr to a TU-local anonymous-namespace
// type).
// No external dependencies. Exception-free: errors propagate via Result<Json>.



namespace keylight {

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
class Json;

// ---------------------------------------------------------------------------
// Internal implementation — named namespace (external linkage, ODR-safe)
// ---------------------------------------------------------------------------
namespace json_detail {

// ---- Value storage --------------------------------------------------------

enum class JType { Null, Bool, Int, Double, String, Array, Object };

struct JValue {
    JType type = JType::Null;

    bool        b   = false;
    int64_t     i   = 0;
    double      d   = 0.0;
    std::string s;

    // Array: ordered elements
    std::vector<std::shared_ptr<JValue>> arr;

    // Object: insertion-ordered keys + lookup map
    std::vector<std::string>                         obj_keys;
    std::map<std::string, std::shared_ptr<JValue>>   obj_map;
};

// ---- Parser ---------------------------------------------------------------

struct Parser {
    const char* p;
    const char* end;

    explicit Parser(const std::string& src)
        : p(src.data()), end(src.data() + src.size()) {}

    inline bool eof() const { return p >= end; }

    inline void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    inline bool peek(char c) const { return !eof() && *p == c; }

    inline bool consume(char c) {
        if (!eof() && *p == c) { ++p; return true; }
        return false;
    }

    // Decode one \uXXXX code unit to UTF-8
    inline bool hex4(uint32_t& out) {
        if (end - p < 4) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = p[k];
            uint32_t nib;
            if      (c >= '0' && c <= '9') nib = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') nib = (uint32_t)(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') nib = (uint32_t)(c - 'A') + 10;
            else return false;
            out = (out << 4) | nib;
        }
        p += 4;
        return true;
    }

    inline void encode_utf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // Parse a JSON string (cursor is past the opening '"')
    inline bool parse_string(std::string& out) {
        out.clear();
        while (!eof()) {
            char c = *p++;
            if (c == '"') return true;   // closing quote
            if (c == '\\') {
                if (eof()) return false;
                char esc = *p++;
                switch (esc) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp;
                        if (!hex4(cp)) return false;
                        // Handle surrogate pairs
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // high surrogate — expect \uXXXX low surrogate
                            if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                                return false;
                            p += 2;
                            uint32_t low;
                            if (!hex4(low)) return false;
                            if (low < 0xDC00 || low > 0xDFFF) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        encode_utf8(cp, out);
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false; // unterminated string
    }

    // Parse a number; cursor is on the first digit or '-'
    inline bool parse_number(std::shared_ptr<JValue>& out) {
        const char* start = p;
        bool neg = false;
        if (peek('-')) { neg = true; ++p; }

        if (eof() || *p < '0' || *p > '9') return false;

        int64_t ival = 0;
        while (!eof() && *p >= '0' && *p <= '9') {
            ival = ival * 10 + (*p - '0');
            ++p;
        }

        bool is_float = false;
        double dval = static_cast<double>(ival);

        // Fractional part
        if (!eof() && *p == '.') {
            is_float = true;
            ++p;
            double frac = 0.1;
            while (!eof() && *p >= '0' && *p <= '9') {
                dval += (*p - '0') * frac;
                frac *= 0.1;
                ++p;
            }
        }

        // Exponent part
        if (!eof() && (*p == 'e' || *p == 'E')) {
            is_float = true;
            ++p;
            bool eneg = false;
            if (!eof() && (*p == '+' || *p == '-')) {
                eneg = (*p == '-');
                ++p;
            }
            int64_t exp = 0;
            while (!eof() && *p >= '0' && *p <= '9') {
                exp = exp * 10 + (*p - '0');
                ++p;
            }
            if (eneg) for (int64_t k = 0; k < exp; ++k) dval /= 10.0;
            else      for (int64_t k = 0; k < exp; ++k) dval *= 10.0;
        }

        (void)start;

        out = std::make_shared<JValue>();
        if (is_float) {
            out->type = JType::Double;
            out->d = neg ? -dval : dval;
        } else {
            out->type = JType::Int;
            out->i = neg ? -ival : ival;
        }
        return true;
    }

    inline bool parse_value(std::shared_ptr<JValue>& out) {
        skip_ws();
        if (eof()) return false;

        char c = *p;

        if (c == '"') {
            ++p;
            out = std::make_shared<JValue>();
            out->type = JType::String;
            return parse_string(out->s);
        }

        if (c == '{') {
            ++p;
            // parse object inline
            out = std::make_shared<JValue>();
            out->type = JType::Object;
            skip_ws();
            if (consume('}')) return true;
            while (true) {
                skip_ws();
                if (!consume('"')) return false;
                std::string key;
                if (!parse_string(key)) return false;
                skip_ws();
                if (!consume(':')) return false;
                skip_ws();
                std::shared_ptr<JValue> val;
                if (!parse_value(val)) return false;
                if (out->obj_map.find(key) == out->obj_map.end()) {
                    out->obj_keys.push_back(key);
                }
                out->obj_map[key] = std::move(val);
                skip_ws();
                if (consume('}')) return true;
                if (!consume(',')) return false;
            }
        }

        if (c == '[') {
            ++p;
            // parse array inline
            out = std::make_shared<JValue>();
            out->type = JType::Array;
            skip_ws();
            if (consume(']')) return true;
            while (true) {
                skip_ws();
                std::shared_ptr<JValue> elem;
                if (!parse_value(elem)) return false;
                out->arr.push_back(std::move(elem));
                skip_ws();
                if (consume(']')) return true;
                if (!consume(',')) return false;
            }
        }

        if (c == 't') {
            if (end - p >= 4 && p[1]=='r' && p[2]=='u' && p[3]=='e') {
                p += 4;
                out = std::make_shared<JValue>();
                out->type = JType::Bool;
                out->b = true;
                return true;
            }
            return false;
        }

        if (c == 'f') {
            if (end - p >= 5 && p[1]=='a' && p[2]=='l' && p[3]=='s' && p[4]=='e') {
                p += 5;
                out = std::make_shared<JValue>();
                out->type = JType::Bool;
                out->b = false;
                return true;
            }
            return false;
        }

        if (c == 'n') {
            if (end - p >= 4 && p[1]=='u' && p[2]=='l' && p[3]=='l') {
                p += 4;
                out = std::make_shared<JValue>();
                out->type = JType::Null;
                return true;
            }
            return false;
        }

        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(out);
        }

        return false; // unknown character
    }
};

} // namespace json_detail

// ---------------------------------------------------------------------------
// keylight::Json — public API
// ---------------------------------------------------------------------------

class Json {
public:
    // Default-construct: null Json (used for missing keys)
    Json() : val_(std::make_shared<json_detail::JValue>()) {}

    // Parse JSON text; returns Result<Json> (never throws)
    static Result<Json> parse(const std::string& src) {
        if (src.empty()) {
            return Result<Json>::err({ErrorCode::BadResponse, "empty JSON input"});
        }
        json_detail::Parser parser(src);
        std::shared_ptr<json_detail::JValue> v;
        if (!parser.parse_value(v)) {
            return Result<Json>::err({ErrorCode::BadResponse, "malformed JSON"});
        }
        parser.skip_ws();
        if (!parser.eof()) {
            return Result<Json>::err({ErrorCode::BadResponse, "trailing garbage in JSON"});
        }
        Json j;
        j.val_ = std::move(v);
        return Result<Json>::ok(std::move(j));
    }

    // Object member access; missing key → null Json
    Json operator[](const std::string& key) const {
        if (val_->type == json_detail::JType::Object) {
            auto it = val_->obj_map.find(key);
            if (it != val_->obj_map.end()) {
                Json j;
                j.val_ = it->second;
                return j;
            }
        }
        return Json{}; // null
    }

    // Array element access; out-of-range → null Json
    Json at(size_t i) const {
        if (val_->type == json_detail::JType::Array && i < val_->arr.size()) {
            Json j;
            j.val_ = val_->arr[i];
            return j;
        }
        return Json{}; // null
    }

    bool is_array() const { return val_->type == json_detail::JType::Array; }

    // Array: element count; Object: member count; others: 0
    size_t size() const {
        if (val_->type == json_detail::JType::Array)  return val_->arr.size();
        if (val_->type == json_detail::JType::Object) return val_->obj_map.size();
        return 0;
    }

    std::string as_string() const {
        if (val_->type == json_detail::JType::String) return val_->s;
        return {};
    }

    int64_t as_int() const {
        if (val_->type == json_detail::JType::Int)    return val_->i;
        if (val_->type == json_detail::JType::Double) return static_cast<int64_t>(val_->d);
        return 0;
    }

    bool as_bool() const {
        if (val_->type == json_detail::JType::Bool) return val_->b;
        return false;
    }

    // Convenience: iterate an array of strings
    std::vector<std::string> as_string_array() const {
        std::vector<std::string> result;
        if (val_->type == json_detail::JType::Array) {
            for (const auto& elem : val_->arr) {
                if (elem->type == json_detail::JType::String) {
                    result.push_back(elem->s);
                } else {
                    result.push_back({});
                }
            }
        }
        return result;
    }

    // Return insertion-ordered member names of an object
    std::vector<std::string> keys() const {
        if (val_->type == json_detail::JType::Object) {
            return val_->obj_keys;
        }
        return {};
    }

private:
    std::shared_ptr<json_detail::JValue> val_;
};

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/lease.hpp
// ──────────────────────────────────────────────────────────────────────────

// keylight/lease.hpp — the signed v3 Lease and its canonical payload.
// Ported from keylight-rust keylight/src/lease.rs



namespace keylight {

struct Lease {
    std::string kid;
    std::string licenseKeyHash;
    std::string instanceId;
    int64_t     issuedAt  = 0;
    int64_t     expiresAt = 0;
    std::string status;
    std::string signature;
    std::vector<std::string> entitlements;

    /// Parse a Lease from a JSON object (as it appears in the vectors file).
    static Result<Lease> from_json(const Json& j) {
        Lease l;
        l.kid            = j["kid"].as_string();
        l.licenseKeyHash = j["licenseKeyHash"].as_string();
        l.instanceId     = j["instanceId"].as_string();
        l.issuedAt       = j["issuedAt"].as_int();
        l.expiresAt      = j["expiresAt"].as_int();
        l.status         = j["status"].as_string();
        l.signature      = j["signature"].as_string();
        // entitlements is an array of strings
        auto ents = j["entitlements"];
        size_t n = ents.size();
        for (size_t i = 0; i < n; ++i) {
            l.entitlements.push_back(ents.at(i).as_string());
        }
        if (l.kid.empty() || l.status.empty()) {
            return Result<Lease>::err({ErrorCode::BadResponse, "missing required lease fields"});
        }
        return Result<Lease>::ok(std::move(l));
    }
};

/// The exact UTF-8 payload that was signed.
/// Format: v3|kid|licenseKeyHash|instanceId|issuedAt|expiresAt|status|entitlements_csv
/// entitlements_csv = entitlements sorted ascending (lexicographic), comma-joined.
/// Empty entitlements → trailing empty string (e.g. "...active|").
inline std::string canonical_payload(const Lease& l) {
    // Sort a copy of entitlements ascending
    std::vector<std::string> ents = l.entitlements;
    std::sort(ents.begin(), ents.end());

    std::string csv;
    for (size_t i = 0; i < ents.size(); ++i) {
        if (i > 0) csv.push_back(',');
        csv += ents[i];
    }

    return "v3|" + l.kid + "|" + l.licenseKeyHash + "|" + l.instanceId
         + "|" + std::to_string(l.issuedAt) + "|" + std::to_string(l.expiresAt)
         + "|" + l.status + "|" + csv;
}

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/ed25519.hpp
// ──────────────────────────────────────────────────────────────────────────

// Keylight Ed25519 — verify-only, header-only, zero external dependencies.
//
// Core algorithm adapted from TweetNaCl (tweetnacl.c, public domain) by
// Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange,
// Peter Schwabe, Sjaak Smetsers. Source: https://tweetnacl.cr.yp.to/
//
// Only the verification path (crypto_sign_open equivalent) is retained.
// Signing and key-generation are NOT included.
//
// SHA-512 is vendored inline (from the same TweetNaCl source).
// No dependency on sha256.hpp or any system crypto library.
//
// Public API (namespace keylight):
//   bool keylight::ed25519_verify(
//       const uint8_t* msg, size_t msg_len,
//       const std::array<uint8_t,64>& sig,
//       const std::array<uint8_t,32>& pubkey);
//


namespace keylight {

// All internals are anonymous-namespace scoped (translation-unit private).
namespace {

using kl_u8  = uint8_t;
using kl_u64 = uint64_t;
using kl_i64 = int64_t;
// GF(2^255-19) element: 16 limbs of 16 bits each (radix 2^16 representation).
using kl_gf  = kl_i64[16];

// ── SHA-512 ───────────────────────────────────────────────────────────────────
// Adapted from TweetNaCl's crypto_hash.

static const kl_u64 kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static inline kl_u64 kl_rotr64(kl_u64 x, int n) {
    return (x >> n) | (x << (64 - n));
}

static void kl_sha512_block(kl_u64 h[8], const kl_u8 blk[128]) {
    kl_u64 w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = 0;
        for (int j = 0; j < 8; ++j) w[i] = (w[i] << 8) | blk[8*i + j];
    }
    for (int i = 16; i < 80; ++i) {
        kl_u64 s0 = kl_rotr64(w[i-15],1) ^ kl_rotr64(w[i-15],8) ^ (w[i-15]>>7);
        kl_u64 s1 = kl_rotr64(w[i-2],19) ^ kl_rotr64(w[i-2],61) ^ (w[i-2]>>6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    kl_u64 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 80; ++i) {
        kl_u64 S1  = kl_rotr64(e,14) ^ kl_rotr64(e,18) ^ kl_rotr64(e,41);
        kl_u64 ch  = (e & f) ^ (~e & g);
        kl_u64 t1  = hh + S1 + ch + kSha512K[i] + w[i];
        kl_u64 S0  = kl_rotr64(a,28) ^ kl_rotr64(a,34) ^ kl_rotr64(a,39);
        kl_u64 maj = (a & b) ^ (a & c) ^ (b & c);
        kl_u64 t2  = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
    h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

// Streaming SHA-512 context.
struct KlSha512Ctx {
    kl_u64 h[8];
    kl_u8  buf[128];
    kl_u64 total;   // bytes fed so far (not counting buf)
    int    used;
};

static void kl_sha512_init(KlSha512Ctx& ctx) {
    ctx.h[0] = 0x6a09e667f3bcc908ULL; ctx.h[1] = 0xbb67ae8584caa73bULL;
    ctx.h[2] = 0x3c6ef372fe94f82bULL; ctx.h[3] = 0xa54ff53a5f1d36f1ULL;
    ctx.h[4] = 0x510e527fade682d1ULL; ctx.h[5] = 0x9b05688c2b3e6c1fULL;
    ctx.h[6] = 0x1f83d9abfb41bd6bULL; ctx.h[7] = 0x5be0cd19137e2179ULL;
    ctx.total = 0; ctx.used = 0;
}

static void kl_sha512_update(KlSha512Ctx& ctx, const kl_u8* data, size_t len) {
    while (len > 0) {
        int space = 128 - ctx.used;
        int take  = (len < (size_t)space) ? (int)len : space;
        for (int i = 0; i < take; ++i) ctx.buf[ctx.used + i] = data[i];
        ctx.used += take; data += take; len -= take;
        if (ctx.used == 128) {
            kl_sha512_block(ctx.h, ctx.buf);
            ctx.total += 128; ctx.used = 0;
        }
    }
}

static void kl_sha512_final(KlSha512Ctx& ctx, kl_u8 out[64]) {
    kl_u64 bits = (ctx.total + (kl_u64)ctx.used) * 8;
    ctx.buf[ctx.used++] = 0x80;
    if (ctx.used > 112) {
        while (ctx.used < 128) ctx.buf[ctx.used++] = 0;
        kl_sha512_block(ctx.h, ctx.buf); ctx.used = 0;
    }
    while (ctx.used < 112) ctx.buf[ctx.used++] = 0;
    for (int i = 0; i < 8; ++i) ctx.buf[112 + i] = 0;
    for (int i = 0; i < 8; ++i) ctx.buf[120 + i] = static_cast<kl_u8>(bits >> (56 - 8*i));
    kl_sha512_block(ctx.h, ctx.buf);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            out[8*i + j] = static_cast<kl_u8>(ctx.h[i] >> (56 - 8*j));
}

// ── GF(2^255-19) field arithmetic ────────────────────────────────────────────
// Verbatim from TweetNaCl (constants verified against the source).

// Neutral element limbs
static const kl_gf kGf0 = {0};
static const kl_gf kGf1 = {1};

// d = -121665/121666 mod p  (Edwards curve constant)
static const kl_gf kD = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
// 2*d mod p
static const kl_gf kD2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
// sqrt(-1) mod p
static const kl_gf kI = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
// Base point X coordinate
static const kl_gf kBX = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
// Base point Y coordinate
static const kl_gf kBY = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};

// Carry propagation: normalize limbs to approximately [0, 2^16).
static void gf_car(kl_gf o) {
    kl_i64 c;
    for (int i = 0; i < 16; ++i) {
        o[i] += (kl_i64)1 << 16;
        c = o[i] >> 16;
        o[(i+1) * (i<15)] += c - 1 + 37*(c-1)*(i==15);
        o[i] -= c << 16;
    }
}

// Conditional swap: if b==1, swap p and q; else leave them.
static void gf_cswap(kl_gf p, kl_gf q, kl_u8 b) {
    kl_i64 t, mask = -(kl_i64)b;
    for (int i = 0; i < 16; ++i) {
        t = mask & (p[i] ^ q[i]);
        p[i] ^= t; q[i] ^= t;
    }
}

// Pack a field element into 32 bytes (little-endian canonical).
static void gf_pack(kl_u8 o[32], const kl_gf n) {
    kl_gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    gf_car(t); gf_car(t); gf_car(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i-1]>>16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14]>>16) & 1);
        int b = (m[15]>>16) & 1;
        m[14] &= 0xffff;
        gf_cswap(t, m, (kl_u8)(1-b));
    }
    for (int i = 0; i < 16; ++i) {
        o[2*i]   = static_cast<kl_u8>(t[i]);
        o[2*i+1] = static_cast<kl_u8>(t[i] >> 8);
    }
}

static int gf_neq(const kl_gf a, const kl_gf b) {
    kl_u8 c[32], d[32];
    gf_pack(c, a); gf_pack(d, b);
    int r = 0;
    for (int i = 0; i < 32; ++i) r |= c[i] ^ d[i];
    return r != 0;
}

static kl_u8 gf_par(const kl_gf a) {
    kl_u8 d[32]; gf_pack(d, a); return d[0] & 1;
}

static void gf_unpack(kl_gf o, const kl_u8 n[32]) {
    for (int i = 0; i < 16; ++i)
        o[i] = (kl_i64)n[2*i] + ((kl_i64)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

static void gf_add(kl_gf o, const kl_gf a, const kl_gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}
static void gf_sub(kl_gf o, const kl_gf a, const kl_gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}
static void gf_mul(kl_gf o, const kl_gf a, const kl_gf b) {
    kl_i64 t[31] = {};
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j)
            t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    gf_car(o); gf_car(o);
}
static void gf_sq(kl_gf o, const kl_gf a) { gf_mul(o, a, a); }

// x^((p-5)/8) used for sqrt recovery in point decompression.
static void gf_pow2523(kl_gf o, const kl_gf i) {
    kl_gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 250; a >= 0; --a) {
        gf_sq(c, c);
        if (a != 1) gf_mul(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

// x^(p-2) = 1/x mod p.
static void gf_inv(kl_gf o, const kl_gf i) {
    kl_gf c;
    for (int j = 0; j < 16; ++j) c[j] = i[j];
    for (int a = 253; a >= 0; --a) {
        gf_sq(c, c);
        if (a != 2 && a != 4) gf_mul(c, c, i);
    }
    for (int j = 0; j < 16; ++j) o[j] = c[j];
}

// ── Extended twisted Edwards point arithmetic ─────────────────────────────────
// Points in (X:Y:Z:T) where x=X/Z, y=Y/Z, T=X*Y/Z.

// Unified point addition (TweetNaCl's add()).
// Result stored in p: p ← p + q.
static void pt_add(kl_gf p[4], kl_gf q[4]) {
    kl_gf a, b, c, d, t, e, f, g, h;
    gf_sub(a, p[1], p[0]);
    gf_sub(t, q[1], q[0]);
    gf_mul(a, a, t);
    gf_add(b, p[0], p[1]);
    gf_add(t, q[0], q[1]);
    gf_mul(b, b, t);
    gf_mul(c, p[3], q[3]);
    gf_mul(c, c, kD2);
    gf_mul(d, p[2], q[2]);
    gf_add(d, d, d);
    gf_sub(e, b, a);
    gf_sub(f, d, c);
    gf_add(g, d, c);
    gf_add(h, b, a);
    gf_mul(p[0], e, f);
    gf_mul(p[1], h, g);
    gf_mul(p[2], g, f);
    gf_mul(p[3], e, h);
}

// Conditional swap of all four coordinates of a point.
static void pt_cswap(kl_gf p[4], kl_gf q[4], kl_u8 b) {
    for (int i = 0; i < 4; ++i) gf_cswap(p[i], q[i], b);
}

// Scalar multiplication: p ← [s]*q (TweetNaCl's scalarmult()).
// q is modified during computation (standard double-and-add ladder).
static void pt_scalarmult(kl_gf p[4], kl_gf q[4], const kl_u8 s[32]) {
    // Initialize p to the neutral element (0, 1, 1, 0).
    for (int j = 0; j < 16; ++j) {
        p[0][j] = kGf0[j];
        p[1][j] = kGf1[j];
        p[2][j] = kGf1[j];
        p[3][j] = kGf0[j];
    }
    for (int i = 255; i >= 0; --i) {
        kl_u8 b = (s[i/8] >> (i & 7)) & 1;
        pt_cswap(p, q, b);
        pt_add(q, p);   // q = q + p
        pt_add(p, p);   // p = 2*p
        pt_cswap(p, q, b);
    }
}

// Scalar multiplication against the standard base point B: p ← [s]*B.
static void pt_scalarbase(kl_gf p[4], const kl_u8 s[32]) {
    kl_gf q[4];
    for (int i = 0; i < 16; ++i) {
        q[0][i] = kBX[i];
        q[1][i] = kBY[i];
        q[2][i] = kGf1[i];
    }
    gf_mul(q[3], kBX, kBY);
    pt_scalarmult(p, q, s);
}

// Decompress a public key into extended coordinates, negating the X coordinate.
// On success, r represents the point -A (negated pubkey), returns true.
// On invalid encoding (not on curve), returns false.
static bool pt_unpackneg(kl_gf r[4], const kl_u8 p[32]) {
    kl_gf t, chk, num, den, den2, den4, den6;
    for (int i = 0; i < 16; ++i) r[2][i] = kGf1[i];
    gf_unpack(r[1], p);
    gf_sq(num, r[1]);
    gf_mul(den, num, kD);
    gf_sub(num, num, r[2]);       // num = y^2 - 1
    gf_add(den, r[2], den);       // den = d*y^2 + 1
    gf_sq(den2, den);
    gf_sq(den4, den2);
    gf_mul(den6, den4, den2);
    gf_mul(t, den6, num);
    gf_mul(t, t, den);
    gf_pow2523(t, t);             // t = (num/den)^((p-5)/8)
    gf_mul(t, t, num);
    gf_mul(t, t, den);
    gf_mul(t, t, den);
    gf_mul(r[0], t, den);         // r[0] = candidate X
    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) gf_mul(r[0], r[0], kI);  // try sqrt(-1)*X
    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) return false;   // not a valid point
    // Negate X to get -A: if current sign matches the encoded sign, flip it.
    if (gf_par(r[0]) == (p[31] >> 7))
        gf_sub(r[0], kGf0, r[0]);
    gf_mul(r[3], r[0], r[1]);
    return true;
}

// Compress a point from extended coordinates to 32 bytes.
static void pt_pack(kl_u8 r[32], kl_gf p[4]) {
    kl_gf tx, ty, zi;
    gf_inv(zi, p[2]);     // zi = 1/Z
    gf_mul(tx, p[0], zi);
    gf_mul(ty, p[1], zi);
    gf_pack(r, ty);
    r[31] ^= gf_par(tx) << 7;
}

// ── Scalar reduction mod L ────────────────────────────────────────────────────
// L = 2^252 + 27742317777372353535851937790883648493

static void kl_modL(kl_u8 r[32], kl_i64 x[64]) {
    static const kl_i64 L[32] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    };
    kl_i64 carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0; int j;
        for (j = i-32; j < i-12; ++j) {
            x[j] += carry - 16*x[i]*L[j-(i-32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry; x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31]>>4)*L[j];
        carry = x[j] >> 8; x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry*L[j];
    for (int i = 0; i < 32; ++i) {
        x[i+1] += x[i] >> 8;
        r[i] = static_cast<kl_u8>(x[i] & 255);
    }
}

// Reduce a 64-byte little-endian integer mod L, result → 32 bytes.
static void kl_reduce(kl_u8 out[32], const kl_u8 in[64]) {
    kl_i64 x[64];
    for (int i = 0; i < 64; ++i) x[i] = static_cast<kl_i64>(in[i]);
    kl_modL(out, x);
}

// Constant-time 32-byte comparison: returns 0 if equal.
static int kl_ct_eq32(const kl_u8* a, const kl_u8* b) {
    unsigned diff = 0;
    for (int i = 0; i < 32; ++i)
        diff |= static_cast<unsigned>(a[i] ^ b[i]);
    // Returns 0 if equal, nonzero if not.
    return (int)((diff | (0u - diff)) >> 31);
}

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────────

/// Verify an Ed25519 signature.
///
/// Implements the standard Ed25519 verification equation:
///   [S]·B = R + [h]·A   where h = SHA-512(R ‖ A ‖ M) mod L
///
/// Equivalently verified as: pack([S]·B + [h]·(-A)) == R
///
/// @param msg      Pointer to message bytes. May be nullptr when msg_len == 0.
/// @param msg_len  Length of the message in bytes.
/// @param sig      64-byte signature (R ‖ S, little-endian).
/// @param pubkey   32-byte compressed public key.
/// @return         true if and only if the signature is valid.
inline bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                            const std::array<uint8_t, 64>& sig,
                            const std::array<uint8_t, 32>& pubkey) {
    // Quick reject: high 3 bits of S (sig[63]) must be clear (S < 2^253 < L*8).
    if (sig[63] & 0xe0) return false;

    // Decompress pubkey A and store as -A in extended coordinates.
    kl_gf A[4];
    if (!pt_unpackneg(A, pubkey.data())) return false;

    // h = SHA-512(R ‖ A ‖ M) mod L
    // where R = sig[0..31], A = pubkey[0..31].
    kl_u8 prefix[64];
    for (int i = 0; i < 32; ++i) prefix[i]    = sig[i];
    for (int i = 0; i < 32; ++i) prefix[32+i] = pubkey[i];

    KlSha512Ctx sha_ctx;
    kl_sha512_init(sha_ctx);
    kl_sha512_update(sha_ctx, prefix, 64);
    if (msg_len > 0) kl_sha512_update(sha_ctx, msg, msg_len);
    kl_u8 hram[64];
    kl_sha512_final(sha_ctx, hram);

    kl_u8 h[32];
    kl_reduce(h, hram);

    // [h]*(-A): scalar mult of -A by h. A is modified but we don't need it after.
    kl_gf hA[4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 16; ++j)
            hA[i][j] = A[i][j];
    pt_scalarmult(hA, A, h);  // hA = [h]*(-A), A (the copy) is modified

    // [S]*B: scalar mult of base point by S = sig[32..63].
    kl_gf SB[4];
    pt_scalarbase(SB, sig.data() + 32);

    // R_computed = [S]*B + [h]*(-A)
    pt_add(SB, hA);

    // Pack and compare to R = sig[0..31].
    kl_u8 R_computed[32];
    pt_pack(R_computed, SB);

    return kl_ct_eq32(sig.data(), R_computed) == 0;
}

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/verifier.hpp
// ──────────────────────────────────────────────────────────────────────────

// keylight/verifier.hpp — Ed25519 lease verifier with clock-skew tolerance.
// Ported from keylight-rust keylight/src/verifier.rs



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

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/transport.hpp
// ──────────────────────────────────────────────────────────────────────────

// ---------------------------------------------------------------------------
// keylight/transport.hpp — zero-dependency HTTP transport interface
//
// This header is part of the core library and has NO external dependencies.
// Do NOT include httplib.h, OpenSSL, or any platform SDK here.
// ---------------------------------------------------------------------------

namespace keylight {

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------
struct HttpResponse {
    int         status = 0;
    std::string body;
};

// ---------------------------------------------------------------------------
// Transport — abstract HTTP seam
// ---------------------------------------------------------------------------
class Transport {
public:
    virtual ~Transport() = default;

    /// Perform an HTTP request.
    /// @param method   HTTP verb ("GET", "POST", …)
    /// @param url      Fully-qualified URL, e.g. "https://api.keylight.dev/v1/…"
    /// @param headers  Request headers (including Content-Type, Authorization, …)
    /// @param body     Request body (may be empty for GET/DELETE)
    /// @returns        Result<HttpResponse> — err(ErrorCode::Network) on I/O failure
    virtual Result<HttpResponse> request(
        const std::string&                        method,
        const std::string&                        url,
        const std::map<std::string, std::string>& headers,
        const std::string&                        body
    ) = 0;
};

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/store.hpp
// ──────────────────────────────────────────────────────────────────────────


namespace keylight {

// ---------------------------------------------------------------------------
// LicenseStore — abstract cache seam for persisting the verified lease blob
// ---------------------------------------------------------------------------
class LicenseStore {
public:
    virtual ~LicenseStore() = default;

    // Returns the stored lease blob, or an ok Result with an empty string if
    // no lease has been saved yet. A missing file is NOT an error.
    virtual Result<std::string> load() = 0;

    // Persists the lease blob. Implementations should write atomically so
    // a crash during save never leaves a half-written file behind.
    virtual Result<void> save(const std::string& data) = 0;

    // Removes the stored lease. Removing a file that does not exist is NOT
    // an error.
    virtual Result<void> clear() = 0;
};

// ---------------------------------------------------------------------------
// FileStore — default on-disk implementation
//
// save() writes atomically: data → temp file → std::filesystem::rename.
// Parent directories are created on first save.
// All filesystem_errors are caught and mapped to Result::err(ErrorCode::Io).
// ---------------------------------------------------------------------------
class FileStore : public LicenseStore {
public:
    explicit FileStore(std::string path) : path_(std::move(path)) {}

    Result<std::string> load() override {
        namespace fs = std::filesystem;
        try {
            if (!fs::exists(path_)) {
                return Result<std::string>::ok(std::string{});
            }
            std::ifstream f(path_, std::ios::binary);
            if (!f) {
                return Result<std::string>::err(
                    {ErrorCode::Io, "FileStore: cannot open " + path_});
            }
            std::string data(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
            return Result<std::string>::ok(std::move(data));
        } catch (const std::filesystem::filesystem_error& e) {
            return Result<std::string>::err({ErrorCode::Io, e.what()});
        }
    }

    Result<void> save(const std::string& data) override {
        namespace fs = std::filesystem;
        try {
            fs::path target(path_);

            // Create parent directories if they don't exist
            if (target.has_parent_path()) {
                fs::create_directories(target.parent_path());
            }

            // Write to a sibling temp file, then rename atomically
            fs::path tmp = target;
            tmp += ".tmp";

            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    return Result<void>::err(
                        {ErrorCode::Io, "FileStore: cannot write " + tmp.string()});
                }
                f.write(data.data(), static_cast<std::streamsize>(data.size()));
                if (!f) {
                    return Result<void>::err(
                        {ErrorCode::Io, "FileStore: write failed for " + tmp.string()});
                }
            } // flush + close before rename

            fs::rename(tmp, target);
            return Result<void>::ok();
        } catch (const std::filesystem::filesystem_error& e) {
            return Result<void>::err({ErrorCode::Io, e.what()});
        }
    }

    Result<void> clear() override {
        namespace fs = std::filesystem;
        try {
            std::error_code ec;
            fs::remove(path_, ec);
            // Ignore ec: removing a non-existent file is not an error
            return Result<void>::ok();
        } catch (const std::filesystem::filesystem_error& e) {
            return Result<void>::err({ErrorCode::Io, e.what()});
        }
    }

private:
    std::string path_;
};

// ---------------------------------------------------------------------------
// default_store_path — sensible per-tenant/per-product path
//
// POSIX: $HOME/.keylight/<tenantId>-<productId>.lease
// Fallback: /tmp/.keylight/<tenantId>-<productId>.lease
// ---------------------------------------------------------------------------
inline std::string default_store_path(const Config& cfg) {
    namespace fs = std::filesystem;

    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) / ".keylight"
                         : fs::temp_directory_path() / ".keylight";

    std::string filename = cfg.tenantId + "-" + cfg.productId + ".lease";
    return (base / filename).string();
}

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/sha256.hpp
// ──────────────────────────────────────────────────────────────────────────

// Keylight SHA-256 — header-only, zero external dependencies.
// Core transform adapted from Brad Conte's public-domain sha256.c
// (https://github.com/B-Con/crypto-algorithms), CC0 / public domain.
// Wrapped in namespace keylight; internals in anonymous namespace.
//
// PUBLIC UTILITY — standalone SHA-256 for integrators (e.g. hashing license
// keys, building custom audit trails).  The core SDK verification path does
// NOT use this header (Ed25519 uses SHA-512 internally); sha256.hpp is
// therefore NOT part of keylight.hpp's include closure.
// Include it directly when you need it:
//   #include <keylight/sha256.hpp>


namespace keylight {

namespace {

// ── SHA-256 constants ─────────────────────────────────────────────────────────

static const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

// ── Bit-manipulation helpers ──────────────────────────────────────────────────

inline uint32_t rotr32(uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}

inline uint32_t ch(uint32_t e, uint32_t f, uint32_t g) noexcept {
    return (e & f) ^ (~e & g);
}

inline uint32_t maj(uint32_t a, uint32_t b, uint32_t c) noexcept {
    return (a & b) ^ (a & c) ^ (b & c);
}

inline uint32_t ep0(uint32_t a) noexcept {
    return rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
}

inline uint32_t ep1(uint32_t e) noexcept {
    return rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
}

inline uint32_t sig0(uint32_t x) noexcept {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

inline uint32_t sig1(uint32_t x) noexcept {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

// ── SHA-256 context ───────────────────────────────────────────────────────────

struct Sha256Ctx {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
};

inline void sha256_init(Sha256Ctx& ctx) noexcept {
    ctx.datalen = 0;
    ctx.bitlen  = 0;
    ctx.state[0] = 0x6a09e667u;
    ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u;
    ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu;
    ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu;
    ctx.state[7] = 0x5be0cd19u;
}

inline void sha256_transform(Sha256Ctx& ctx, const uint8_t* data) noexcept {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];

    for (unsigned i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (static_cast<uint32_t>(data[j    ]) << 24)
              | (static_cast<uint32_t>(data[j + 1]) << 16)
              | (static_cast<uint32_t>(data[j + 2]) <<  8)
              |  static_cast<uint32_t>(data[j + 3]);

    for (unsigned i = 16; i < 64; ++i)
        m[i] = sig1(m[i - 2]) + m[i - 7] + sig0(m[i - 15]) + m[i - 16];

    a = ctx.state[0]; b = ctx.state[1]; c = ctx.state[2]; d = ctx.state[3];
    e = ctx.state[4]; f = ctx.state[5]; g = ctx.state[6]; h = ctx.state[7];

    for (unsigned i = 0; i < 64; ++i) {
        t1 = h + ep1(e) + ch(e, f, g) + kK[i] + m[i];
        t2 = ep0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
    ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
}

inline void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) {
        ctx.data[ctx.datalen++] = data[i];
        if (ctx.datalen == 64) {
            sha256_transform(ctx, ctx.data);
            ctx.bitlen += 512;
            ctx.datalen = 0;
        }
    }
}

inline void sha256_final(Sha256Ctx& ctx, uint8_t* hash) noexcept {
    uint32_t i = ctx.datalen;

    // Pad message
    if (ctx.datalen < 56) {
        ctx.data[i++] = 0x80u;
        while (i < 56) ctx.data[i++] = 0x00u;
    } else {
        ctx.data[i++] = 0x80u;
        while (i < 64) ctx.data[i++] = 0x00u;
        sha256_transform(ctx, ctx.data);
        for (unsigned k = 0; k < 56; ++k) ctx.data[k] = 0x00u;
    }

    // Append bit length (big-endian)
    ctx.bitlen += static_cast<uint64_t>(ctx.datalen) * 8u;
    ctx.data[63] = static_cast<uint8_t>( ctx.bitlen        & 0xffu);
    ctx.data[62] = static_cast<uint8_t>((ctx.bitlen >>  8) & 0xffu);
    ctx.data[61] = static_cast<uint8_t>((ctx.bitlen >> 16) & 0xffu);
    ctx.data[60] = static_cast<uint8_t>((ctx.bitlen >> 24) & 0xffu);
    ctx.data[59] = static_cast<uint8_t>((ctx.bitlen >> 32) & 0xffu);
    ctx.data[58] = static_cast<uint8_t>((ctx.bitlen >> 40) & 0xffu);
    ctx.data[57] = static_cast<uint8_t>((ctx.bitlen >> 48) & 0xffu);
    ctx.data[56] = static_cast<uint8_t>((ctx.bitlen >> 56) & 0xffu);
    sha256_transform(ctx, ctx.data);

    // Produce big-endian digest bytes
    for (unsigned j = 0; j < 4; ++j) {
        hash[     j] = static_cast<uint8_t>((ctx.state[0] >> (24 - j * 8)) & 0xffu);
        hash[ 4 + j] = static_cast<uint8_t>((ctx.state[1] >> (24 - j * 8)) & 0xffu);
        hash[ 8 + j] = static_cast<uint8_t>((ctx.state[2] >> (24 - j * 8)) & 0xffu);
        hash[12 + j] = static_cast<uint8_t>((ctx.state[3] >> (24 - j * 8)) & 0xffu);
        hash[16 + j] = static_cast<uint8_t>((ctx.state[4] >> (24 - j * 8)) & 0xffu);
        hash[20 + j] = static_cast<uint8_t>((ctx.state[5] >> (24 - j * 8)) & 0xffu);
        hash[24 + j] = static_cast<uint8_t>((ctx.state[6] >> (24 - j * 8)) & 0xffu);
        hash[28 + j] = static_cast<uint8_t>((ctx.state[7] >> (24 - j * 8)) & 0xffu);
    }
}

static const char kHexChars[] = "0123456789abcdef";

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────────

/// Hash `len` bytes at `data`; return raw 32-byte digest.
inline std::array<uint8_t, 32> sha256_bytes(const uint8_t* data, size_t len) noexcept {
    Sha256Ctx ctx;
    sha256_init(ctx);
    sha256_update(ctx, data, len);
    std::array<uint8_t, 32> digest{};
    sha256_final(ctx, digest.data());
    return digest;
}

/// Hash a UTF-8 string; return 64-char lowercase hex digest.
inline std::string sha256_hex(const std::string& input) {
    auto digest = sha256_bytes(
        reinterpret_cast<const uint8_t*>(input.data()), input.size());
    std::string hex;
    hex.reserve(64);
    for (uint8_t byte : digest) {
        hex += kHexChars[(byte >> 4) & 0xfu];
        hex += kHexChars[ byte       & 0xfu];
    }
    return hex;
}

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/machine_id.hpp
// ──────────────────────────────────────────────────────────────────────────

// keylight/machine_id.hpp — device identity for the anonymous keyless beacon.
//
// Ported from keylight-rust keylight/src/machine.rs and
//            keylight/src/store/device.rs
//
// `machine_hash` lets the server dedupe keyless/free-tier devices by a *true*
// OS/hardware identifier instead of a random per-install id.  It is only ever
// computed from a real hardware id — callers MUST omit the field entirely when
// read_hardware_id() returns nullopt.  Substituting a random fallback would
// defeat the cross-install dedup the hash exists for.



#if defined(__APPLE__)
#  include <CoreFoundation/CoreFoundation.h>
#  include <IOKit/IOKitLib.h>
#elif defined(_WIN32)
#  include <windows.h>
#else
#  include <fstream>
#  include <sstream>
#endif

namespace keylight {
namespace detail {

/// sha256("keylight-keyless-machine-v1|{tenant}|{product}|{stable_id}"),
/// lowercase hex.  Must match byte-for-byte across all Keylight SDKs.
inline std::string machine_hash(const std::string& tenant_id,
                                const std::string& product_id,
                                const std::string& stable_id)
{
    return sha256_hex("keylight-keyless-machine-v1|" + tenant_id + "|" +
                      product_id + "|" + stable_id);
}

#if !defined(__APPLE__) && !defined(_WIN32)
/// Read a file and trim surrounding whitespace; nullopt when missing or blank.
inline std::optional<std::string> read_file_trimmed_(const char* path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    const char* ws = " \t\n\r\f\v";
    const auto  b  = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::nullopt;
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}
#endif

/// True OS/hardware machine id, or nullopt when the platform has none.
/// NEVER returns a random fallback — see the header comment.
inline std::optional<std::string> read_hardware_id() {
#if defined(__APPLE__)
    // MACH_PORT_NULL selects the default port on every macOS version, which
    // sidesteps the kIOMasterPortDefault -> kIOMainPortDefault rename in the
    // macOS 12 SDK.
    io_service_t svc = IOServiceGetMatchingService(
        MACH_PORT_NULL, IOServiceMatching("IOPlatformExpertDevice"));
    if (!svc) return std::nullopt;
    CFTypeRef prop = IORegistryEntryCreateCFProperty(
        svc, CFSTR("IOPlatformUUID"), kCFAllocatorDefault, 0);
    IOObjectRelease(svc);
    if (!prop) return std::nullopt;
    std::optional<std::string> out;
    if (CFGetTypeID(prop) == CFStringGetTypeID()) {
        char buf[128] = {0};
        if (CFStringGetCString(static_cast<CFStringRef>(prop), buf, sizeof(buf),
                               kCFStringEncodingUTF8) && buf[0] != '\0') {
            out = std::string(buf);
        }
    }
    CFRelease(prop);
    return out;

#elif defined(_WIN32)
    // RRF_SUBKEY_WOW6464KEY forces the 64-bit registry view.  Without it a
    // 32-bit plugin host is redirected and reads a DIFFERENT MachineGuid than
    // the other SDKs, silently breaking the cross-SDK hash.
    char  buf[256];
    DWORD size = sizeof(buf);
    const LSTATUS rc = ::RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Cryptography",
        "MachineGuid",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
        nullptr, buf, &size);
    if (rc != ERROR_SUCCESS || buf[0] == '\0') return std::nullopt;
    return std::string(buf);

#else
    if (auto v = read_file_trimmed_("/etc/machine-id")) return v;
    return read_file_trimmed_("/var/lib/dbus/machine-id");
#endif
}

/// RFC 4122 version-4 UUID.  Used for the anonymous free-tier instance id.
inline std::string uuid_v4() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<unsigned> dist(0, 255);

    uint8_t b[16];
    for (auto& x : b) x = static_cast<uint8_t>(dist(gen));
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);  // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);  // variant

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += kHex[(b[i] >> 4) & 0x0f];
        out += kHex[ b[i]       & 0x0f];
    }
    return out;
}

} // namespace detail
} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/client.hpp
// ──────────────────────────────────────────────────────────────────────────

// keylight/client.hpp — Client state machine: activate/validate/deactivate/
//                       checkOnLaunch/refreshIfNeeded + events + offline grace.
// Ported from keylight-rust keylight/src/client.rs and
//            keylight-csharp src/Keylight/Keylight.cs
//
// URL pattern:  {baseUrl}/{tenantId}/{productId}/{action}
// Activate:     POST /{tenantId}/{productId}/activate
// Validate:     POST /{tenantId}/{productId}/validate
// Deactivate:   POST /{tenantId}/{productId}/deactivate
//
// Thread-safety: state() reads std::atomics and the injected clock, and takes
//                no lock — audio-thread safe. See the NOW-FUNCTION CONTRACT
//                below for what that requires of the clock.
//                hasEntitlement / cachedLicenseExpiresAt / listener list are
//                guarded by a mutex.
//
// NOW-FUNCTION CONTRACT
// ─────────────────────
// Client takes an optional `now_fn` (a std::function<int64_t()>) so tests and
// integrators can supply the clock. state() calls it — to run the clock-
// rollback guard — and state() is `noexcept` and documented audio-thread safe.
// A caller-supplied `now_fn` MUST therefore be:
//   - non-throwing        — an exception escaping a noexcept function is
//                           std::terminate, and here that would happen on the
//                           audio thread;
//   - non-blocking        — no mutex, no I/O, no syscall that can wait;
//   - allocation-free     — no heap traffic on the audio thread.
// It must also be non-empty: invoking an empty std::function throws
// std::bad_function_call, which is the same std::terminate.
// The shipped default, std::time(nullptr), satisfies all of this. If your
// clock cannot, do not call state() from an audio callback — mirror it into
// your own std::atomic from a background thread instead (this is exactly what
// the JUCE adapter does).



namespace keylight {

// ---------------------------------------------------------------------------
// State — high-level license state (C++ subset of Rust/C# states)
// ---------------------------------------------------------------------------
enum class State {
    Licensed,   // trusted, unexpired active lease
    Trial,      // no license; within trial window
    Expired,    // trusted lease expired, or license status "expired"
    Invalid,    // no trusted lease, no active trial
    FreeTier,   // no license and no trial, but the product offers a free tier.
                // Appended last on purpose: renumbering the values above would
                // break any integrator that persisted a State as an integer.
    Limited,    // trusted lease with server status "fallback": the server could
                // not mint a full lease, so the app runs degraded rather than
                // locked. Appended last for the same reason as FreeTier —
                // renumbering would break any integrator that persisted a
                // State as an integer.
};

// ---------------------------------------------------------------------------
// TrialStatus — local, offline-first trial (mirrors keylight-rust TrialStatus)
//
// Trials are entirely local: the start timestamp is persisted next to the
// lease and the window is measured against the client's clock. No API call is
// involved — the free-tier / keyless beacon is a separate feature.
// ---------------------------------------------------------------------------
enum class TrialStatus {
    NotStarted, // no trial timestamp persisted (or trials disabled)
    Active,     // within trialDurationDays of the persisted start
    Expired,    // the trial window has elapsed
};

// ---------------------------------------------------------------------------
// KeylessState — what the anonymous keyless beacon reports (mirrors
// keylight-rust KeylessState).  The wire strings are fixed by the server and
// shared with every other Keylight SDK.
// ---------------------------------------------------------------------------
enum class KeylessState {
    Trial,
    FreeTier,
    Expired,
};

inline const char* keyless_state_wire(KeylessState s) {
    switch (s) {
        case KeylessState::Trial:    return "trial";
        case KeylessState::FreeTier: return "free_tier";
        case KeylessState::Expired:  return "expired";
    }
    return "expired";
}

// ---------------------------------------------------------------------------
// compile-time platform string (matches Rust telemetry.rs)
// ---------------------------------------------------------------------------
namespace detail {
inline const char* current_platform() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32) || defined(_WIN64)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

// 128 bits of hex for the X-Keylight-Request-Id correlation header. Not a
// security token — it exists so an app log line and a worker log line can be
// matched up during support, so a per-thread PRNG is sufficient.
inline std::string random_request_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* kHex = "0123456789abcdef";

    std::string out;
    out.reserve(32);
    for (int i = 0; i < 4; ++i) {
        uint64_t chunk = rng();
        for (int j = 0; j < 8; ++j) {
            out += kHex[chunk & 0xF];
            chunk >>= 4;
        }
    }
    return out;
}
} // namespace detail

// ---------------------------------------------------------------------------
// Timer-model constants (ported verbatim from keylight-rust client.rs)
// ---------------------------------------------------------------------------
static constexpr int64_t REFRESH_DEBOUNCE  =   300; // 5 min
static constexpr int64_t REFRESH_STALE     = 21600; // 6 h
static constexpr int64_t NEAR_EXPIRY_SECS  = 86400; // 24 h — refresh when lease < 24h away

// ---------------------------------------------------------------------------
// Subscription — RAII handle returned by on() / subscribe().
// Calling unsubscribe() (or letting the handle go out of scope / be moved-from)
// removes the callback from the client's listener list.
// ---------------------------------------------------------------------------
class Client; // forward

class Subscription {
public:
    // Default-constructed handle is a no-op.
    Subscription() = default;

    // Move-only.
    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& o) noexcept
        : client_(o.client_), id_(o.id_) { o.client_ = nullptr; }

    Subscription& operator=(Subscription&& o) noexcept {
        if (this != &o) {
            unsubscribe();
            client_ = o.client_;
            id_     = o.id_;
            o.client_ = nullptr;
        }
        return *this;
    }

    ~Subscription() { unsubscribe(); }

    void unsubscribe();

private:
    friend class Client;
    explicit Subscription(Client* c, uint64_t id) : client_(c), id_(id) {}

    Client*  client_ = nullptr;
    uint64_t id_     = 0;
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
class Client {
public:
    // Production constructor — clock defaults to real wall clock.
    Client(Config cfg, Transport& transport, LicenseStore& store)
        : Client(std::move(cfg), transport, store,
                 []{ return static_cast<int64_t>(std::time(nullptr)); },
                 []{ return detail::read_hardware_id(); })
    {}

    // Testable constructor — inject a deterministic clock.
    // now_fn() must return Unix epoch seconds as int64_t.
    Client(Config                   cfg,
           Transport&               transport,
           LicenseStore&            store,
           std::function<int64_t()> now_fn)
        : Client(std::move(cfg), transport, store, std::move(now_fn),
                 []{ return detail::read_hardware_id(); })
    {}

    // Testable constructor — inject a deterministic clock AND hardware id.
    // hardware_id_fn() returns the true OS/hardware id, or nullopt when the
    // platform has none.  It must NEVER return a random per-install value:
    // machine_hash exists to dedupe a device across reinstalls, and a random
    // fallback would defeat exactly that.
    Client(Config                                      cfg,
           Transport&                                  transport,
           LicenseStore&                               store,
           std::function<int64_t()>                    now_fn,
           std::function<std::optional<std::string>()> hardware_id_fn)
        : cfg_(std::move(cfg))
        , transport_(transport)
        , store_(store)
        , now_fn_(std::move(now_fn))
        , hardware_id_fn_(std::move(hardware_id_fn))
        , verifier_(cfg_.trustedKeys)
        , state_(State::Invalid)
    {
        // Prime state from persisted store (if any) on construction.
        refresh_state_from_store_();
        // Seed the event dedupe with what we booted with. Nobody can have
        // subscribed yet, so this is not a suppressed event -- it stops the
        // first notify_() poll from reporting the initial state as a change.
        last_reported_.store(state());
    }

    // Destructor: stops and joins any running auto-validation thread so the
    // thread cannot outlive the Client (no detached threads, no std::terminate).
    ~Client() {
        stopAutoValidation();
    }

    // ── Sync API ──────────────────────────────────────────────────────────

    /// Activate a license key.  Returns the resulting State.
    /// On an unrecognised/invalid lease the store is NOT updated and
    /// State::Invalid is returned (no exception thrown).
    Result<State> activate(const std::string& key) {
        // Build activate request body
        // A real hostname, not a constant: this string is what the customer
        // sees in their device list. "device" survives only as the fallback
        // when the platform read fails.
        std::string instance_name = detail::detect_machine_name();
        if (instance_name.empty()) instance_name = "device";

        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key",   json_str(key)},
            {"instance_name", json_str(instance_name)},
        };
        append_attribution_fields_(fields, /*include_instance_id=*/true);
        std::string body = build_json_(std::move(fields), true /*telemetry*/);

        std::string url = api_url_("activate");
        auto hr = transport_.request("POST", url, json_headers_(), body);
        if (!hr.is_ok()) {
            return Result<State>::err(hr.error());
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
            return Result<State>::err({ErrorCode::Http,
                http_error_message_(resp.body, "activate", resp.status)});
        }

        // Parse activate response
        auto jr = Json::parse(resp.body);
        if (!jr.is_ok()) {
            return Result<State>::err({ErrorCode::BadResponse, "activate: invalid JSON"});
        }
        const Json& j = jr.value();

        bool activated = j["activated"].as_bool();
        if (!activated) {
            // Server declined — keep existing state
            return report_(state_.load());
        }

        // Parse optional lease (present when the object has sub-keys)
        std::optional<Lease> lease;
        auto lease_node = j["lease"];
        if (lease_node.size() > 0) {
            auto lr = Lease::from_json(lease_node);
            if (!lr.is_ok()) {
                return Result<State>::err(lr.error());
            }
            lease = lr.value();
        }

        // Parse optional license_expires_at (0 means absent/null)
        std::optional<int64_t> expires_at;
        {
            int64_t v = j["license_expires_at"].as_int();
            if (v != 0) expires_at = v;
        }

        // Parse optional instance_id
        std::optional<std::string> instance_id;
        {
            std::string v = j["instance_id"].as_string();
            if (!v.empty()) instance_id = v;
        }

        // Resolve state from the returned lease (verify but don't persist
        // on invalid signature)
        State new_state = resolve_from_lease_(lease);

        // Persist only trusted leases
        if (lease.has_value() && verifier_.verify(*lease, now_fn_()).is_trusted()) {
            std::string lease_json = lease_to_json_(*lease);
            persist_({lease_json, expires_at, instance_id, key});
            save_last_validated_online_(now_fn_());
        } else if (!lease.has_value() && activated) {
            // Server said activated=true but sent no lease — treat as Licensed
            // without a local lease; persist what we have.
            persist_({std::nullopt, expires_at, instance_id, key});
            new_state = State::Licensed;
        }

        new_state = resolve_with_trial_(new_state);
        set_state_(new_state);
        return report_(new_state);
    }

    /// Validate the stored license online.  Returns the resulting State.
    Result<State> validate() {
        // Poll the clock guard, for the same reason refreshIfNeeded() does:
        // a host that polls validate() on its own timer would otherwise get
        // the guard in state() and never in its callback.
        notify_();

        // Need license_key and instance_id from cache (Worker requires both)
        std::string license_key  = load_license_key_();
        std::string instance_id  = load_instance_id_();

        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key", json_str(license_key)},
            {"instance_id", json_str(instance_id)},
        };
        // machine_hash only — the free-tier id belongs on activate, which is
        // where a conversion is actually recorded (keylight-rust does the same;
        // deactivate gets neither, it already identifies the device).
        append_attribution_fields_(fields, /*include_instance_id=*/false);
        std::string body = build_json_(std::move(fields), true /*telemetry*/);

        std::string url = api_url_("validate");
        auto hr = transport_.request("POST", url, json_headers_(), body);
        if (!hr.is_ok()) {
            // Network failure: keep existing state
            return report_(state_.load());
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
            // 422 is the worker's definitive-rejection status for /validate
            // (revoke, deactivated instance, expired/fallback lease) — parse
            // the body instead of treating it as transient. Any other
            // non-200 status keeps the existing state (unchanged behavior).
            if (resp.status == 422) {
                auto rejected = handle_validate_rejection_(resp.body, now_fn_());
                if (rejected.has_value()) {
                    return report_(*rejected);
                }
            }
            return report_(state_.load());
        }

        auto jr = Json::parse(resp.body);
        if (!jr.is_ok()) {
            return report_(state_.load());
        }
        const Json& j = jr.value();

        // Parse optional lease
        std::optional<Lease> lease;
        auto lease_node = j["lease"];
        if (lease_node.size() > 0) {
            auto lr = Lease::from_json(lease_node);
            if (lr.is_ok()) {
                lease = lr.value();
            }
        }

        // Parse optional license_expires_at (0 means absent/null)
        std::optional<int64_t> expires_at;
        {
            int64_t v = j["license_expires_at"].as_int();
            if (v != 0) expires_at = v;
        }

        // Update cached lease if server returned one
        if (lease.has_value() && verifier_.verify(*lease, now_fn_()).is_trusted()) {
            std::string lease_json = lease_to_json_(*lease);
            // Keep existing instance_id
            std::optional<std::string> iid;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (cached_instance_id_.has_value()) {
                    iid = cached_instance_id_;
                }
            }
            persist_({lease_json, expires_at, iid});
            save_last_validated_online_(now_fn_());
        }

        // Paid licensing wins; an unusable result falls back to the local
        // trial instead of dropping a trialling user to Invalid.
        State new_state = resolve_with_trial_(resolve_from_lease_(lease));
        set_state_(new_state);
        return report_(new_state);
    }

    /// Deactivate this device.  Clears the local cache regardless of the
    /// network outcome, but no longer hides a server rejection: a 4xx here
    /// means the seat is still consumed, and only the caller can decide to
    /// retry.
    Result<void> deactivate() {
        std::string instance_id = load_instance_id_();
        std::string license_key = load_license_key_();

        std::optional<Error> server_error;
        if (!instance_id.empty()) {
            // The worker requires BOTH fields (DeactivateBodySchema); sending
            // instance_id alone is rejected by zod and frees nothing.
            std::string body = build_json_({
                {"license_key", json_str(license_key)},
                {"instance_id", json_str(instance_id)},
            }, false);
            std::string url = api_url_("deactivate");
            auto hr = transport_.request("POST", url, json_headers_(), body);
            if (!hr.is_ok()) {
                server_error = hr.error();
            } else if (hr.value().status != 200) {
                server_error = Error{ErrorCode::Http,
                    http_error_message_(hr.value().body, "deactivate",
                                        hr.value().status)};
            }
        }

        // Clear the paid-licensing half of the cache. The trial start survives:
        // deactivating a paid license must never hand the user a fresh trial
        // (nor restart an expired one) — see startTrial().
        bool keep_trial = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_lease_                  = std::nullopt;
            cached_expires_at_             = std::nullopt;
            cached_instance_id_            = std::nullopt;
            cached_license_key_            = std::nullopt;
            cached_last_validated_online_  = 0;
            last_validated_online_atomic_.store(0);
            keep_trial                     = cached_trial_start_.has_value();
        }

        if (keep_trial) {
            // Rewrite the blob with the trial start (and nothing else) instead
            // of dropping the file — clear() would restart the trial clock.
            auto sr = save_cache_();
            if (!sr.is_ok()) {
                return sr;
            }
        } else {
            auto cr = store_.clear();
            if (!cr.is_ok()) {
                return cr;
            }
        }

        // No paid license left: the persisted trial (if any) decides the state.
        set_state_(resolve_with_trial_(State::Invalid));

        if (server_error.has_value()) {
            return Result<void>::err(*server_error);
        }
        return Result<void>::ok();
    }

    // ── Trial API (local, offline-first) ──────────────────────────────────

    /// Explicitly begin the local trial. Idempotent: an existing trial start
    /// is never overwritten, so an expired trial cannot be restarted by
    /// calling this again (or by deactivating a paid license and re-calling).
    /// No-op when trials are disabled (Config::trialDurationDays <= 0).
    /// Performs store I/O — never call this from an audio thread.
    /// Anonymous, per-install identifier for keyless/free-tier reporting.
    /// Minted on first use and persisted; never derived from a licence or from
    /// hardware.  Returns an empty string only when the store write fails.
    /// Mirrors keylight-rust free_tier_instance_id().
    std::string freeTierInstanceId() {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_free_tier_instance_id_.has_value()) {
                return *cached_free_tier_instance_id_;
            }
            cached_free_tier_instance_id_ = detail::uuid_v4();
        }
        if (!save_cache_().is_ok()) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_free_tier_instance_id_.reset();
            return {};
        }
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return *cached_free_tier_instance_id_;
    }

    /// Anonymous keyless/free-tier beacon.  Fire-and-forget: every error is
    /// swallowed, nothing is thrown, and the resolved state never changes.
    /// Debounced to once per 24h per state — a state *change* always sends.
    ///
    /// Nothing calls this for you.  keylight-rust behaves the same way: the
    /// core never emits network traffic the integrator did not ask for, which
    /// is what keeps checkOnLaunch() free of network I/O while a DAW scans the
    /// plugin.  The JUCE adapter wires it to state transitions for you.
    ///
    /// Blocking network call — never invoke it from an audio thread.
    void reportKeylessState(KeylessState state) {
        const std::string wire = keyless_state_wire(state);

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            const bool changed =
                !cached_keyless_last_state_.has_value() ||
                *cached_keyless_last_state_ != wire;
            const bool within_24h =
                cached_last_keyless_ping_at_ != 0 &&
                (now_fn_() - cached_last_keyless_ping_at_) < 86400;
            if (!changed && within_24h) {
                return;
            }
        }

        const std::string instance = freeTierInstanceId();
        if (instance.empty()) {
            return;   // could not persist an id; nothing to report under
        }

        std::vector<std::pair<std::string, std::string>> fields{
            {"instance_id", json_str(instance)},
            {"state",       json_str(wire)},
        };
        if (auto hash = machine_hash_()) {
            fields.push_back({"machine_hash", json_str(*hash)});
        }

        auto hr = transport_.request("POST", api_url_("keyless"),
                                     json_headers_(),
                                     build_json_(std::move(fields), true));
        // Arm the debounce ONLY on a real 200.  A failed beacon must not
        // suppress reporting for a day (keylight-rust does the same).
        if (!hr.is_ok() || hr.value().status != 200) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_keyless_last_state_   = wire;
            cached_last_keyless_ping_at_ = now_fn_();
        }
        (void)save_cache_();
    }

    Result<State> startTrial() {
        if (cfg_.trialDurationDays <= 0) {
            // Trials disabled — nothing is persisted and no state changes.
            return report_(state_.load());
        }

        bool started = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (!cached_trial_start_.has_value()) {
                cached_trial_start_ = now_fn_();
                started             = true;
            }
        }
        if (started) {
            save_cache_();
        }
        // Attribution: a trial that later converts must carry the same
        // anonymous id the keyless beacon reported it under.
        freeTierInstanceId();

        State new_state = resolve_current_state_();
        set_state_(new_state);
        return report_(new_state);
    }

    /// Current local trial status. Never performs I/O beyond reading the
    /// in-memory cache primed from the store.
    TrialStatus checkTrial() const {
        if (cfg_.trialDurationDays <= 0) {
            return TrialStatus::NotStarted;
        }
        std::optional<int64_t> start;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            start = cached_trial_start_;
        }
        if (!start.has_value()) {
            return TrialStatus::NotStarted;
        }
        return days_left_from_(*start) > 0 ? TrialStatus::Active
                                           : TrialStatus::Expired;
    }

    /// Whole days remaining in the local trial; 0 when disabled, not started,
    /// or elapsed. Matches keylight-rust's `days_left` (seconds / 86400).
    int trialDaysLeft() const {
        if (cfg_.trialDurationDays <= 0) {
            return 0;
        }
        std::optional<int64_t> start;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            start = cached_trial_start_;
        }
        if (!start.has_value()) {
            return 0;
        }
        int64_t left = days_left_from_(*start);
        return left > 0 ? static_cast<int>(left) : 0;
    }

    // ── Async wrappers ────────────────────────────────────────────────────

    std::future<Result<State>> activateAsync(const std::string& key) {
        return std::async(std::launch::async,
                          [this, key]{ return activate(key); });
    }

    std::future<Result<State>> validateAsync() {
        return std::async(std::launch::async,
                          [this]{ return validate(); });
    }

    std::future<Result<void>> deactivateAsync() {
        return std::async(std::launch::async,
                          [this]{ return deactivate(); });
    }

    // ── Background auto-validation ────────────────────────────────────────

    /// Spawn a single background thread that periodically calls
    /// refreshIfNeeded() on the schedule configured by
    /// cfg_.autoValidationIntervalMs.  Never started implicitly — the host
    /// application must call this explicitly.
    ///
    /// Idempotent: calling startAutoValidation() while a thread is already
    /// running is a no-op (the existing thread continues).
    ///
    /// Restartable: stop-then-start is an ordinary pattern (stop polling when
    /// the licence goes invalid, restart when the user activates), including
    /// after a stopAutoValidation() that came from a listener on the worker
    /// thread itself — that path deliberately leaves the thread unjoined, and
    /// a finished thread is still joinable(), so this reaps it rather than
    /// mistaking it for a live worker and no-opping forever.
    void startAutoValidation() {
        std::thread stale;
        {
            std::lock_guard<std::mutex> lock(av_mutex_);
            if (av_thread_.joinable()) {
                if (!av_stop_) return;   // genuinely running — no-op
                // Cannot reap ourselves; the caller is the worker. It exits
                // when this listener returns, and the next start reaps it.
                if (av_thread_.get_id() == std::this_thread::get_id()) return;
                stale = std::move(av_thread_);
            }
        }
        // Join outside the lock — the worker needs av_mutex_ to exit — and
        // BEFORE clearing av_stop_, or the old worker would see the flag drop
        // and keep running.
        if (stale.joinable()) stale.join();

        std::lock_guard<std::mutex> lock(av_mutex_);
        if (av_thread_.joinable()) return;   // another caller won the race

        av_stop_ = false;
        av_thread_ = std::thread([this] {
            auto interval = std::chrono::milliseconds(cfg_.autoValidationIntervalMs);
            std::unique_lock<std::mutex> lk(av_mutex_);
            while (!av_stop_) {
                // Interruptible wait: wakes immediately on stopAutoValidation().
                av_cv_.wait_for(lk, interval, [this]{ return av_stop_; });
                if (av_stop_) break;
                // Release the mutex while calling refreshIfNeeded so it can
                // acquire cache_mutex_ / listeners_mutex_ without deadlock.
                lk.unlock();
                refreshIfNeeded();
                lk.lock();
            }
        });
    }

    /// Signal the background thread to stop and join it.
    /// Idempotent: safe to call when no thread is running.
    /// Returns promptly — the thread wakes up via the condition variable
    /// instead of blocking for the full interval.
    void stopAutoValidation() {
        std::thread to_join;
        {
            std::lock_guard<std::mutex> lock(av_mutex_);
            if (!av_thread_.joinable()) return; // not running — no-op
            av_stop_ = true;
            av_cv_.notify_all();

            // Called from the auto-validation thread itself — i.e. from a
            // state-change listener, which is delivered on whichever thread
            // caused the transition. Joining here would self-join: join()
            // throws EDEADLK, the std::thread is destroyed still-joinable,
            // and ~thread() calls std::terminate() — an abort no caller can
            // catch. Leave the thread owned by av_thread_ instead: it has
            // been signalled, so it exits as soon as this listener returns,
            // and the next stopAutoValidation() or ~Client() from any other
            // thread joins it properly.
            if (av_thread_.get_id() == std::this_thread::get_id()) return;

            to_join = std::move(av_thread_); // move out before unlocking
        }
        // Join outside the lock so the worker can re-acquire av_mutex_ to exit.
        if (to_join.joinable()) to_join.join();
    }

    // ── Launch / refresh API ──────────────────────────────────────────────

    /// Load the cached lease from the store, verify it offline, set state;
    /// then ALWAYS perform a server validate() round-trip — never gated by
    /// the in-session staleness timer. A revoke/expiry on the dashboard must
    /// land on the very next launch, not lag behind the 5min/6h/24h cadence
    /// that refreshIfNeeded() uses for long-running hosts between launches.
    /// If there is no cached lease, state stays as-is (Invalid/initial) and
    /// no network call is made (nothing to revalidate).
    /// A transient/network failure does not mutate state beyond the existing
    /// offline-grace bound (see apply_offline_grace_).
    /// Ported from keylight-rust check_on_launch() and keylight-csharp CheckOnLaunchAsync(),
    /// updated per the cross-SDK revocation/offline-bound parity design (2026-07-08).
    Result<State> checkOnLaunch() {
        // The cache is already primed on construction via refresh_state_from_store_().
        if (has_stored_license_()) {
            // Report what state() reports. Offline, the grace window would
            // otherwise hand back Licensed against a clock that has moved
            // backward — the launch path and the paywall must not disagree.
            return report_(validate_and_reconcile_());
        }
        // No paid license: resolve the persisted local trial offline. This
        // never *starts* a trial — a DAW scanning or instantiating a plugin
        // must not consume the user's trial window; only startTrial() does
        // that, and only when the user asks for it.
        State new_state = resolve_current_state_();
        set_state_(new_state);
        return report_(new_state);
    }

    /// Apply the timer model: refresh debounce 5min, stale 6h, near-expiry 24h.
    /// If a refresh is due, calls validate(); otherwise returns current state.
    /// On a network failure within maxOfflineDays grace window, keeps Licensed.
    /// Ported from keylight-rust refresh_if_needed() and keylight-csharp RefreshIfNeededAsync().
    /// This in-session cadence is unchanged by the always-validate-on-launch
    /// fix: it still governs long-running hosts between launches.
    Result<State> refreshIfNeeded() {
        // Poll the clock guard first. Every return below this line can
        // short-circuit without touching state_, and a clock that moved
        // changes no raw state — so without this, a rollback would reach
        // state() and never reach a single subscriber.
        notify_();

        if (!has_stored_license_()) {
            // No paid license — nothing to revalidate online, but the local
            // trial may have elapsed since the last resolve. keylight-rust and
            // keylight-js recompute check_trial() inside state() on every call;
            // C++ state() reads an atomic (audio-thread contract) and cannot,
            // so the trial is re-resolved here instead. Hosts already call this
            // on focus/resume, and startAutoValidation() ticks it, so a trial
            // that runs out mid-session downgrades on its own and the change
            // reaches subscribers. Still purely local — no network call.
            State new_state = resolve_current_state_();
            set_state_(new_state);
            return report_(new_state);
        }

        int64_t now          = now_fn_();
        int64_t last_lvo     = load_last_validated_online_();
        bool    has_lvo      = (last_lvo > 0);

        // Debounce: skip if validated within the last 5 minutes
        if (has_lvo && (now - last_lvo) < REFRESH_DEBOUNCE) {
            return report_(state_.load());
        }

        // Near-expiry check: refresh if lease expires within 24h
        bool near_expiry = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (cached_lease_.has_value()) {
                near_expiry = (cached_lease_->expiresAt - now) < NEAR_EXPIRY_SECS;
            }
        }

        // Refresh if no prior validated_online, stale (>=6h), or near expiry
        bool do_refresh = !has_lvo
            || (now - last_lvo) >= REFRESH_STALE
            || near_expiry;

        if (!do_refresh) {
            return report_(state_.load());
        }

        return report_(validate_and_reconcile_());
    }

    // ── Events API ────────────────────────────────────────────────────────

    /// Register a callback for state-transition events.
    /// event: currently only "change" is defined (fires on every state transition).
    /// Returns a Subscription RAII handle; when the handle is destroyed or
    /// unsubscribe() is called, the callback is removed.
    /// Callbacks are dispatched on whichever thread happens to be draining the
    /// event queue, which is NOT necessarily the thread that caused the
    /// transition. UI/audio hosts must marshal to their own thread.
    Subscription on(const std::string& /*event*/,
                    std::function<void(State)> cb)
    {
        return subscribe(std::move(cb));
    }

    /// Subscribe to all state transitions. Returns a Subscription RAII handle.
    ///
    /// The callback receives what state() would return, so an event-driven
    /// paywall and a query-driven one cannot disagree.
    ///
    /// LISTENER CONTRACT:
    ///   - No lock is held while your callback runs, so it may call back into
    ///     this Client (validate(), refreshIfNeeded(), …) and may take your
    ///     own locks. A re-entrant call queues its event rather than
    ///     recursing; it may be delivered by a different thread.
    ///   - Unsubscribing from inside your own callback is supported.
    ///   - Do NOT destroy this Client from a callback. ~Client() joins the
    ///     auto-validation thread, and a listener delivered on that thread
    ///     cannot join itself. stopAutoValidation() handles that case (it
    ///     signals and returns); the destructor cannot.
    ///   - Events are delivered in order, but not synchronously: the call
    ///     that caused a transition may return before the event has been
    ///     delivered by whichever thread holds the delivery baton.
    ///
    ///   - A listener MUST NOT throw. An exception cannot be reported from
    ///     here — delivery runs on whatever thread moved the state — so it is
    ///     caught and swallowed, and the remaining listeners still get the
    ///     event.
    ///   - unsubscribe() does not fence a delivery already in flight on
    ///     another thread. Keep whatever your listener captures alive across
    ///     that window (the JUCE adapter uses a shared alive_ flag).
    ///
    /// The callback runs on whichever thread is draining the queue. That is
    /// usually the thread that caused the transition, but under concurrency it
    /// can be another one — a thread already delivering picks up your event
    /// rather than handing it back. Never the audio thread. Marshal to your UI
    /// thread yourself.
    Subscription subscribe(std::function<void(State)> cb) {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        uint64_t id = ++next_listener_id_;
        listeners_.push_back({id, std::move(cb)});
        return Subscription(this, id);
    }

    // ── Query API ─────────────────────────────────────────────────────────

    /// Current state — reads atomics only; audio-thread safe, never blocks.
    ///
    /// A clock rolled back beyond tolerance since the last recorded server
    /// contact invalidates any offline reasoning we could do, so this fails
    /// closed rather than trusting a lease against a moved clock.
    ///
    /// This method is `noexcept` and documented audio-thread safe, and it
    /// calls the caller-supplied `now_fn_`. See the NOW-FUNCTION CONTRACT at
    /// the top of this header: a `now_fn` that throws terminates the process,
    /// and one that locks or allocates breaks the audio-thread guarantee.
    State state() const noexcept {
        if (clock_untrusted_()) return State::Invalid;
        return state_.load();
    }

    /// True iff the cached, verified lease contains the named entitlement.
    ///
    /// Applies the same clock-rollback guard as state(). A feature gate that
    /// kept answering true while state() answered Invalid would fail OPEN —
    /// the paywall and the gate must agree.
    bool hasEntitlement(const std::string& feature) const {
        if (clock_untrusted_()) return false;
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (!cached_lease_.has_value()) return false;
        const auto& l = *cached_lease_;
        // Only count if still trusted + not expired at current clock
        auto vr = verifier_.verify(l, now_fn_());
        if (!vr.is_trusted() || vr.expired || l.status == "expired") return false;
        for (const auto& e : l.entitlements) {
            if (e == feature) return true;
        }
        return false;
    }

    /// Cached license expiry (epoch seconds) from the last activate/validate.
    std::optional<int64_t> cachedLicenseExpiresAt() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cached_expires_at_;
    }

private:
    // ── Clock trust ───────────────────────────────────────────────────────

    // state() promises the audio thread "atomics only, no lock". On a target
    // where these are mutex-backed that promise is silently false, and the
    // failure mode is a priority-inverted audio dropout, not a test failure.
    static_assert(std::atomic<int64_t>::is_always_lock_free,
                  "keylight::Client::state() is documented audio-thread safe; "
                  "std::atomic<int64_t> is not lock-free on this target");
    static_assert(std::atomic<State>::is_always_lock_free,
                  "keylight::Client::state() is documented audio-thread safe; "
                  "std::atomic<State> is not lock-free on this target");

    /// True when the system clock has moved backward, beyond tolerance, since
    /// the last recorded server contact. Every state read point consults this
    /// so they cannot disagree: state(), hasEntitlement() and — through
    /// report_() — every State a public method hands back all fail closed
    /// together.
    ///
    /// Reads the ATOMIC anchor mirror, never the mutex-guarded field, so it
    /// stays usable from noexcept, lock-free, audio-thread-safe state().
    /// An anchor of 0 means "never validated online" — there is nothing to
    /// compare against, and the offline bound in refresh_state_from_store_()
    /// already fails that case closed.
    bool clock_untrusted_() const noexcept {
        const int64_t anchor = last_validated_online_atomic_.load();
        return anchor != 0 && clock_rolled_back(anchor, now_fn_());
    }

    /// Every public entry point hands its State back through here, so no
    /// caller can be told something state() would contradict. Without it the
    /// guard covers only the paywall: refreshIfNeeded() is what long-running
    /// hosts poll between launches, and it would keep reporting Licensed from
    /// the debounce and staleness short-circuits — no server contact, cached
    /// state, moved clock — while state() answered Invalid.
    ///
    /// A successful round-trip re-anchors the clock before returning, so on
    /// the online paths this is a no-op; it bites exactly on the offline and
    /// short-circuit returns, which is where it must.
    ///
    /// Errors pass through untouched: an error is not a state claim, and
    /// rewriting it to Invalid would lose the failure the caller needs.
    Result<State> report_(State s) const {
        if (clock_untrusted_()) return Result<State>::ok(State::Invalid);
        return Result<State>::ok(s);
    }

    Result<State> report_(Result<State> r) const {
        if (r.is_ok() && clock_untrusted_()) {
            return Result<State>::ok(State::Invalid);
        }
        return r;
    }

    // ── Dependencies ──────────────────────────────────────────────────────
    Config                   cfg_;
    Transport&               transport_;
    LicenseStore&            store_;
    std::function<int64_t()> now_fn_;
    std::function<std::optional<std::string>()> hardware_id_fn_;
    Verifier                 verifier_;

    // ── State ─────────────────────────────────────────────────────────────
    std::atomic<State>       state_;
    // Last value handed to subscribers. Distinct from state_ because the
    // clock guard can change what we report without state_ changing at all,
    // and because two raw states can report as the same guarded one.
    std::atomic<State>       last_reported_{State::Invalid};
    // Atomic mirror of cached_last_validated_online_, so the clock guard can
    // run inside noexcept, lock-free state(). The mutex-protected field stays
    // the source of truth for persistence; this is written alongside it.
    std::atomic<int64_t>     last_validated_online_atomic_{0};

    // Mutex-guarded cache of the decoded lease + extras
    mutable std::mutex               cache_mutex_;
    std::optional<Lease>             cached_lease_;
    std::optional<int64_t>           cached_expires_at_;
    std::optional<std::string>       cached_instance_id_;
    std::optional<std::string>       cached_license_key_;
    // Epoch seconds of last successful online validation (0 = never).
    int64_t                          cached_last_validated_online_ = 0;
    // Epoch seconds when the local trial was started (nullopt = never started).
    std::optional<int64_t>           cached_trial_start_;
    std::optional<std::string>       cached_free_tier_instance_id_;
    std::optional<std::string>       cached_hardware_id_;
    std::optional<std::string>       cached_keyless_last_state_;
    int64_t                          cached_last_keyless_ping_at_ = 0;

    // ── Event listeners ───────────────────────────────────────────────────
    struct Listener {
        uint64_t                   id;
        std::function<void(State)> cb;
    };
    mutable std::mutex        listeners_mutex_;
    std::vector<Listener>     listeners_;
    uint64_t                  next_listener_id_ = 0;
    // Guards the event ORDER (pending_ + delivering_ + the dedupe), never the
    // delivery itself — see notify_(). Never nested with listeners_mutex_,
    // cache_mutex_ or av_mutex_ — each is taken and released on its own — and
    // never held across a callback, so it cannot join an application's lock
    // cycle.
    std::mutex                notify_mutex_;
    std::vector<State>        pending_;              // events in delivery order
    bool                      delivering_ = false;   // the delivery baton

    // ── Background auto-validation ────────────────────────────────────────
    // av_mutex_ guards av_stop_ and av_thread_.
    // The worker holds a unique_lock<av_mutex_> for its wait/flag check,
    // then RELEASES it before calling refreshIfNeeded() (which acquires
    // cache_mutex_ / listeners_mutex_) to avoid deadlock.
    std::mutex              av_mutex_;
    std::condition_variable av_cv_;
    bool                    av_stop_  = false;
    std::thread             av_thread_;

    // ── Private helpers ───────────────────────────────────────────────────

    std::string api_url_(const std::string& action) const {
        std::string base = cfg_.apiBaseUrl;
        // Strip trailing slash
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/" + cfg_.tenantId + "/" + cfg_.productId + "/" + action;
    }

    /// Headers for every Keylight API call. The tenant SDK key authenticates
    /// the request; without it the worker answers 401 to activate/validate/
    /// deactivate. Every call site goes through this helper so no endpoint can
    /// be added later that forgets to authenticate.
    std::map<std::string, std::string> json_headers_() const {
        std::map<std::string, std::string> headers{
            {"Content-Type", "application/json"},
        };
        if (!cfg_.sdkKey.empty()) {
            headers["X-Keylight-SDK-Key"] = cfg_.sdkKey;
        }
        headers["X-Keylight-Request-Id"] = detail::random_request_id();
        return headers;
    }

    // The worker's human-readable rejection reason, e.g. "License key not
    // found" or "Activation limit reached". This is the string an integrator's
    // UI shows the customer, so a status line is the fallback, not the default.
    // `message` is accepted alongside `error` because the two are used
    // interchangeably across worker routes.
    static std::string http_error_message_(const std::string& body,
                                           const std::string& action,
                                           int                status)
    {
        const std::string fallback = action + " HTTP " + std::to_string(status);
        if (body.empty()) return fallback;

        auto jr = Json::parse(body);
        if (!jr.is_ok()) return fallback;

        std::string msg = jr.value()["error"].as_string();
        if (msg.empty()) msg = jr.value()["message"].as_string();
        return msg.empty() ? fallback : msg;
    }

    // Tiny JSON string escaping (no control chars expected in these values)
    static std::string json_str(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if      (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else                out += c;
        }
        out += "\"";
        return out;
    }

    // Build JSON object string from key→pre-encoded-value pairs.
    // If include_telemetry is true, appends sdk_version, platform, sdk,
    // app_version and the coarse cpu_cores / memory buckets.
    std::string build_json_(
        std::vector<std::pair<std::string, std::string>> fields,
        bool include_telemetry) const
    {
        if (include_telemetry) {
            fields.push_back({"sdk_version", json_str(KEYLIGHT_SDK_VERSION)});
            fields.push_back({"platform",    json_str(detail::current_platform())});
            // `platform` cannot identify the SDK: this one, Rust and C# all send
            // the same canonical macos/windows/linux tokens, so the server used
            // to label every C++ device "Rust". Identify ourselves explicitly.
            fields.push_back({"sdk",         json_str(KEYLIGHT_SDK_ID)});
            if (!cfg_.appVersion.empty()) {
                fields.push_back({"app_version", json_str(cfg_.appVersion)});
            }
            // Coarse device buckets. Never the raw core count or byte count —
            // see device_info.hpp for the cross-SDK bucket contract. Omitted
            // entirely when the platform cannot report the value.
            const char* cores = detail::cpu_cores_bucket(detail::detect_cpu_cores());
            if (cores[0] != '\0') {
                fields.push_back({"cpu_cores", json_str(cores)});
            }
            const char* mem = detail::memory_bucket(detail::detect_physical_memory_bytes());
            if (mem[0] != '\0') {
                fields.push_back({"memory", json_str(mem)});
            }
            // Phase-3 device dimensions. Both are omitted entirely when the
            // platform cannot report them — never a placeholder. device_class
            // is deliberately absent: the server derives it, and inventing one
            // here would fight that.
            std::string osv = detail::detect_os_version();
            if (!osv.empty()) {
                fields.push_back({"os_version", json_str(osv)});
            }
            const char* arch = detail::current_arch();
            if (arch[0] != '\0') {
                fields.push_back({"arch", json_str(arch)});
            }
        }

        std::string out = "{";
        bool first = true;
        for (const auto& [k, v] : fields) {
            if (!first) out += ",";
            out += json_str(k) + ":" + v;
            first = false;
        }
        out += "}";
        return out;
    }

    // Serialize a Lease to JSON (camelCase keys — wire format).
    static std::string lease_to_json_(const Lease& l) {
        std::string ents = "[";
        for (size_t i = 0; i < l.entitlements.size(); ++i) {
            if (i > 0) ents += ",";
            ents += json_str(l.entitlements[i]);
        }
        ents += "]";

        // clang-format off
        return "{"
            "\"kid\":"            + json_str(l.kid)            + ","
            "\"licenseKeyHash\":" + json_str(l.licenseKeyHash) + ","
            "\"instanceId\":"     + json_str(l.instanceId)     + ","
            "\"issuedAt\":"       + std::to_string(l.issuedAt)  + ","
            "\"expiresAt\":"      + std::to_string(l.expiresAt) + ","
            "\"status\":"         + json_str(l.status)          + ","
            "\"entitlements\":"   + ents                        + ","
            "\"signature\":"      + json_str(l.signature)       +
            "}";
        // clang-format on
    }

    /// Handle a 422 response body from the /validate endpoint — the worker's
    /// status for a definitive rejection (revoke, deactivated instance, or a
    /// genuinely stale license). Real 422 payloads take two shapes:
    ///   - `{"lease": {...}, ...}` — the license itself is stale (lease
    ///     status "expired"/"fallback"); the lease is still signed and must
    ///     be trusted/persisted so state() resolves Expired/Limited off it,
    ///     exactly like a 200 response with a non-"active" lease.
    ///   - `{"error": "..."}` with NO `lease` field at all — a genuine
    ///     revoke / "instance not found or deactivated". There is nothing
    ///     to trust here: the previously-cached "active" lease must be
    ///     cleared so state() can no longer resolve Licensed off stale data
    ///     (leaving it in place is exactly the bug this method fixes).
    /// Returns std::nullopt if the body could not be decoded at all — the
    /// caller then falls back to treating this like a transient failure
    /// (network hiccup / non-JSON body), never mutating stored state.
    /// Mirrors keylight-js Client.validate()'s 422-decodable handling.
    std::optional<State> handle_validate_rejection_(const std::string& body, int64_t now) {
        auto jr = Json::parse(body);
        if (!jr.is_ok()) {
            return std::nullopt; // undecodable — treat as transient
        }
        const Json& j = jr.value();

        auto lease_node = j["lease"];
        if (lease_node.size() > 0) {
            auto lr = Lease::from_json(lease_node);
            if (lr.is_ok()) {
                const Lease& lease = lr.value();
                if (verifier_.verify(lease, now).is_trusted()) {
                    std::string lease_json = lease_to_json_(lease);
                    std::optional<std::string> iid;
                    {
                        std::lock_guard<std::mutex> lock(cache_mutex_);
                        iid = cached_instance_id_;
                    }
                    std::optional<int64_t> expires_at;
                    {
                        int64_t v = j["license_expires_at"].as_int();
                        if (v != 0) expires_at = v;
                    }
                    persist_({lease_json, expires_at, iid});
                    save_last_validated_online_(now);
                }
                State new_state = resolve_with_trial_(resolve_from_lease_(lease));
                set_state_(new_state);
                return new_state;
            }
            // Malformed lease payload inside a definitive-rejection response —
            // cannot be trusted either way; fall through and treat it like the
            // no-lease deny path rather than silently keeping stale state.
        }

        // Definitive rejection with no (usable) lease — a real revoke. This
        // deliberately does NOT fall back to the local trial: the stored
        // license key is still there and a revoked seat must not silently
        // reopen an old trial (keylight-rust resolves the same case off
        // `had_stored_license`, never off the trial).
        clear_lease_();
        set_state_(State::Invalid);
        return State::Invalid;
    }

    /// Clear the stored trusted lease (used when a 422 definitively rejects
    /// with no lease at all — a real revoke / deactivated instance). Keeps
    /// license_key/instance_id/lastValidatedOnline intact — only the lease
    /// itself is removed, so a later refresh_state_from_store_() (e.g. next
    /// process launch) sees no cached lease and resolves Invalid instead of
    /// trusting the last-known "active" data. Mirrors keylight-js's
    /// `del(ACCOUNT.LEASE)` in the no-lease rejection branch of validate().
    void clear_lease_() {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_lease_ = std::nullopt;
        }
        // Rewrite the blob from the cache: every other field (instanceId,
        // licenseKey, lastValidatedOnline, trialStart) is preserved because
        // serialization happens in exactly one place.
        save_cache_();
    }

    // Derive State from an optional (possibly-null) lease using current clock.
    State resolve_from_lease_(const std::optional<Lease>& lease) const {
        if (!lease.has_value()) {
            // No lease in response — stay at current state (caller may override)
            return state_.load();
        }
        const Lease& l = *lease;
        auto vr = verifier_.verify(l, now_fn_());
        if (!vr.is_trusted()) {
            return State::Invalid;
        }
        // Trusted: interpret status
        if (l.status == "active") {
            return vr.expired ? State::Expired : State::Licensed;
        }
        // Rust's resolve_state maps ("fallback", _) -> Limited BEFORE the
        // expired arm. Keeping fallback on Expired locks the app over a
        // server-side signing incident.
        if (l.status == "fallback") return State::Limited;
        // "expired", or anything else from a trusted lease → Expired
        return State::Expired;
    }

    // Reload state from the persistent store (called on construction).
    void refresh_state_from_store_() {
        auto lr = store_.load();
        if (!lr.is_ok() || lr.value().empty()) {
            state_.store(State::Invalid);
            return;
        }
        // Try to decode as our persisted blob: a JSON object with
        // "lease", "expiresAt", "instanceId", … fields.
        auto jr = Json::parse(lr.value());
        if (!jr.is_ok()) {
            // Unreadable blob: nothing is cached, so checkTrial() reports
            // NotStarted. A corrupted store must never look like a fresh
            // install that is entitled to a brand-new trial — only an
            // explicit startTrial() writes a trial start.
            state_.store(State::Invalid);
            return;
        }
        const Json& j = jr.value();

        std::optional<Lease> lease;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            // Decode lease (absent/undecodable → no cached lease; the trial
            // fallback below still applies).
            auto lease_node = j["lease"];
            if (lease_node.size() > 0) {
                auto lease_r = Lease::from_json(lease_node);
                if (lease_r.is_ok()) {
                    cached_lease_ = lease_r.value();
                    lease         = cached_lease_;
                }
            }
            {
                int64_t v = j["expiresAt"].as_int();
                if (v != 0) cached_expires_at_ = v;
            }
            {
                std::string v = j["instanceId"].as_string();
                if (!v.empty()) cached_instance_id_ = v;
            }
            {
                std::string v = j["licenseKey"].as_string();
                if (!v.empty()) cached_license_key_ = v;
            }
            {
                // Load lastValidatedOnline (written by save_last_validated_online_)
                int64_t v = j["lastValidatedOnline"].as_int();
                if (v != 0) {
                    cached_last_validated_online_ = v;
                    last_validated_online_atomic_.store(v);
                }
            }
            {
                // Load the local trial start written by startTrial().
                int64_t v = j["trialStart"].as_int();
                if (v != 0) cached_trial_start_ = v;
            }
            {
                // Anonymous free-tier instance id (see freeTierInstanceId()).
                std::string fid = j["freeTierInstanceId"].as_string();
                if (!fid.empty()) cached_free_tier_instance_id_ = fid;
            }
            {
                std::string hw = j["cachedHardwareId"].as_string();
                if (!hw.empty()) cached_hardware_id_ = hw;
            }
            {
                std::string kls = j["keylessLastState"].as_string();
                if (!kls.empty()) cached_keyless_last_state_ = kls;
                cached_last_keyless_ping_at_ = j["lastKeylessPingAt"].as_int();
            }
        }

        State paid = State::Invalid;
        if (lease.has_value()) {
            auto vr = verifier_.verify(*lease, now_fn_());
            paid    = derive_state_from_verify_(*lease, vr);
        }

        // Bound how long a cached lease may carry the app without server
        // contact. The lease's own 7-day TTL is the ceiling; maxOfflineDays is
        // the tenant's policy underneath it, and it was previously ignored on
        // this path — so a tenant setting 2 still got 7.
        //
        // Fail closed when the anchor is missing: a lease with no record of
        // ever having been validated online cannot be aged, and treating
        // "unknown" as "recent" is exactly the gap an attacker deletes a field
        // to create. Matches keylight-rust.
        //
        // Fail closed too when the anchor sits AHEAD of the clock beyond the
        // rollback tolerance: `now - anchor` is negative there, so the
        // `> max_age` test can never fire and the bound is silently disabled
        // for as long as the anchor stays in the future — push the clock
        // forward across one validate and the tenant's policy stops applying.
        // Within the tolerance the anchor is trusted, for the same reason
        // clock.hpp tolerates a small backward step: NTP corrections and
        // suspend/resume routinely move the clock a little, and locking out a
        // paying customer over a second of drift would be the worse bug.
        if (paid != State::Invalid && cfg_.maxOfflineDays > 0) {
            int64_t anchor;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                anchor = cached_last_validated_online_;
            }
            const int64_t now = now_fn_();
            const int64_t max_age =
                static_cast<int64_t>(cfg_.maxOfflineDays) * 86400;
            if (anchor == 0 || clock_rolled_back(anchor, now) ||
                (now - anchor) > max_age) {
                paid = State::Expired;
            }
        }

        // Paid licensing wins; the persisted local trial only fills the gap.
        state_.store(resolve_with_trial_(paid));
    }

    static State derive_state_from_verify_(const Lease& l, const VerifyResult& vr) {
        if (!vr.is_trusted()) return State::Invalid;
        if (l.status == "active") return vr.expired ? State::Expired : State::Licensed;
        // Mirrors resolve_from_lease_: a cached "fallback" lease must still
        // resolve to Limited after an offline relaunch, not re-lock to
        // Expired just because the store reload took a different path.
        if (l.status == "fallback") return State::Limited;
        return State::Expired;
    }

    // ── Trial helpers ─────────────────────────────────────────────────────

    /// Whole days remaining from a trial start timestamp; <= 0 means elapsed.
    /// Elapsed time is seconds / 86400 (matching keylight-rust check_trial),
    /// clamped at zero so a wall clock that moved backwards cannot extend the
    /// window past its configured length.
    int64_t days_left_from_(int64_t start) const {
        int64_t elapsed_secs = now_fn_() - start;
        if (elapsed_secs < 0) elapsed_secs = 0;
        int64_t days_elapsed = elapsed_secs / 86400;
        return static_cast<int64_t>(cfg_.trialDurationDays) - days_elapsed;
    }

    // ── Device identity helpers ───────────────────────────────────────────

    /// Append the anonymous free-tier id (only if one already exists — never
    /// mint one here) and machine_hash to an outgoing body.  Mirrors
    /// keylight-rust, which attaches both to activate and machine_hash to
    /// validate, so a device converting from free tier to paid is counted once
    /// rather than twice.
    void append_attribution_fields_(
        std::vector<std::pair<std::string, std::string>>& fields,
        bool include_instance_id)
    {
        if (include_instance_id) {
            std::optional<std::string> id;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                id = cached_free_tier_instance_id_;
            }
            if (id.has_value() && !id->empty()) {
                fields.push_back({"free_tier_instance_id", json_str(*id)});
            }
        }
        if (auto hash = machine_hash_()) {
            fields.push_back({"machine_hash", json_str(*hash)});
        }
    }


    /// The true hardware id: read live, written through to the store on
    /// success, falling back to the last cached value when a live read fails.
    /// Keeps machine_hash stable across a transient IOKit/registry failure.
    /// NO random fallback — nullopt means "omit machine_hash entirely".
    std::optional<std::string> cached_hardware_id_value_() {
        std::optional<std::string> live =
            hardware_id_fn_ ? hardware_id_fn_() : std::nullopt;
        if (live.has_value() && !live->empty()) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                if (cached_hardware_id_ != live) {
                    cached_hardware_id_ = live;
                    changed = true;
                }
            }
            if (changed) (void)save_cache_();
            return live;
        }
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cached_hardware_id_.has_value() && !cached_hardware_id_->empty()) {
            return cached_hardware_id_;
        }
        return std::nullopt;
    }

    /// Cross-SDK machine_hash from the cached hardware id, if any.
    std::optional<std::string> machine_hash_() {
        auto hw = cached_hardware_id_value_();
        if (!hw.has_value()) return std::nullopt;
        return detail::machine_hash(cfg_.tenantId, cfg_.productId, *hw);
    }

    /// Apply the local-trial and free-tier fallbacks to a state resolved from
    /// paid licensing.
    /// Priority: valid paid licence → active trial → free tier → elapsed trial
    /// → Invalid.  Only an otherwise-unusable (Invalid) paid state consults the
    /// trial, so paid licensing — including a paid Expired — always wins,
    /// mirroring keylight-rust's resolve_state() (`had_license`
    /// short-circuits the trial).
    /// Must NOT be called while holding cache_mutex_ (checkTrial() locks it).
    State resolve_with_trial_(State paid_state) const {
        if (paid_state != State::Invalid) {
            return paid_state;
        }
        switch (checkTrial()) {
            case TrialStatus::Active:
                return State::Trial;
            case TrialStatus::Expired:
                // Free tier outranks an elapsed trial: keylight-rust's
                // `_ if free_tier_enabled` arm sits AFTER the trial match, so a
                // lapsed trial drops to the free tier rather than the paywall.
                return cfg_.freeTierEnabled ? State::FreeTier : State::Expired;
            case TrialStatus::NotStarted:
                break;
        }
        return cfg_.freeTierEnabled ? State::FreeTier : State::Invalid;
    }

    /// Re-resolve the current state offline. When any paid-licensing material
    /// is cached the license flow owns the state and it is returned as-is;
    /// otherwise the persisted trial (or Invalid) decides. No network I/O.
    State resolve_current_state_() const {
        bool has_paid_material = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            has_paid_material = cached_lease_.has_value()
                || (cached_license_key_.has_value() && !cached_license_key_->empty());
        }
        if (has_paid_material) {
            return state_.load();
        }
        return resolve_with_trial_(State::Invalid);
    }

    // ── Persist helpers ───────────────────────────────────────────────────

    struct PersistData {
        // nullopt means "no lease string to write" (keep as-is)
        std::optional<std::string>       lease_json;
        std::optional<int64_t>           expires_at;
        std::optional<std::string>       instance_id;
        std::optional<std::string>       license_key;
    };

    /// Merge the supplied fields into the in-memory cache, then rewrite the
    /// whole store blob from that cache. Fields the caller did not supply keep
    /// their cached value — a partial update can no longer drop licenseKey,
    /// lastValidatedOnline or trialStart from disk.
    void persist_(const PersistData& d) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            if (d.lease_json.has_value()) {
                // Re-parse so we have the Lease struct
                auto jr = Json::parse(*d.lease_json);
                if (jr.is_ok()) {
                    auto lr = Lease::from_json(jr.value());
                    if (lr.is_ok()) {
                        cached_lease_ = lr.value();
                    }
                }
            }
            if (d.expires_at.has_value()) {
                cached_expires_at_ = *d.expires_at;
            }
            if (d.instance_id.has_value()) {
                cached_instance_id_ = *d.instance_id;
            }
            if (d.license_key.has_value()) {
                cached_license_key_ = *d.license_key;
            }
        }

        save_cache_();
    }

    /// THE serializer for the persisted blob. Every field the client keeps on
    /// disk is written here and nowhere else, so no code path (activate,
    /// validate, lease refresh, lease clearing, revoke, deactivate) can erase
    /// a field it never touched.
    /// Blob format:
    ///   {"lease":{…},"expiresAt":N,"instanceId":"…","licenseKey":"…",
    ///    "lastValidatedOnline":N,"trialStart":N}
    /// Caller must hold cache_mutex_.
    std::string build_blob_locked_() const {
        std::string blob  = "{";
        bool        first = true;
        auto append = [&](const std::string& kv) {
            if (!first) blob += ",";
            blob += kv;
            first = false;
        };

        if (cached_lease_.has_value()) {
            append("\"lease\":" + lease_to_json_(*cached_lease_));
        }
        if (cached_expires_at_.has_value()) {
            append("\"expiresAt\":" + std::to_string(*cached_expires_at_));
        }
        if (cached_instance_id_.has_value()) {
            append("\"instanceId\":" + json_str(*cached_instance_id_));
        }
        if (cached_license_key_.has_value()) {
            append("\"licenseKey\":" + json_str(*cached_license_key_));
        }
        if (cached_last_validated_online_ != 0) {
            append("\"lastValidatedOnline\":" +
                   std::to_string(cached_last_validated_online_));
        }
        if (cached_trial_start_.has_value()) {
            append("\"trialStart\":" + std::to_string(*cached_trial_start_));
        }
        if (cached_free_tier_instance_id_.has_value()) {
            append("\"freeTierInstanceId\":" +
                   json_str(*cached_free_tier_instance_id_));
        }
        if (cached_hardware_id_.has_value()) {
            append("\"cachedHardwareId\":" + json_str(*cached_hardware_id_));
        }
        if (cached_keyless_last_state_.has_value()) {
            append("\"keylessLastState\":" + json_str(*cached_keyless_last_state_));
        }
        if (cached_last_keyless_ping_at_ != 0) {
            append("\"lastKeylessPingAt\":" +
                   std::to_string(cached_last_keyless_ping_at_));
        }

        blob += "}";
        return blob;
    }

    /// Serialize the current cache and hand it to the store.
    Result<void> save_cache_() {
        std::string blob;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            blob = build_blob_locked_();
        }
        return store_.save(blob);
    }

    /// Load the stored instance_id from cache (or empty string if none).
    std::string load_instance_id_() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cached_instance_id_.has_value()) {
            return *cached_instance_id_;
        }
        return "";
    }

    /// Load the stored license key from cache (or empty string if none).
    std::string load_license_key_() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (cached_license_key_.has_value()) {
            return *cached_license_key_;
        }
        return "";
    }

    // ── E2 helpers ────────────────────────────────────────────────────────

    /// True iff there is a stored license (license key in cache).
    bool has_stored_license_() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cached_license_key_.has_value() && !cached_license_key_->empty();
    }

    /// Load the last-validated-online timestamp (epoch seconds, 0 if absent).
    int64_t load_last_validated_online_() const {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        return cached_last_validated_online_;
    }

    /// Persist the last-validated-online timestamp (called after each successful validate).
    void save_last_validated_online_(int64_t t) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_last_validated_online_ = t;
        }
        last_validated_online_atomic_.store(t);
        // Rewrite the blob from the cache (best-effort; failures are non-fatal).
        save_cache_();
    }

    /// Build the JSON body for a validate request.
    /// Not const: machine_hash_() writes the cached hardware id through to the
    /// store on a successful live read.
    std::string build_validate_body_() {
        std::vector<std::pair<std::string, std::string>> fields{
            {"license_key", json_str(load_license_key_())},
            {"instance_id", json_str(load_instance_id_())},
        };
        append_attribution_fields_(fields, /*include_instance_id=*/false);
        return build_json_(std::move(fields), true);
    }

    /// Perform a single live validate() round-trip against the server and
    /// reconcile the result, applying the offline-grace bound on any
    /// transient failure (network error, non-200, or unparseable body).
    /// Shared by refreshIfNeeded() (gated by the debounce/stale/near-expiry
    /// timer) and checkOnLaunch() (called unconditionally — no gating).
    /// Extracted verbatim from the former refreshIfNeeded() body so both
    /// call sites keep identical reconcile/grace semantics.
    Result<State> validate_and_reconcile_() {
        int64_t now      = now_fn_();
        int64_t last_lvo = load_last_validated_online_();

        // Attempt network refresh via validate()
        State before = state_.load();
        auto hr = transport_.request("POST", api_url_("validate"),
                                     json_headers_(),
                                     build_validate_body_());
        if (!hr.is_ok()) {
            // Network failure — apply offline grace
            return apply_offline_grace_(before, now, last_lvo);
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
            // 422 is the worker's definitive-rejection status for /validate
            // (revoke, deactivated instance, expired/fallback lease) — parse
            // the body instead of treating it as transient/offline-graceable.
            // Any other non-200 status (or an undecodable 422 body) falls
            // through to the existing offline-grace handling below.
            if (resp.status == 422) {
                auto rejected = handle_validate_rejection_(resp.body, now);
                if (rejected.has_value()) {
                    return Result<State>::ok(*rejected);
                }
            }
            return apply_offline_grace_(before, now, last_lvo);
        }

        // Parse and apply the validate response
        auto jr = Json::parse(resp.body);
        if (!jr.is_ok()) {
            return apply_offline_grace_(before, now, last_lvo);
        }
        const Json& j = jr.value();

        std::optional<Lease> lease;
        auto lease_node = j["lease"];
        if (lease_node.size() > 0) {
            auto lr = Lease::from_json(lease_node);
            if (lr.is_ok()) {
                lease = lr.value();
            }
        }

        std::optional<int64_t> expires_at;
        {
            int64_t v = j["license_expires_at"].as_int();
            if (v != 0) expires_at = v;
        }

        if (lease.has_value() && verifier_.verify(*lease, now_fn_()).is_trusted()) {
            std::string lease_json = lease_to_json_(*lease);
            std::optional<std::string> iid;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                iid = cached_instance_id_;
            }
            persist_({lease_json, expires_at, iid});
            // Update last_validated_online timestamp
            save_last_validated_online_(now);
        }

        // Paid licensing wins; an unusable result falls back to the local
        // trial instead of dropping a trialling user to Invalid.
        State new_state = resolve_with_trial_(resolve_from_lease_(lease));
        set_state_(new_state);
        return Result<State>::ok(new_state);
    }

    /// Apply offline grace logic when a network call fails.
    /// Grace only keeps Licensed when the cached lease is NOT yet expired (raw
    /// expiresAt, no skew tolerance) AND we are within maxOfflineDays of the
    /// last successful online validation.  If the lease has passed its own
    /// expiry timestamp the offline grace window is irrelevant — an expired
    /// lease must downgrade regardless.
    /// Ported from keylight-rust cached_lease() + state() and C# ResolveState():
    ///   - Rust:  cached_lease() returns None when r.expired; grace is checked
    ///            first, then expiry.  Absent cached_lease → Expired/Invalid.
    ///   - C#:    ResolveState "stale active lease: fall through to Expired"
    ///            — the offline-grace path must not override that.
    Result<State> apply_offline_grace_(State before, int64_t now, int64_t last_lvo) {
        // Check whether the cached lease has passed its own raw expiresAt.
        // Grace cannot rescue a genuinely expired lease.
        bool lease_raw_expired = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (!cached_lease_.has_value()) {
                // No cached lease — nothing to grace; fall through to downgrade.
                lease_raw_expired = true;
            } else {
                lease_raw_expired = (now > cached_lease_->expiresAt);
            }
        }

        if (lease_raw_expired) {
            // Lease is genuinely expired (or absent) — downgrade regardless of grace.
            State current = state_.load();
            if (current == State::Licensed) {
                set_state_(State::Expired);
                return Result<State>::ok(State::Expired);
            }
            return Result<State>::ok(current);
        }

        // Lease is not yet expired.  Only apply grace if maxOfflineDays > 0.
        if (cfg_.maxOfflineDays <= 0) {
            // No grace configured — keep existing state (mirrors C# MaxOfflineDays=0).
            return Result<State>::ok(state_.load());
        }

        // Check if within the offline grace window.
        if (last_lvo > 0) {
            int64_t offline_secs = now - last_lvo;
            int64_t grace_secs   = static_cast<int64_t>(cfg_.maxOfflineDays) * 86400LL;
            if (offline_secs <= grace_secs) {
                // Within grace — lease is valid + not expired → keep Licensed.
                return Result<State>::ok(state_.load());
            }
        }

        // Beyond grace (or never validated online): downgrade.
        // A Licensed state that has run out of grace degrades to Expired.
        State current = state_.load();
        if (current == State::Licensed) {
            set_state_(State::Expired);
            return Result<State>::ok(State::Expired);
        }
        return Result<State>::ok(current);
    }

    /// Set the raw resolved state, then let notify_() decide whether that is
    /// a change worth reporting. state_ stays the raw resolution — it is what
    /// gets persisted reasoning and what the guard is applied *to*.
    ///
    /// Anything that changes state_ AFTER construction must go through here.
    /// The bare state_.store() calls in refresh_state_from_store_() are safe
    /// only because its sole caller is the constructor, where nobody can have
    /// subscribed yet; a second caller would silently swallow a transition.
    void set_state_(State new_state) {
        state_.store(new_state);
        notify_();
    }

    /// Fire listeners when what state() reports has changed since the last
    /// event. This is the SDK's event channel; nothing else calls listeners.
    ///
    /// It reports the GUARDED state for the same reason every other read
    /// point does. A transition resolved while the clock is untrusted would
    /// otherwise deliver, say, Trial to a subscriber while state() answered
    /// Invalid — the paywall driven by subscribe() and the one driven by
    /// state() would disagree, which is the exact split the guard exists to
    /// close.
    ///
    /// Deduping on the REPORTED value, not the raw one, is what gives the
    /// guard an event of its own. A clock rolled back mid-session changes no
    /// raw state, so a raw-value dedupe would never fire and a host that
    /// caches the last event — JUCE's audio-thread snapshot does exactly
    /// that — would sit on a stale Licensed for the rest of the session.
    /// refreshIfNeeded() and validate() both poll this, and
    /// startAutoValidation() ticks refreshIfNeeded(), so both the rollback and
    /// the later correction reach subscribers without any new machinery.
    /// THREAD SAFETY: the ORDER of events is fixed under notify_mutex_; the
    /// DELIVERY of them happens with no lock held.
    ///
    /// Both halves are load-bearing, and each was a shipped bug at some point
    /// in this branch:
    ///
    /// Publishing the dedupe and then delivering outside any lock lets two
    /// notifiers interleave — both pass the dedupe, then deliver in whatever
    /// order the scheduler picks, and because the dedupe is already satisfied
    /// no later poll corrects it. The subscriber is left permanently
    /// contradicting state(), including stale-Licensed on a rolled-back clock,
    /// which is the very fail-open this guard exists to close.
    ///
    /// Holding the lock across the callbacks fixes that and buys a deadlock.
    /// It puts an SDK-internal lock into the APPLICATION's lock-order graph:
    /// a callback that takes the app's own model mutex — the most obvious
    /// thing a state-change handler does — deadlocks against any thread that
    /// holds that mutex across a Client call, and refreshIfNeeded() on
    /// focus/resume is exactly that. No contract can rescue it, because the
    /// rule would have to be "your callback must not touch any lock any
    /// thread holds across any Client call", which nobody can audit.
    ///
    /// So: append to pending_ under the lock, atomically with the dedupe, and
    /// let ONE thread hold the delivery baton and drain the queue with no lock
    /// held. Order is total, no application lock can invert against ours, and
    /// a callback may re-enter the Client freely — it just queues.
    ///
    /// Consequence to know: validate()/refreshIfNeeded() can return before an
    /// event queued concurrently has been delivered by the thread holding the
    /// baton. Delivery is ordered, not synchronous.
    void notify_() {
        {
            std::lock_guard<std::mutex> lock(notify_mutex_);
            const State reported = state();
            if (last_reported_.exchange(reported) == reported) return;
            // Order is decided HERE, atomically with the dedupe. Everything
            // after this point may run in any order on any thread.
            pending_.push_back(reported);
            if (delivering_) return;   // another thread owns the baton
            delivering_ = true;
        }

        // The baton is a plain bool, so unlike a lock_guard it does NOT release
        // on unwind. Anything that throws past this point — a listener, or a
        // bad_alloc from the copies below — would leave delivering_ stuck true
        // and every later notify_() would take the "someone else is draining"
        // exit. The channel would be dead for the life of the Client while
        // state() kept moving: a caching subscriber like JUCE's audio-thread
        // snapshot would hold its last value forever, which on a rolled-back
        // clock is a permanent fail-open.
        struct BatonGuard {
            Client* self;
            ~BatonGuard() {
                std::lock_guard<std::mutex> lock(self->notify_mutex_);
                self->delivering_ = false;
            }
        } baton_guard{this};

        for (;;) {
            State ev;
            {
                std::lock_guard<std::mutex> lock(notify_mutex_);
                if (pending_.empty()) return;   // guard hands the baton back
                ev = pending_.front();
                pending_.erase(pending_.begin());
            }

            // Copy the callbacks out so a listener can unsubscribe from inside
            // its own callback without invalidating the iteration.
            std::vector<std::function<void(State)>> cbs;
            {
                std::lock_guard<std::mutex> lock(listeners_mutex_);
                cbs.reserve(listeners_.size());
                for (const auto& l : listeners_) {
                    cbs.push_back(l.cb);
                }
            }
            for (const auto& cb : cbs) {
                // Contain each listener. One that throws must not cost the
                // others their event, and must not propagate out of an SDK
                // call the integrator made for an unrelated reason.
                try {
                    cb(ev);   // NO lock held here. This is the whole point.
                } catch (...) {
                    // Swallowed deliberately: see the LISTENER CONTRACT on
                    // subscribe(). There is nowhere to report it — notify_()
                    // runs on whatever thread moved the state.
                }
            }
        }
    }

    /// Remove listener with the given id (called from Subscription::unsubscribe).
    void remove_listener_(uint64_t id) {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listeners_.erase(
            std::remove_if(listeners_.begin(), listeners_.end(),
                           [id](const Listener& l){ return l.id == id; }),
            listeners_.end());
    }

    // Allow Subscription to call remove_listener_
    friend class Subscription;
};

// ---------------------------------------------------------------------------
// Subscription::unsubscribe — defined after Client is complete
// ---------------------------------------------------------------------------
inline void Subscription::unsubscribe() {
    if (client_) {
        client_->remove_listener_(id_);
        client_ = nullptr;
    }
}

} // namespace keylight

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/keyset.hpp
// ──────────────────────────────────────────────────────────────────────────

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

// ──────────────────────────────────────────────────────────────────────────
// include/keylight/keylight.hpp
// ──────────────────────────────────────────────────────────────────────────

// #include "transport/httplib.hpp" // opt-in: KEYLIGHT_BUILD_HTTPLIB_TRANSPORT
