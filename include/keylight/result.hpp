#pragma once
#include <cassert>
#include <cstdint>
#include <string>

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
