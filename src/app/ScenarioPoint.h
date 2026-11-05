#pragma once

// Pointer positions of the scenario commands `mouse`, `click` and `touch` (docs/app.md, "Scenarios"): window points,
// percent of the window, or a point the game names, so that one script fits every window size and orientation.

#include <glm/vec2.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace quench::app::detail {

// One coordinate: window points ("120", "-12.5") or percent of the window's extent on that axis ("25%").
std::optional<float> parseScenarioCoordinate(std::string_view token, float extent);

struct ScenarioPoint {
    glm::vec2 position{0.0f}; // window points
    std::size_t tokens = 0;   // arguments taken: 2 for `X Y`, 1 for `@NAME`
};

using ScenarioPointLookup = std::function<std::optional<glm::vec2>(std::string_view name)>;

// The point at args[first]: `X Y` (each one in points or percent) or `@NAME`, resolved by `named`
// (Game::scenarioPoint). nullopt for a missing or malformed coordinate and for a name `named` does not know.
std::optional<ScenarioPoint> parseScenarioPoint(std::span<const std::string> args, std::size_t first, glm::vec2 window,
                                                const ScenarioPointLookup& named);

} // namespace quench::app::detail
