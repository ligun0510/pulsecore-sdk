// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PulseCore Game Integration — minimal JSON value + parser/serializer.
//
// Deliberately dependency-free and small: the Bridge protocol (v1) and effects.json only need flat
// objects, arrays, strings, numbers, bools and null. A bounded recursive-descent parser is used so a
// malformed or hostile message errors out (never crashes / never recurses without limit). Part of the
// OPEN protocol/SDK layer — no PulseCore-proprietary dependencies.
#pragma once

#include <charconv>   // from_chars: locale-independent number parsing (see ParseNumber)
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::json {

enum class Type { Null, Bool, Number, String, Array, Object };

// Ordered object (insertion order preserved so serialized messages are stable/testable).
class Value {
public:
    Value() = default;
    explicit Value(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Value(double n) : type_(Type::Number), num_(n) {}
    explicit Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Value MakeArray() { Value v; v.type_ = Type::Array; return v; }
    static Value MakeObject() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const { return type_ == Type::Number ? num_ : fallback; }
    const std::string& as_string() const { return str_; }
    std::string as_string_or(const std::string& fallback) const {
        return type_ == Type::String ? str_ : fallback;
    }

    const std::vector<Value>& items() const { return arr_; }
    const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }

    void push_back(Value v) { arr_.push_back(std::move(v)); }

    // Set (or replace) an object member, preserving order for a fresh key.
    void set(std::string key, Value v) {
        for (auto& kv : obj_) {
            if (kv.first == key) { kv.second = std::move(v); return; }
        }
        obj_.emplace_back(std::move(key), std::move(v));
    }

    // Returns nullptr if this is not an object or the key is absent.
    const Value* find(std::string_view key) const {
        if (type_ != Type::Object) return nullptr;
        for (const auto& kv : obj_) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    std::string get_string(std::string_view key, const std::string& fallback = {}) const {
        const Value* v = find(key);
        return (v && v->is_string()) ? v->str_ : fallback;
    }
    double get_number(std::string_view key, double fallback = 0.0) const {
        const Value* v = find(key);
        return (v && v->is_number()) ? v->num_ : fallback;
    }
    bool get_bool(std::string_view key, bool fallback = false) const {
        const Value* v = find(key);
        return (v && v->is_bool()) ? v->bool_ : fallback;
    }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

namespace detail {

inline void AppendEscaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0xF]);
                    out.push_back(hex[static_cast<unsigned char>(c) & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

inline void DumpTo(std::string& out, const Value& v) {
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.as_bool() ? "true" : "false"; break;
        case Type::Number: {
            const double n = v.as_number();
            const long long asInt = static_cast<long long>(n);
            if (static_cast<double>(asInt) == n) {
                out += std::to_string(asInt);   // emit integers without a trailing ".000000"
            } else {
                // to_chars, not snprintf: snprintf honours LC_NUMERIC, so any host that had called
                // setlocale() with a comma-decimal locale emitted "0,5" -- invalid JSON that the parser
                // (already locale-independent, from_chars) rejects.
                char buf[32];
                const auto res = std::to_chars(buf, buf + sizeof(buf), n, std::chars_format::general, 6);
                out.append(buf, res.ec == std::errc() ? res.ptr : buf);
            }
            break;
        }
        case Type::String: AppendEscaped(out, v.as_string()); break;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& item : v.items()) {
                if (!first) out.push_back(',');
                first = false;
                DumpTo(out, item);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& kv : v.members()) {
                if (!first) out.push_back(',');
                first = false;
                AppendEscaped(out, kv.first);
                out.push_back(':');
                DumpTo(out, kv.second);
            }
            out.push_back('}');
            break;
        }
    }
}

class Parser {
public:
    Parser(std::string_view text, std::string& error) : text_(text), error_(error) {}

    bool Parse(Value& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        if (pos_ != text_.size()) return Fail("trailing characters after JSON value");
        return true;
    }

private:
    static constexpr int kMaxDepth = 32;

    bool Fail(const char* msg) { if (error_.empty()) error_ = msg; return false; }
    void SkipWs() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }
    char Peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    bool ParseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        SkipWs();
        if (pos_ >= text_.size()) return Fail("unexpected end of input");
        const char c = text_[pos_];
        switch (c) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': { std::string s; if (!ParseString(s)) return false; out = Value(std::move(s)); return true; }
            case 't': case 'f': return ParseBool(out);
            case 'n': return ParseNull(out);
            default:
                if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
                return Fail("unexpected character");
        }
    }

    bool ParseObject(Value& out, int depth) {
        out = Value::MakeObject();
        ++pos_;   // consume '{'
        SkipWs();
        if (Peek() == '}') { ++pos_; return true; }
        for (;;) {
            SkipWs();
            if (Peek() != '"') return Fail("expected object key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (Peek() != ':') return Fail("expected ':' after key");
            ++pos_;
            Value val;
            if (!ParseValue(val, depth + 1)) return false;
            out.set(std::move(key), std::move(val));
            SkipWs();
            const char c = Peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; return true; }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value& out, int depth) {
        out = Value::MakeArray();
        ++pos_;   // consume '['
        SkipWs();
        if (Peek() == ']') { ++pos_; return true; }
        for (;;) {
            Value val;
            if (!ParseValue(val, depth + 1)) return false;
            out.push_back(std::move(val));
            SkipWs();
            const char c = Peek();
            if (c == ',') { ++pos_; continue; }
            if (c == ']') { ++pos_; return true; }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseString(std::string& out) {
        ++pos_;   // consume opening quote
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= text_.size()) return Fail("unterminated escape");
                const char e = text_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (!ParseUnicodeEscape(out)) return false;
                        break;
                    }
                    default: return Fail("invalid escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return Fail("unterminated string");
    }

    bool ParseUnicodeEscape(std::string& out) {
        if (pos_ + 4 > text_.size()) return Fail("truncated \\u escape");
        unsigned int cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
            else return Fail("invalid \\u hex digit");
        }
        // A surrogate half is not a character. Encoding one as three bytes produces CESU-8, which is
        // not valid UTF-8 -- so `"😀"` (an emoji, written the way JSON requires) came out as
        // six bytes no UTF-8 reader accepts. Nothing downstream can currently be made to misbehave with
        // it, but "not currently reachable" is a property of today's consumers, not of the parser: the
        // next thing to read one of these strings is a log file, the state JSON, or the C# UI.
        //
        // Pairs are joined properly; a lone half is rejected rather than mangled.
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: a matching low half must follow, spelled as its own \u escape.
            if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                return Fail("lone high surrogate in \\u escape");
            }
            pos_ += 2;
            if (pos_ + 4 > text_.size()) return Fail("truncated low surrogate");
            unsigned int low = 0;
            for (int i = 0; i < 4; ++i) {
                const char h = text_[pos_++];
                low <<= 4;
                if (h >= '0' && h <= '9') low |= static_cast<unsigned int>(h - '0');
                else if (h >= 'a' && h <= 'f') low |= static_cast<unsigned int>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') low |= static_cast<unsigned int>(h - 'A' + 10);
                else return Fail("invalid \\u hex digit");
            }
            if (low < 0xDC00 || low > 0xDFFF) return Fail("invalid low surrogate");
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("lone low surrogate in \\u escape");
        }
        // A NUL inside a std::string is a trap for every consumer that later touches it as a C string:
        // the value silently truncates at the escape. No protocol field has a use for one.
        if (cp == 0) return Fail("\\u0000 is not permitted");

        // Minimal UTF-8 encode.
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
        return true;
    }

    bool ParseBool(Value& out) {
        if (text_.substr(pos_, 4) == "true") { pos_ += 4; out = Value(true); return true; }
        if (text_.substr(pos_, 5) == "false") { pos_ += 5; out = Value(false); return true; }
        return Fail("invalid literal");
    }
    bool ParseNull(Value& out) {
        if (text_.substr(pos_, 4) == "null") { pos_ += 4; out = Value(); return true; }
        return Fail("invalid literal");
    }
    /// <summary>
    /// Parse a JSON number: grammar-checked, then converted without consulting the C locale.
    ///
    /// Two defects, both quiet:
    ///
    /// 1. The scanner accepted any run of digits and number-ish punctuation, so `1-2`, `1e+-` and
    ///    `..5` were "numbers"; std::stod then silently took whatever prefix it liked (`1-2` -> 1).
    ///    Undetected garbage from a third-party mod becomes a plausible-looking effect intensity.
    /// 2. std::stod honours LC_NUMERIC. Today the process never calls setlocale, so it happens to be
    ///    "C" and everything works -- but this is an application shipping in 30 languages, and one
    ///    setlocale(LC_ALL, "") anywhere in the process (or inside a DLL it loads) makes every
    ///    fractional value from every mod parse as its integer part. "0.5" becomes 0. Nothing would
    ///    report an error; effects would simply stop scaling.
    ///
    /// std::from_chars for floating point is locale-independent by definition and does not throw.
    /// </summary>
    bool ParseNumber(Value& out) {
        const std::size_t start = pos_;
        // JSON grammar: -? int frac? exp?
        if (Peek() == '-') ++pos_;
        const std::size_t intStart = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (pos_ == intStart) return Fail("invalid number: no integer part");
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            const std::size_t fracStart = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == fracStart) return Fail("invalid number: no digits after '.'");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            const std::size_t expStart = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == expStart) return Fail("invalid number: no digits in exponent");
        }

        const std::string_view tok = text_.substr(start, pos_ - start);
        double value = 0.0;
        const auto result = std::from_chars(tok.data(), tok.data() + tok.size(), value);
        if (result.ec != std::errc{} || result.ptr != tok.data() + tok.size()) {
            return Fail("number out of range");
        }
        out = Value(value);
        return true;
    }

    std::string_view text_;
    std::string& error_;
    std::size_t pos_ = 0;
};

}  // namespace detail

// Parse text into a Value. On failure returns a Null value and fills `error`.
inline Value Parse(std::string_view text, std::string& error) {
    error.clear();
    Value out;
    detail::Parser parser(text, error);
    if (!parser.Parse(out)) return Value();
    return out;
}

inline std::string Dump(const Value& v) {
    std::string out;
    detail::DumpTo(out, v);
    return out;
}

}  // namespace pulse::json
