#include "ScenarioPoint.h"

#include <quench/core/Parse.h>

namespace quench::app::detail {

std::optional<float> parseScenarioCoordinate(std::string_view token, float extent) {
    const bool percent = token.ends_with('%');
    if (percent) token.remove_suffix(1);
    const std::optional<double> value = parseDouble(token);
    if (!value) return std::nullopt;
    return percent ? float(*value / 100.0) * extent : float(*value);
}

std::optional<ScenarioPoint> parseScenarioPoint(std::span<const std::string> args, std::size_t first, glm::vec2 window,
                                                const ScenarioPointLookup& named) {
    if (first >= args.size()) return std::nullopt;
    const std::string_view token = args[first];
    if (token.starts_with('@')) {
        if (token.size() < 2 || !named) return std::nullopt;
        const std::optional<glm::vec2> point = named(token.substr(1));
        if (!point) return std::nullopt;
        return ScenarioPoint{.position = *point, .tokens = 1};
    }
    if (first + 1 >= args.size()) return std::nullopt;
    const std::optional<float> x = parseScenarioCoordinate(token, window.x);
    const std::optional<float> y = parseScenarioCoordinate(args[first + 1], window.y);
    if (!x || !y) return std::nullopt;
    return ScenarioPoint{.position = glm::vec2(*x, *y), .tokens = 2};
}

} // namespace quench::app::detail
