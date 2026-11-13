#include <quench/core/Json.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace quench::json {
namespace {

class Parser {
public:
    explicit Parser(const std::string& t) : m_t(t) {}

    bool document(Value& out) {
        skipWs();
        if (!value(out, 0)) return false;
        skipWs();
        if (m_i != m_t.size()) return fail("trailing characters");
        return true;
    }
    std::string error;

private:
    bool fail(const char* what) {
        error = std::string(what) + " at offset " + std::to_string(m_i);
        return false;
    }
    void skipWs() {
        while (m_i < m_t.size() && (m_t[m_i] == ' ' || m_t[m_i] == '\n' || m_t[m_i] == '\r' || m_t[m_i] == '\t')) ++m_i;
    }
    bool literal(const char* word) {
        std::size_t n = 0;
        while (word[n]) ++n;
        if (m_t.compare(m_i, n, word) != 0) return fail("invalid literal");
        m_i += n;
        return true;
    }
    bool string(std::string& out) {
        if (m_t[m_i] != '"') return fail("expected string");
        ++m_i;
        while (m_i < m_t.size() && m_t[m_i] != '"') {
            char c = m_t[m_i++];
            if (c == '\\') {
                if (m_i >= m_t.size()) return fail("bad escape");
                const char e = m_t[m_i++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        // A high surrogate: with the low one of a following \u escape, one code point outside the
                        // Basic Multilingual Plane; without it, U+FFFD (a following escape is then read on its own).
                        unsigned low = 0;
                        const std::size_t at = m_i;
                        if (m_i + 1 < m_t.size() && m_t[m_i] == '\\' && m_t[m_i + 1] == 'u') {
                            m_i += 2;
                            if (!hex4(low)) return fail("bad \\u escape");
                        }
                        if (low >= 0xDC00 && low < 0xE000) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        } else {
                            cp = 0xFFFD;
                            m_i = at;
                        }
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        cp = 0xFFFD; // a lone low surrogate
                    }
                    appendUtf8(out, cp);
                    continue;
                }
                default: c = e; break; // \" \\ \/
                }
            }
            out += c;
        }
        if (m_i >= m_t.size()) return fail("unterminated string");
        ++m_i;
        return true;
    }
    // Four hex digits at m_i (consumed only when all four are digits).
    bool hex4(unsigned& cp) {
        if (m_i + 4 > m_t.size()) return false;
        cp = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char h = m_t[m_i + k];
            unsigned d = 0;
            if (h >= '0' && h <= '9') {
                d = unsigned(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                d = unsigned(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                d = unsigned(h - 'A' + 10);
            } else {
                return false;
            }
            cp = cp << 4 | d;
        }
        m_i += 4;
        return true;
    }
    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    bool value(Value& v, int depth) {
        if (depth > 64) return fail("nesting too deep");
        if (m_i >= m_t.size()) return fail("unexpected end");
        const char c = m_t[m_i];
        if (c == '{') {
            v.type = Value::Type::Object;
            ++m_i;
            skipWs();
            if (m_i < m_t.size() && m_t[m_i] == '}') return close();
            for (;;) {
                skipWs();
                std::string key;
                if (m_i >= m_t.size() || !string(key)) return fail("expected key");
                skipWs();
                if (m_i >= m_t.size() || m_t[m_i] != ':') return fail("expected ':'");
                ++m_i;
                skipWs();
                if (!value(v.object[key], depth + 1)) return false;
                skipWs();
                if (m_i < m_t.size() && m_t[m_i] == ',') {
                    ++m_i;
                    continue;
                }
                if (m_i < m_t.size() && m_t[m_i] == '}') return close();
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Value::Type::Array;
            ++m_i;
            skipWs();
            if (m_i < m_t.size() && m_t[m_i] == ']') return close();
            for (;;) {
                skipWs();
                v.array.emplace_back();
                if (!value(v.array.back(), depth + 1)) return false;
                skipWs();
                if (m_i < m_t.size() && m_t[m_i] == ',') {
                    ++m_i;
                    continue;
                }
                if (m_i < m_t.size() && m_t[m_i] == ']') return close();
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Value::Type::String;
            return string(v.string);
        }
        if (c == 't') return boolean(v, true, "true");
        if (c == 'f') return boolean(v, false, "false");
        if (c == 'n') {
            v.type = Value::Type::Null;
            return literal("null");
        }
        const char* begin = m_t.c_str() + m_i;
        char* end = nullptr;
        v.number = std::strtod(begin, &end);
        if (end == begin) return fail("unexpected character");
        v.type = Value::Type::Number;
        m_i += std::size_t(end - begin);
        return true;
    }
    bool close() {
        ++m_i;
        return true;
    }
    bool boolean(Value& v, bool b, const char* word) {
        v.type = Value::Type::Bool;
        v.boolean = b;
        return literal(word);
    }

    const std::string& m_t;
    std::size_t m_i = 0;
};

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void writeNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    char buf[40];
    // Whole numbers without an exponent: glTF readers parse integer fields such as byte offsets with strtol and would
    // read "1.63958e+06" as 1.
    if (v == std::floor(v) && std::abs(v) < 9.0e15) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    out += buf;
}

void writeValue(std::string& out, const Value& v, int indent) {
    const std::string pad(std::size_t(indent) * 2, ' ');
    const std::string padIn(std::size_t(indent + 1) * 2, ' ');
    switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Type::Number: writeNumber(out, v.number); break;
    case Value::Type::String: writeString(out, v.string); break;
    case Value::Type::Array: {
        bool flat = true;
        for (const Value& e : v.array) flat = flat && (e.type == Value::Type::Number || e.type == Value::Type::Bool);
        if (v.array.empty()) {
            out += "[]";
        } else if (flat) {
            out += '[';
            for (std::size_t i = 0; i < v.array.size(); ++i) {
                if (i) out += ", ";
                writeValue(out, v.array[i], indent + 1);
            }
            out += ']';
        } else {
            out += "[\n";
            for (std::size_t i = 0; i < v.array.size(); ++i) {
                out += padIn;
                writeValue(out, v.array[i], indent + 1);
                out += i + 1 < v.array.size() ? ",\n" : "\n";
            }
            out += pad + "]";
        }
        break;
    }
    case Value::Type::Object: {
        if (v.object.empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        std::size_t i = 0;
        for (const auto& [key, member] : v.object) {
            out += padIn;
            writeString(out, key);
            out += ": ";
            writeValue(out, member, indent + 1);
            out += ++i < v.object.size() ? ",\n" : "\n";
        }
        out += pad + "}";
        break;
    }
    }
}

} // namespace

const Value& Value::operator[](const std::string& key) const {
    static const Value kNull;
    if (type != Type::Object) return kNull;
    const auto it = object.find(key);
    return it != object.end() ? it->second : kNull;
}

Value Value::makeNumber(double v) {
    Value r;
    r.type = Type::Number;
    r.number = v;
    return r;
}

Value Value::makeBool(bool v) {
    Value r;
    r.type = Type::Bool;
    r.boolean = v;
    return r;
}

Value Value::makeString(std::string v) {
    Value r;
    r.type = Type::String;
    r.string = std::move(v);
    return r;
}

Value Value::makeArray() {
    Value r;
    r.type = Type::Array;
    return r;
}

Value Value::makeObject() {
    Value r;
    r.type = Type::Object;
    return r;
}

Result<Value> parse(const std::string& text) {
    Parser p(text);
    Value out;
    if (!p.document(out)) return Error{p.error};
    return out;
}

std::string write(const Value& v) {
    std::string out;
    writeValue(out, v, 0);
    out += '\n';
    return out;
}

void merge(Value& base, const Value& overlay) {
    if (base.type == Value::Type::Object && overlay.type == Value::Type::Object) {
        for (const auto& [key, member] : overlay.object) merge(base.object[key], member);
        return;
    }
    base = overlay;
}

} // namespace quench::json
