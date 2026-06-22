#pragma once
// keylight/json.hpp — minimal header-only recursive-descent JSON reader
// Namespace keylight; internals in anonymous namespace.
// No external dependencies. Exception-free: errors propagate via Result<Json>.

#include "result.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
