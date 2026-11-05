#pragma once

// Strict number parsing for command lines and scripts: the whole text must be the number (no spaces, no suffix).
// Uses strtod / strtoll in the "C" locale the engine runs in (std::from_chars for floating point needs macOS 26).

#include <cstdint>
#include <optional>
#include <string_view>

namespace quench {

std::optional<double> parseDouble(std::string_view text);
std::optional<std::int64_t> parseInt(std::string_view text);

} // namespace quench
