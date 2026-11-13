#pragma once

#include <quench/core/Result.h>

#include <map>
#include <string>
#include <vector>

namespace quench::json {

// Minimal JSON document model, parser and writer. UTF-8 passthrough: the writer escapes only '"', '\\' and the control
// characters, so text outside the Basic Multilingual Plane is written as its four UTF-8 bytes. The parser decodes \u
// escapes to UTF-8, a surrogate pair ("\ud83d\ude00") to one four-byte code point; a lone surrogate becomes U+FFFD
// (what a browser's TextEncoder makes of one) rather than an error, so one bad escape in a server's text does not lose
// the whole document; a \u without four hex digits is an error. Object keys are kept sorted (std::map). Numbers are
// parsed and written with the C library, so the process must keep the "C" numeric locale.
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    Value() = default;
    static Value makeNumber(double v);
    static Value makeBool(bool v);
    static Value makeString(std::string v);
    static Value makeArray();
    static Value makeObject();

    bool isNull() const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isNumber() const { return type == Type::Number; }
    // Member lookup; returns a static null value when this is not an object or the key is absent.
    const Value& operator[](const std::string& key) const;
    bool has(const std::string& key) const { return type == Type::Object && object.count(key) != 0; }
    double numberOr(double fallback) const { return type == Type::Number ? number : fallback; }
    std::string stringOr(const std::string& fallback) const { return type == Type::String ? string : fallback; }
    bool boolOr(bool fallback) const { return type == Type::Bool ? boolean : fallback; }
};

// Parses a document; the error names the problem and its byte offset.
Result<Value> parse(const std::string& text);

// Serializes with two-space indentation; numbers use the shortest form that reads back to the same double, whole
// numbers below 9e15 without an exponent.
std::string write(const Value& v);

// Recursive merge: object members of `overlay` replace or extend those of `base`; any other overlay value replaces
// the base value.
void merge(Value& base, const Value& overlay);

} // namespace quench::json
