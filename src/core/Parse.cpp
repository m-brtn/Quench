#include <quench/core/Parse.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

namespace quench {

std::optional<double> parseDouble(std::string_view text) {
    if (text.empty() || std::isspace(static_cast<unsigned char>(text.front()))) return std::nullopt;
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || errno == ERANGE || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::optional<std::int64_t> parseInt(std::string_view text) {
    if (text.empty() || std::isspace(static_cast<unsigned char>(text.front()))) return std::nullopt;
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(copy.c_str(), &end, 10);
    if (end != copy.c_str() + copy.size() || errno == ERANGE) return std::nullopt;
    return std::int64_t(value);
}

} // namespace quench
