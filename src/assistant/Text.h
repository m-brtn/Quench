#pragma once

// Text helpers of quench_assistant: UTF-8 cuts and counts, the trimming of tool results.

#include <quench/assistant/Provider.h>
#include <quench/net/Http.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace quench::assistant::detail {

// Content-Type, Authorization (with a key) and the settings' extra headers.
net::Headers requestHeaders(const ProviderSettings& settings, std::string contentType);
// The base URL without its trailing '/' and `path` ("/chat/completions").
std::string endpointUrl(const ProviderSettings& settings, std::string_view path);

// The code points of `text` (a byte that does not continue a sequence counts as one).
std::size_t codePoints(std::string_view text);

// At most `limit` code points of `text`; when it is longer, its start and "... (N more characters)".
std::string trimText(std::string_view text, std::size_t limit);

// The longest prefix of `text` of at most `bytes` bytes that does not end inside a UTF-8 sequence.
std::string_view utf8Prefix(std::string_view text, std::size_t bytes);

// Without the whitespace at both ends.
std::string_view trimSpace(std::string_view text);

} // namespace quench::assistant::detail
