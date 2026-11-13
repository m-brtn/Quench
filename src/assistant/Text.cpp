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
