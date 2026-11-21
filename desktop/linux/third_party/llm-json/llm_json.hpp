#pragma once
#define NOMINMAX
// llm_json.hpp — Single-header C++ JSON parser and builder
// MIT License — Mattbusel, 2026
// Usage: #define LLM_JSON_IMPLEMENTATION in ONE .cpp before including

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <initializer_list>
#include <utility>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace llm {
namespace json {

class Value {
public:
    enum class Type { Null, Bool, Int, Float, String, Array, Object };

    // ── Constructors ──────────────────────────────────────────────────────────
    Value()                       : type_(Type::Null)   {}
    Value(bool v)                 : type_(Type::Bool),   bool_(v)   {}
    Value(int v)                  : type_(Type::Int),    int_(v)    {}
    Value(long long v)            : type_(Type::Int),    int_(v)    {}
    Value(double v)               : type_(Type::Float),  float_(v)  {}
    Value(const std::string& v)   : type_(Type::String), str_(v)    {}
    Value(std::string&& v)        : type_(Type::String), str_(std::move(v)) {}
    Value(const char* v)          : type_(Type::String), str_(v)    {}

    // Array literal: Value arr = {1, "two", 3.0};
    Value(std::initializer_list<Value> items) : type_(Type::Array) {
        for (auto& i : items) arr_.push_back(i);
    }

    // ── Type checks ───────────────────────────────────────────────────────────
    bool is_null()   const { return type_ == Type::Null;   }
    bool is_bool()   const { return type_ == Type::Bool;   }
    bool is_int()    const { return type_ == Type::Int;    }
    bool is_float()  const { return type_ == Type::Float;  }
    bool is_number() const { return type_ == Type::Int || type_ == Type::Float; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array()  const { return type_ == Type::Array;  }
    bool is_object() const { return type_ == Type::Object; }
    Type type()      const { return type_; }

    // ── Accessors ─────────────────────────────────────────────────────────────
    bool as_bool() const {
        if (type_ != Type::Bool) throw std::runtime_error("json: not a bool");
        return bool_;
    }
    long long as_int() const {
        if (type_ == Type::Int)   return int_;
        if (type_ == Type::Float) return (long long)float_;
        throw std::runtime_error("json: not an int");
    }
    double as_float() const {
        if (type_ == Type::Float) return float_;
        if (type_ == Type::Int)   return (double)int_;
        throw std::runtime_error("json: not a float");
    }
    double as_number() const { return as_float(); }
    const std::string& as_string() const {
        if (type_ != Type::String) throw std::runtime_error("json: not a string");
        return str_;
    }
    std::string& as_string() {
        if (type_ != Type::String) throw std::runtime_error("json: not a string");
        return str_;
    }

    // ── Array access ──────────────────────────────────────────────────────────
    Value& operator[](size_t idx) {
        ensure_array();
        if (idx >= arr_.size()) throw std::out_of_range("json array index out of range");
        return arr_[idx];
    }
    const Value& operator[](size_t idx) const {
        if (type_ != Type::Array) throw std::runtime_error("json: not an array");
        if (idx >= arr_.size()) throw std::out_of_range("json array index out of range");
        return arr_[idx];
    }
    void push_back(Value v) { ensure_array(); arr_.push_back(std::move(v)); }
    size_t size() const {
        if (type_ == Type::Array)  return arr_.size();
        if (type_ == Type::Object) return obj_.size();
        return 0;
    }
    bool empty() const { return size() == 0; }
    std::vector<Value>::iterator begin() { ensure_array(); return arr_.begin(); }
    std::vector<Value>::iterator end()   { ensure_array(); return arr_.end();   }
    std::vector<Value>::const_iterator begin() const { return arr_.begin(); }
    std::vector<Value>::const_iterator end()   const { return arr_.end();   }

    // ── Object access ─────────────────────────────────────────────────────────
    Value& operator[](const std::string& key) {
        ensure_object();
        for (auto& kv : obj_) if (kv.first == key) return kv.second;
        obj_.emplace_back(key, Value{});
        return obj_.back().second;
    }
    const Value& operator[](const std::string& key) const {
        if (type_ != Type::Object) throw std::runtime_error("json: not an object");
        for (const auto& kv : obj_) if (kv.first == key) return kv.second;
        throw std::out_of_range("json key not found: " + key);
    }
    bool contains(const std::string& key) const {
        if (type_ != Type::Object) return false;
        for (const auto& kv : obj_) if (kv.first == key) return true;
        return false;
    }
    void set(const std::string& key, Value v) { (*this)[key] = std::move(v); }
    bool erase(const std::string& key) {
        if (type_ != Type::Object) return false;
        for (auto it = obj_.begin(); it != obj_.end(); ++it) {
            if (it->first == key) { obj_.erase(it); return true; }
        }
        return false;
    }
    std::vector<std::string> keys() const {
        std::vector<std::string> ks;
        if (type_ == Type::Object)
            for (const auto& kv : obj_) ks.push_back(kv.first);
        return ks;
    }

    // ── Safe access with default ───────────────────────────────────────────────
    Value get(const std::string& key, const Value& def = {}) const {
        if (type_ != Type::Object) return def;
        for (const auto& kv : obj_) if (kv.first == key) return kv.second;
        return def;
    }
    Value get(size_t idx, const Value& def = {}) const {
        if (type_ != Type::Array || idx >= arr_.size()) return def;
        return arr_[idx];
    }

    // ── Serialization ──────────────────────────────────────────────────────────
    std::string dump(int indent = -1) const;
    std::string dump_compact() const   { return dump(-1); }
    std::string dump_pretty(int ind=2) const { return dump(ind); }

    // Internal helpers — must be accessible to free functions and Parser
    void ensure_array()  { if (type_ == Type::Null) type_ = Type::Array;  }
    void ensure_object() { if (type_ == Type::Null) type_ = Type::Object; }

private:
    Type type_;
    bool      bool_  = false;
    long long int_   = 0;
    double    float_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;

    void dump_impl(std::string& out, int indent, int depth) const;
    static void dump_string(std::string& out, const std::string& s);
    friend Value parse(const std::string&);
    friend Value parse(const char*, size_t);
    friend struct Parser;
};

// ── Free functions ────────────────────────────────────────────────────────────

Value parse(const std::string& json_str);
Value parse(const char* json_str, size_t length);

struct ParseResult {
    Value value;
    bool ok = false;
    std::string error;
    size_t error_offset = 0;
};
ParseResult try_parse(const std::string& json_str);

inline Value object() { Value v; v.ensure_object(); return v; }
inline Value array()  { Value v; v.ensure_array();  return v; }
inline Value null_value() { return Value{}; }

inline Value make_object(std::initializer_list<std::pair<std::string, Value>> pairs) {
    Value v;
    for (auto& p : pairs) v.set(p.first, p.second);
    return v;
}

// Free-function dump convenience
inline std::string dump(const Value& v, int indent = -1) { return v.dump(indent); }

} // namespace json
} // namespace llm

// ─────────────────────────────────────────────────────────────────────────────
#ifdef LLM_JSON_IMPLEMENTATION

#include <sstream>
#include <cassert>

namespace llm {
namespace json {

// ── Parser ────────────────────────────────────────────────────────────────────

struct Parser {
    const char* p;
    const char* end;
    size_t offset() const { return 0; } // simplified

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }

    Value parse_value();
    Value parse_string_val();
    Value parse_number();
    Value parse_array();
    Value parse_object();
    std::string parse_string_raw();

    void expect(char c) {
        skip_ws();
        if (p >= end || *p != c)
            throw std::runtime_error(std::string("json: expected '") + c + "' at offset " + std::to_string(p - (end - (end - p))));
        ++p;
    }
    bool consume(char c) { skip_ws(); if (p < end && *p == c) { ++p; return true; } return false; }

    static uint32_t hex4(const char* s) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v <<= 4;
            char c = s[i];
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else throw std::runtime_error("json: bad unicode escape");
        }
        return v;
    }
    static void encode_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) { out += (char)cp; }
        else if (cp < 0x800) { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
        else { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
    }
};

std::string Parser::parse_string_raw() {
    expect('"');
    std::string out;
    while (p < end && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (p >= end) throw std::runtime_error("json: unexpected end in string");
            switch (*p++) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (p + 4 > end) throw std::runtime_error("json: short unicode");
                    uint32_t cp = hex4(p); p += 4;
                    encode_utf8(out, cp);
                    break;
                }
                default: throw std::runtime_error("json: bad escape");
            }
        } else {
            out += *p++;
        }
    }
    if (p >= end) throw std::runtime_error("json: unterminated string");
    ++p; // closing "
    return out;
}

Value Parser::parse_number() {
    const char* start = p;
    bool is_float = false;
    if (*p == '-') ++p;
    while (p < end && *p >= '0' && *p <= '9') ++p;
    if (p < end && *p == '.') { is_float = true; ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
    if (p < end && (*p == 'e' || *p == 'E')) { is_float = true; ++p; if (p < end && (*p == '+' || *p == '-')) ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
    std::string s(start, p);
    if (is_float) return Value(std::stod(s));
    return Value((long long)std::stoll(s));
}

Value Parser::parse_array() {
    expect('[');
    Value v; v.ensure_array();
    skip_ws();
    if (p < end && *p == ']') { ++p; return v; }
    do {
        skip_ws();
        v.push_back(parse_value());
        skip_ws();
    } while (consume(','));
    expect(']');
    return v;
}

Value Parser::parse_object() {
    expect('{');
    Value v; v.ensure_object();
    skip_ws();
    if (p < end && *p == '}') { ++p; return v; }
    do {
        skip_ws();
        std::string key = parse_string_raw();
        skip_ws(); expect(':');
        skip_ws();
        Value val = parse_value();
        v.set(key, std::move(val));
        skip_ws();
    } while (consume(','));
    expect('}');
    return v;
}

Value Parser::parse_value() {
    skip_ws();
    if (p >= end) throw std::runtime_error("json: unexpected end");
    if (*p == '"') return Value(parse_string_raw());
    if (*p == '[') return parse_array();
    if (*p == '{') return parse_object();
    if (*p == 't') {
        if (p+4 <= end && strncmp(p, "true", 4) == 0) { p += 4; return Value(true); }
        throw std::runtime_error("json: expected 'true'");
    }
    if (*p == 'f') {
        if (p+5 <= end && strncmp(p, "false", 5) == 0) { p += 5; return Value(false); }
        throw std::runtime_error("json: expected 'false'");
    }
    if (*p == 'n') {
        if (p+4 <= end && strncmp(p, "null", 4) == 0) { p += 4; return Value{}; }
        throw std::runtime_error("json: expected 'null'");
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) return parse_number();
    throw std::runtime_error(std::string("json: unexpected char '") + *p + "'");
}

Value parse(const char* s, size_t len) {
    Parser p;
    p.p = s; p.end = s + len;
    Value v = p.parse_value();
    p.skip_ws();
    if (p.p != p.end) throw std::runtime_error("json: trailing garbage");
    return v;
}

Value parse(const std::string& s) { return parse(s.c_str(), s.size()); }

ParseResult try_parse(const std::string& s) {
    ParseResult r;
    try {
        r.value = parse(s);
        r.ok = true;
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    return r;
}

// ── Serializer ────────────────────────────────────────────────────────────────

void Value::dump_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[7]; snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += (char)c;
        }
    }
    out += '"';
}

void Value::dump_impl(std::string& out, int indent, int depth) const {
    auto nl_indent = [&](int d) {
        if (indent >= 0) { out += '\n'; out.append(d * indent, ' '); }
    };
    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Int:    out += std::to_string(int_); break;
        case Type::Float: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.17g", float_);
            out += buf;
            break;
        }
        case Type::String: dump_string(out, str_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i > 0) out += ',';
                nl_indent(depth + 1);
                arr_[i].dump_impl(out, indent, depth + 1);
            }
            if (!arr_.empty()) nl_indent(depth);
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i > 0) out += ',';
                nl_indent(depth + 1);
                dump_string(out, obj_[i].first);
                out += ':';
                if (indent >= 0) out += ' ';
                obj_[i].second.dump_impl(out, indent, depth + 1);
            }
            if (!obj_.empty()) nl_indent(depth);
            out += '}';
            break;
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    out.reserve(256);
    dump_impl(out, indent, 0);
    return out;
}

} // namespace json
} // namespace llm

#endif // LLM_JSON_IMPLEMENTATION