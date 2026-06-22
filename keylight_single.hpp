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
#include <string>
#include <thread>
#include <vector>


// ──────────────────────────────────────────────────────────────────────────
// include/keylight/version.hpp
// ──────────────────────────────────────────────────────────────────────────

#define KEYLIGHT_SDK_VERSION "0.1.0"

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
// include/keylight/config.hpp
// ──────────────────────────────────────────────────────────────────────────


namespace keylight {

struct Config {
    std::string tenantId;
    std::string productId;
    std::string sdkKey;

    // Map of keyId → base64-encoded Ed25519 public key (32 bytes)
    std::map<std::string, std::string> trustedKeys;

    int         maxOfflineDays     = 7;
    std::string keyPrefix;
    int         trialDurationDays  = 0;
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
// include/keylight/json.hpp
// ──────────────────────────────────────────────────────────────────────────

// keylight/json.hpp — minimal header-only recursive-descent JSON reader
// Namespace keylight; internals in anonymous namespace.
// No external dependencies. Exception-free: errors propagate via Result<Json>.



namespace keylight {

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
class Json;

// ---------------------------------------------------------------------------
// Internal implementation — anonymous namespace
// ---------------------------------------------------------------------------
namespace {

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

    bool eof() const { return p >= end; }

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    bool peek(char c) const { return !eof() && *p == c; }

    bool consume(char c) {
        if (!eof() && *p == c) { ++p; return true; }
        return false;
    }

    // Decode one \uXXXX code unit to UTF-8
    bool hex4(uint32_t& out) {
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

    void encode_utf8(uint32_t cp, std::string& out) {
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
    bool parse_string(std::string& out) {
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
    bool parse_number(std::shared_ptr<JValue>& out) {
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

    bool parse_value(std::shared_ptr<JValue>& out) {
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// keylight::Json — public API
// ---------------------------------------------------------------------------

class Json {
public:
    // Default-construct: null Json (used for missing keys)
    Json() : val_(std::make_shared<JValue>()) {}

    // Parse JSON text; returns Result<Json> (never throws)
    static Result<Json> parse(const std::string& src) {
        if (src.empty()) {
            return Result<Json>::err({ErrorCode::BadResponse, "empty JSON input"});
        }
        Parser parser(src);
        std::shared_ptr<JValue> v;
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
        if (val_->type == JType::Object) {
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
        if (val_->type == JType::Array && i < val_->arr.size()) {
            Json j;
            j.val_ = val_->arr[i];
            return j;
        }
        return Json{}; // null
    }

    bool is_array() const { return val_->type == JType::Array; }

    // Array: element count; Object: member count; others: 0
    size_t size() const {
        if (val_->type == JType::Array)  return val_->arr.size();
        if (val_->type == JType::Object) return val_->obj_map.size();
        return 0;
    }

    std::string as_string() const {
        if (val_->type == JType::String) return val_->s;
        return {};
    }

    int64_t as_int() const {
        if (val_->type == JType::Int)    return val_->i;
        if (val_->type == JType::Double) return static_cast<int64_t>(val_->d);
        return 0;
    }

    bool as_bool() const {
        if (val_->type == JType::Bool) return val_->b;
        return false;
    }

    // Convenience: iterate an array of strings
    std::vector<std::string> as_string_array() const {
        std::vector<std::string> result;
        if (val_->type == JType::Array) {
            for (const auto& elem : val_->arr) {
                if (elem->type == JType::String) {
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
        if (val_->type == JType::Object) {
            return val_->obj_keys;
        }
        return {};
    }

private:
    std::shared_ptr<JValue> val_;
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
// Thread-safety: state() reads a std::atomic<State> — audio-thread safe.
//                hasEntitlement / cachedLicenseExpiresAt / listener list are
//                guarded by a mutex.



namespace keylight {

// ---------------------------------------------------------------------------
// State — high-level license state (C++ subset of Rust/C# states)
// ---------------------------------------------------------------------------
enum class State {
    Licensed,   // trusted, unexpired active lease
    Trial,      // no license; within trial window
    Expired,    // trusted lease expired, or license status "expired"/"fallback"
    Invalid,    // no trusted lease, no active trial
};

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
                 []{ return static_cast<int64_t>(std::time(nullptr)); })
    {}

    // Testable constructor — inject a deterministic clock.
    // now_fn() must return Unix epoch seconds as int64_t.
    Client(Config                   cfg,
           Transport&               transport,
           LicenseStore&            store,
           std::function<int64_t()> now_fn)
        : cfg_(std::move(cfg))
        , transport_(transport)
        , store_(store)
        , now_fn_(std::move(now_fn))
        , verifier_(cfg_.trustedKeys)
        , state_(State::Invalid)
    {
        // Prime state from persisted store (if any) on construction.
        refresh_state_from_store_();
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
        std::string body = build_json_({
            {"license_key",   json_str(key)},
            {"instance_name", json_str("device")},
        }, true /*include telemetry*/);

        std::string url = api_url_("activate");
        auto hr = transport_.request("POST", url, json_headers_(), body);
        if (!hr.is_ok()) {
            return Result<State>::err(hr.error());
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
            return Result<State>::err({ErrorCode::Http,
                "activate HTTP " + std::to_string(resp.status)});
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
            return Result<State>::ok(state_.load());
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

        set_state_(new_state);
        return Result<State>::ok(new_state);
    }

    /// Validate the stored license online.  Returns the resulting State.
    Result<State> validate() {
        // Need license_key and instance_id from cache (Worker requires both)
        std::string license_key  = load_license_key_();
        std::string instance_id  = load_instance_id_();

        std::string body = build_json_({
            {"license_key", json_str(license_key)},
            {"instance_id", json_str(instance_id)},
        }, true /*include telemetry*/);

        std::string url = api_url_("validate");
        auto hr = transport_.request("POST", url, json_headers_(), body);
        if (!hr.is_ok()) {
            // Network failure: keep existing state
            return Result<State>::ok(state_.load());
        }
        const auto& resp = hr.value();
        if (resp.status != 200) {
            return Result<State>::ok(state_.load());
        }

        auto jr = Json::parse(resp.body);
        if (!jr.is_ok()) {
            return Result<State>::ok(state_.load());
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

        State new_state = resolve_from_lease_(lease);
        set_state_(new_state);
        return Result<State>::ok(new_state);
    }

    /// Deactivate this device.  Clears the store regardless of network outcome.
    Result<void> deactivate() {
        std::string instance_id = load_instance_id_();

        if (!instance_id.empty()) {
            std::string body = build_json_({
                {"instance_id", json_str(instance_id)},
            }, false);
            std::string url = api_url_("deactivate");
            // Best-effort: ignore network errors (mirror Rust/C# behaviour)
            transport_.request("POST", url, json_headers_(), body);
        }

        // Clear store
        auto cr = store_.clear();
        if (!cr.is_ok()) {
            return cr;
        }

        // Clear in-memory cache
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cached_lease_                  = std::nullopt;
            cached_expires_at_             = std::nullopt;
            cached_instance_id_            = std::nullopt;
            cached_license_key_            = std::nullopt;
            cached_last_validated_online_  = 0;
        }
        set_state_(State::Invalid);
        return Result<void>::ok();
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
    void startAutoValidation() {
        std::lock_guard<std::mutex> lock(av_mutex_);
        if (av_thread_.joinable()) return; // already running — no-op

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
            to_join = std::move(av_thread_); // move out before unlocking
        }
        // Join outside the lock so the worker can re-acquire av_mutex_ to exit.
        if (to_join.joinable()) to_join.join();
    }

    // ── Launch / refresh API ──────────────────────────────────────────────

    /// Load the cached lease from the store, verify it offline, set state;
    /// then call refreshIfNeeded() (which may hit the network if stale/near-expiry).
    /// If there is no cached lease, state stays as-is (Invalid/initial).
    /// Ported from keylight-rust check_on_launch() and keylight-csharp CheckOnLaunchAsync().
    Result<State> checkOnLaunch() {
        // The cache is already primed on construction via refresh_state_from_store_().
        // Call refreshIfNeeded to make a network call if the cached data is stale.
        if (has_stored_license_()) {
            auto r = refreshIfNeeded();
            if (!r.is_ok()) {
                // If refreshIfNeeded fails hard (non-network), propagate.
                // Network failures are handled inside refreshIfNeeded (grace).
                return r;
            }
        }
        return Result<State>::ok(state_.load());
    }

    /// Apply the timer model: refresh debounce 5min, stale 6h, near-expiry 24h.
    /// If a refresh is due, calls validate(); otherwise returns current state.
    /// On a network failure within maxOfflineDays grace window, keeps Licensed.
    /// Ported from keylight-rust refresh_if_needed() and keylight-csharp RefreshIfNeededAsync().
    Result<State> refreshIfNeeded() {
        if (!has_stored_license_()) {
            return Result<State>::ok(state_.load());
        }

        int64_t now          = now_fn_();
        int64_t last_lvo     = load_last_validated_online_();
        bool    has_lvo      = (last_lvo > 0);

        // Debounce: skip if validated within the last 5 minutes
        if (has_lvo && (now - last_lvo) < REFRESH_DEBOUNCE) {
            return Result<State>::ok(state_.load());
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
            return Result<State>::ok(state_.load());
        }

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
            // HTTP error — treat like network failure for grace purposes
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

        State new_state = resolve_from_lease_(lease);
        set_state_(new_state);
        return Result<State>::ok(new_state);
    }

    // ── Events API ────────────────────────────────────────────────────────

    /// Register a callback for state-transition events.
    /// event: currently only "change" is defined (fires on every state transition).
    /// Returns a Subscription RAII handle; when the handle is destroyed or
    /// unsubscribe() is called, the callback is removed.
    /// Callbacks are dispatched on the calling thread; UI/audio hosts must
    /// marshal to their own thread if required.
    Subscription on(const std::string& /*event*/,
                    std::function<void(State)> cb)
    {
        return subscribe(std::move(cb));
    }

    /// Subscribe to all state transitions. Returns a Subscription RAII handle.
    Subscription subscribe(std::function<void(State)> cb) {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        uint64_t id = ++next_listener_id_;
        listeners_.push_back({id, std::move(cb)});
        return Subscription(this, id);
    }

    // ── Query API ─────────────────────────────────────────────────────────

    /// Current state — reads atomic; audio-thread safe.
    State state() const noexcept {
        return state_.load();
    }

    /// True iff the cached, verified lease contains the named entitlement.
    bool hasEntitlement(const std::string& feature) const {
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
    // ── Dependencies ──────────────────────────────────────────────────────
    Config                   cfg_;
    Transport&               transport_;
    LicenseStore&            store_;
    std::function<int64_t()> now_fn_;
    Verifier                 verifier_;

    // ── State ─────────────────────────────────────────────────────────────
    std::atomic<State>       state_;

    // Mutex-guarded cache of the decoded lease + extras
    mutable std::mutex               cache_mutex_;
    std::optional<Lease>             cached_lease_;
    std::optional<int64_t>           cached_expires_at_;
    std::optional<std::string>       cached_instance_id_;
    std::optional<std::string>       cached_license_key_;
    // Epoch seconds of last successful online validation (0 = never).
    int64_t                          cached_last_validated_online_ = 0;

    // ── Event listeners ───────────────────────────────────────────────────
    struct Listener {
        uint64_t                   id;
        std::function<void(State)> cb;
    };
    mutable std::mutex        listeners_mutex_;
    std::vector<Listener>     listeners_;
    uint64_t                  next_listener_id_ = 0;

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

    static std::map<std::string, std::string> json_headers_() {
        return {
            {"Content-Type", "application/json"},
        };
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
    // If include_telemetry is true, appends sdk_version and platform.
    std::string build_json_(
        std::vector<std::pair<std::string, std::string>> fields,
        bool include_telemetry) const
    {
        if (include_telemetry) {
            fields.push_back({"sdk_version", json_str(KEYLIGHT_SDK_VERSION)});
            fields.push_back({"platform",    json_str(detail::current_platform())});
            if (!cfg_.appVersion.empty()) {
                fields.push_back({"app_version", json_str(cfg_.appVersion)});
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
        // "expired", "fallback", or anything else from a trusted lease → Expired
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
        // "lease", "expiresAt", "instanceId" fields.
        auto jr = Json::parse(lr.value());
        if (!jr.is_ok()) {
            state_.store(State::Invalid);
            return;
        }
        const Json& j = jr.value();

        // Decode lease
        auto lease_node = j["lease"];
        if (lease_node.size() == 0) {
            state_.store(State::Invalid);
            return;
        }
        auto lease_r = Lease::from_json(lease_node);
        if (!lease_r.is_ok()) {
            state_.store(State::Invalid);
            return;
        }

        std::lock_guard<std::mutex> lock(cache_mutex_);
        cached_lease_ = lease_r.value();

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
            if (v != 0) cached_last_validated_online_ = v;
        }

        // Don't hold mutex while computing state
        Lease lease_copy = *cached_lease_;
        // release lock before verify (not needed but cleaner)
        // Actually we already hold it — that's fine, verifier doesn't touch mutex
        auto vr = verifier_.verify(lease_copy, now_fn_());
        State s = derive_state_from_verify_(lease_copy, vr);
        state_.store(s);
    }

    static State derive_state_from_verify_(const Lease& l, const VerifyResult& vr) {
        if (!vr.is_trusted()) return State::Invalid;
        if (l.status == "active") return vr.expired ? State::Expired : State::Licensed;
        return State::Expired;
    }

    // ── Persist helpers ───────────────────────────────────────────────────

    struct PersistData {
        // nullopt means "no lease string to write" (keep as-is)
        std::optional<std::string>       lease_json;
        std::optional<int64_t>           expires_at;
        std::optional<std::string>       instance_id;
        std::optional<std::string>       license_key;
    };

    /// Write a blob to the store in the format we read back.
    /// Blob format: {"lease":{...},"expiresAt":N,"instanceId":"..."}
    void persist_(const PersistData& d) {
        // Build the storage blob
        std::string blob = "{";

        if (d.lease_json.has_value()) {
            blob += "\"lease\":" + *d.lease_json;
        }

        if (d.expires_at.has_value()) {
            if (blob.size() > 1) blob += ",";
            blob += "\"expiresAt\":" + std::to_string(*d.expires_at);
        }

        if (d.instance_id.has_value()) {
            if (blob.size() > 1) blob += ",";
            blob += "\"instanceId\":" + json_str(*d.instance_id);
        }

        if (d.license_key.has_value()) {
            if (blob.size() > 1) blob += ",";
            blob += "\"licenseKey\":" + json_str(*d.license_key);
        }

        blob += "}";

        store_.save(blob);

        // Update in-memory cache
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
        // Also write into the store blob. We reload the existing blob and patch it.
        // This is a best-effort update; failures are non-fatal.
        auto lr = store_.load();
        if (!lr.is_ok() || lr.value().empty()) return;
        // Append/overwrite the lastValidatedOnline field by rebuilding the blob.
        // Simple approach: strip trailing '}' and append the key.
        std::string blob = lr.value();
        if (!blob.empty() && blob.back() == '}') {
            blob.pop_back();
            blob += ",\"lastValidatedOnline\":" + std::to_string(t) + "}";
            store_.save(blob);
        }
    }

    /// Build the JSON body for a validate request.
    std::string build_validate_body_() const {
        return build_json_({
            {"license_key", json_str(load_license_key_())},
            {"instance_id", json_str(load_instance_id_())},
        }, true);
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

    /// Set state_ and fire event listeners if the state changed.
    void set_state_(State new_state) {
        State old_state = state_.exchange(new_state);
        if (old_state == new_state) return; // no transition — no event

        // Collect callbacks under the lock, fire outside it to avoid re-entrancy.
        std::vector<std::function<void(State)>> cbs;
        {
            std::lock_guard<std::mutex> lock(listeners_mutex_);
            cbs.reserve(listeners_.size());
            for (const auto& l : listeners_) {
                cbs.push_back(l.cb);
            }
        }
        for (const auto& cb : cbs) {
            cb(new_state);
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
