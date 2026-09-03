#pragma once
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

#include "sha256.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <string>

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
