// Text helpers of quench_assistant (Text.h).

#include "assistant/Text.h"

#include <format>

namespace quench::assistant::detail {
namespace {

bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

} // namespace

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

namespace {

std::string cut(std::string_view text, std::size_t limit, std::size_t alreadyOut) {
    const std::size_t total = codePoints(text);
    if (total <= limit && alreadyOut == 0) return std::string(text);
    if (total <= limit) return std::format("{}... ({} more characters)", text, alreadyOut);
    std::size_t count = 0;
    std::size_t end = 0;
    while (end < text.size()) {
        if (!isContinuation(static_cast<unsigned char>(text[end]))) {
            if (count == limit) break;
            ++count;
        }
        ++end;
    }
    return std::format("{}... ({} more characters)", text.substr(0, end), total - limit + alreadyOut);
}

} // namespace

std::string trimText(std::string_view text, std::size_t limit) { return cut(text, limit, 0); }

std::string retrimText(std::string_view text, std::size_t limit) {
    if (codePoints(text) <= limit) return std::string(text);
    constexpr std::string_view open = "... (";
    constexpr std::string_view close = " more characters)";
    std::size_t alreadyOut = 0;
    std::string_view body = text;
    if (text.ends_with(close)) {
        const std::size_t at = text.rfind(open);
        if (at != std::string_view::npos) {
            const std::string_view digits =
                text.substr(at + open.size(), text.size() - close.size() - at - open.size());
            bool number = !digits.empty() && digits.size() < 16;
            std::size_t value = 0;
            for (const char c : digits) {
                if (c < '0' || c > '9') {
                    number = false;
                    break;
                }
                value = value * 10 + std::size_t(c - '0');
            }
            if (number) {
                alreadyOut = value;
                body = text.substr(0, at);
            }
        }
    }
    return cut(body, limit, alreadyOut);
}

std::string_view trimSpace(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

} // namespace quench::assistant::detail
