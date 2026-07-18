// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PulseCore Game Integration — minimal JSON value + parser/serializer.
//
// Deliberately dependency-free and small: the Bridge protocol (v1) and effects.json only need flat
// objects, arrays, strings, numbers, bools and null. A bounded recursive-descent parser is used so a
// malformed or hostile message errors out (never crashes / never recurses without limit). Part of the
// OPEN protocol/SDK layer (PolyForm Noncommercial 1.0.0) — no PulseCore-proprietary dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
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
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.6g", n);
                out += buf;
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
        // Minimal UTF-8 encode of the basic plane (surrogate pairs are not needed for v1 payloads).
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
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
    bool ParseNumber(Value& out) {
        const std::size_t start = pos_;
        if (Peek() == '-') ++pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') ++pos_;
            else break;
        }
        const std::string_view tok = text_.substr(start, pos_ - start);
        if (tok.empty()) return Fail("invalid number");
        try {
            out = Value(std::stod(std::string(tok)));
        } catch (...) {
            return Fail("number out of range");
        }
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
