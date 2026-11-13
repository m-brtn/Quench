// Text helpers of quench_assistant (Text.h).

#include "assistant/Text.h"

#include <format>
#include <utility>

namespace quench::assistant::detail {
namespace {

bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// A three-byte sequence of a surrogate (U+D800 .. U+DFFF) at `i`: its value, else 0.
unsigned surrogateAt(const std::string& text, std::size_t i) {
    if (i + 2 >= text.size()) return 0;
    const auto a = static_cast<unsigned char>(text[i]);
    const auto b = static_cast<unsigned char>(text[i + 1]);
    const auto c = static_cast<unsigned char>(text[i + 2]);
    if (a != 0xED || b < 0xA0 || b > 0xBF || !isContinuation(c)) return 0;
    return 0xD000u | ((b & 0x3Fu) << 6) | (c & 0x3Fu);
}

void appendUtf8(std::string& out, unsigned cp) {
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

} // namespace

void repairSurrogates(std::string& text) {
    if (text.find('\xED') == std::string::npos) return;
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned high = surrogateAt(text, i);
        if (high == 0) {
            out += text[i++];
            continue;
        }
        const unsigned low = high < 0xDC00 ? surrogateAt(text, i + 3) : 0;
        if (high < 0xDC00 && low >= 0xDC00) {
            appendUtf8(out, 0x10000u + ((high - 0xD800u) << 10) + (low - 0xDC00u));
            i += 6;
        } else {
            appendUtf8(out, 0xFFFD);
            i += 3;
        }
    }
    text = std::move(out);
}

std::size_t codePoints(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) n += isContinuation(static_cast<unsigned char>(c)) ? 0 : 1;
    return n;
}

std::string_view utf8Prefix(std::string_view text, std::size_t bytes) {
    if (bytes >= text.size()) return text;
    std::size_t end = bytes;
    while (end > 0 && isContinuation(static_cast<unsigned char>(text[end]))) --end;
    return text.substr(0, end);
}

std::string trimText(std::string_view text, std::size_t limit) {
    const std::size_t total = codePoints(text);
    if (total <= limit) return std::string(text);
    std::size_t count = 0;
    std::size_t end = 0;
    while (end < text.size()) {
        if (!isContinuation(static_cast<unsigned char>(text[end]))) {
            if (count == limit) break;
            ++count;
        }
        ++end;
    }
    return std::format("{}... ({} more characters)", text.substr(0, end), total - limit);
}

std::string_view trimSpace(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

} // namespace quench::assistant::detail
