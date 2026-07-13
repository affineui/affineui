#pragma once

// Minimal, strict JSON reader for the affinetools protocol server
// (AFFINETOOLS_DESIGN.md §3.1: commands are parsed on the SERVER thread
// into typed values; the app thread never sees JSON). Internal — not part
// of the public API.
//
// Hard-to-crash rules (the length prefix and payload are untrusted input
// arriving inside the probed process):
//   • fail closed: any malformed byte → parse() returns false;
//   • bounded recursion (max_depth) — deep nesting can't blow the stack;
//   • no exceptions, no asserts on input.
//
// Allocations are fine here — this runs on the server thread only.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace affineui::detail::json {

struct Value {
    enum class Kind : std::uint8_t {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    Kind        kind{Kind::Null};
    bool        boolean{false};
    double      number{0.0};
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    Value();
    ~Value();
    Value(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(const Value&);
    Value& operator=(Value&&) noexcept;

    [[nodiscard]] bool is_object() const noexcept { return kind == Kind::Object; }
    [[nodiscard]] bool is_string() const noexcept { return kind == Kind::String; }
    [[nodiscard]] bool is_number() const noexcept { return kind == Kind::Number; }

    /// Object member lookup; nullptr when absent or not an object.
    [[nodiscard]] const Value* get(std::string_view key) const noexcept;

    /// Convenience: object member as string ("" when absent/wrong type).
    [[nodiscard]] std::string_view get_string(
        std::string_view key) const noexcept;

    /// Convenience: object member as number (`fallback` when absent/wrong type).
    [[nodiscard]] double get_number(std::string_view key,
                                    double fallback = 0.0) const noexcept;
};

inline Value::Value() = default;
inline Value::~Value() = default;
inline Value::Value(const Value&) = default;
inline Value::Value(Value&&) noexcept = default;
inline Value& Value::operator=(const Value&) = default;
inline Value& Value::operator=(Value&&) noexcept = default;

inline const Value* Value::get(std::string_view key) const noexcept {
    if (kind != Kind::Object) return nullptr;
    for (const auto& [member_key, member_value] : object) {
        if (member_key == key) return &member_value;
    }
    return nullptr;
}

inline std::string_view Value::get_string(
    std::string_view key) const noexcept {
    const Value* value = get(key);
    return (value != nullptr && value->kind == Kind::String)
               ? std::string_view{value->string}
               : std::string_view{};
}

inline double Value::get_number(std::string_view key,
                                double fallback) const noexcept {
    const Value* value = get(key);
    return (value != nullptr && value->kind == Kind::Number)
               ? value->number
               : fallback;
}

namespace detail_impl {

struct Parser {
    const char* p;
    const char* end;
    int         depth_left;

    void skip_ws() noexcept {
        while (p < end &&
               (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }

    bool consume(char c) noexcept {
        if (p < end && *p == c) {
            ++p;
            return true;
        }
        return false;
    }

    bool literal(const char* lit) noexcept {
        const char* q = p;
        while (*lit != '\0') {
            if (q >= end || *q != *lit) return false;
            ++q;
            ++lit;
        }
        p = q;
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    bool hex4(std::uint32_t& out) noexcept {
        if (end - p < 4) return false;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = p[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        p += 4;
        out = v;
        return true;
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (p < end) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') {
                ++p;
                return true;
            }
            if (c == '\\') {
                ++p;
                if (p >= end) return false;
                switch (*p) {
                    case '"':  out.push_back('"');  ++p; break;
                    case '\\': out.push_back('\\'); ++p; break;
                    case '/':  out.push_back('/');  ++p; break;
                    case 'b':  out.push_back('\b'); ++p; break;
                    case 'f':  out.push_back('\f'); ++p; break;
                    case 'n':  out.push_back('\n'); ++p; break;
                    case 'r':  out.push_back('\r'); ++p; break;
                    case 't':  out.push_back('\t'); ++p; break;
                    case 'u': {
                        ++p;
                        std::uint32_t cp = 0;
                        if (!hex4(cp)) return false;
                        // Surrogate pair → single code point.
                        if (cp >= 0xD800u && cp <= 0xDBFFu) {
                            if (end - p < 6 || p[0] != '\\' || p[1] != 'u') {
                                return false;
                            }
                            p += 2;
                            std::uint32_t lo = 0;
                            if (!hex4(lo) || lo < 0xDC00u || lo > 0xDFFFu) {
                                return false;
                            }
                            cp = 0x10000u + ((cp - 0xD800u) << 10) +
                                 (lo - 0xDC00u);
                        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                            return false;  // lone low surrogate
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return false;
                }
                continue;
            }
            if (c < 0x20u) return false;  // raw control char
            out.push_back(static_cast<char>(c));
            ++p;
        }
        return false;  // unterminated
    }

    bool parse_number(double& out) noexcept {
        const char* start = p;
        if (consume('-')) {}
        if (p >= end) return false;
        if (*p == '0') {
            ++p;
        } else if (*p >= '1' && *p <= '9') {
            while (p < end && *p >= '0' && *p <= '9') ++p;
        } else {
            return false;
        }
        if (p < end && *p == '.') {
            ++p;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        // strtod on a bounded copy (the token is short by construction).
        char buf[64];
        const std::size_t len = static_cast<std::size_t>(p - start);
        if (len == 0 || len >= sizeof(buf)) return false;
        for (std::size_t i = 0; i < len; ++i) buf[i] = start[i];
        buf[len] = '\0';
        out = std::strtod(buf, nullptr);
        return true;
    }

    bool parse_value(Value& out) {
        if (depth_left <= 0) return false;
        skip_ws();
        if (p >= end) return false;
        switch (*p) {
            case '{': {
                ++p;
                --depth_left;
                out.kind = Value::Kind::Object;
                skip_ws();
                if (consume('}')) { ++depth_left; return true; }
                while (true) {
                    skip_ws();
                    std::string key;
                    if (!parse_string(key)) return false;
                    skip_ws();
                    if (!consume(':')) return false;
                    Value member;
                    if (!parse_value(member)) return false;
                    out.object.emplace_back(std::move(key), std::move(member));
                    skip_ws();
                    if (consume(',')) continue;
                    if (consume('}')) { ++depth_left; return true; }
                    return false;
                }
            }
            case '[': {
                ++p;
                --depth_left;
                out.kind = Value::Kind::Array;
                skip_ws();
                if (consume(']')) { ++depth_left; return true; }
                while (true) {
                    Value item;
                    if (!parse_value(item)) return false;
                    out.array.push_back(std::move(item));
                    skip_ws();
                    if (consume(',')) continue;
                    if (consume(']')) { ++depth_left; return true; }
                    return false;
                }
            }
            case '"':
                out.kind = Value::Kind::String;
                return parse_string(out.string);
            case 't':
                out.kind = Value::Kind::Boolean;
                out.boolean = true;
                return literal("true");
            case 'f':
                out.kind = Value::Kind::Boolean;
                out.boolean = false;
                return literal("false");
            case 'n':
                out.kind = Value::Kind::Null;
                return literal("null");
            default:
                out.kind = Value::Kind::Number;
                return parse_number(out.number);
        }
    }
};

}  // namespace detail_impl

/// Parse a complete JSON document. Trailing non-whitespace → false.
[[nodiscard]] inline bool parse(std::string_view text, Value& out,
                                int max_depth = 32) {
    detail_impl::Parser parser{text.data(), text.data() + text.size(),
                               max_depth};
    if (!parser.parse_value(out)) return false;
    parser.skip_ws();
    return parser.p == parser.end;
}

}  // namespace affineui::detail::json
