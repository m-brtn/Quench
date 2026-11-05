#include "Test.h"

#include "app/ScenarioPoint.h"

#include <cmath>
#include <string>
#include <vector>

using namespace quench;
using app::detail::parseScenarioCoordinate;
using app::detail::parseScenarioPoint;
using app::detail::ScenarioPointLookup;

namespace {

const ScenarioPointLookup kNames = [](std::string_view name) -> std::optional<glm::vec2> {
    if (name == "menu:shadows") return glm::vec2(60.0f, 58.0f);
    return std::nullopt;
};

} // namespace

QUENCH_TEST(scenario_point_coordinates) {
    CHECK(parseScenarioCoordinate("120", 440.0f) == 120.0f, "points");
    CHECK(parseScenarioCoordinate("-12.5", 440.0f) == -12.5f, "negative points (a drag to the left)");
    CHECK(parseScenarioCoordinate("50%", 440.0f) == 220.0f, "percent of the extent");
    CHECK(parseScenarioCoordinate("25%", 956.0f) == 239.0f, "percent of another extent");
    CHECK(std::abs(*parseScenarioCoordinate("-10%", 200.0f) + 20.0f) < 1e-4f, "negative percent");
    CHECK(!parseScenarioCoordinate("", 440.0f), "empty");
    CHECK(!parseScenarioCoordinate("%", 440.0f), "percent sign alone");
    CHECK(!parseScenarioCoordinate("12px", 440.0f), "unknown unit");
    CHECK(!parseScenarioCoordinate("50%%", 440.0f), "two percent signs");
}

QUENCH_TEST(scenario_point_forms) {
    const glm::vec2 portrait(440.0f, 956.0f), landscape(960.0f, 540.0f);
    const std::vector<std::string> mixed{"75%", "380"};
    auto p = parseScenarioPoint(mixed, 0, portrait, kNames);
    REQUIRE(p.has_value(), "mixed percent and points");
    CHECK(p->position == glm::vec2(330.0f, 380.0f) && p->tokens == 2, "%f %f %zu", double(p->position.x),
          double(p->position.y), p->tokens);
    p = parseScenarioPoint(mixed, 0, landscape, kNames);
    REQUIRE(p.has_value(), "same arguments, other window");
    CHECK(p->position == glm::vec2(720.0f, 380.0f), "%f %f", double(p->position.x), double(p->position.y));

    const std::vector<std::string> named{"@menu:shadows", "left"};
    p = parseScenarioPoint(named, 0, portrait, kNames);
    REQUIRE(p.has_value(), "named point");
    CHECK(p->position == glm::vec2(60.0f, 58.0f) && p->tokens == 1, "one token for a name");

    const std::vector<std::string> drag{"25%", "70%", "60", "0", "30"};
    p = parseScenarioPoint(drag, 0, portrait, kNames);
    REQUIRE(p.has_value() && p->tokens == 2, "the point of a drag; the drag arguments follow it");
    CHECK(p->position.x == 110.0f && std::abs(p->position.y - 669.2f) < 1e-3f, "%f %f", double(p->position.x),
          double(p->position.y));
}

QUENCH_TEST(scenario_point_errors) {
    const glm::vec2 window(960.0f, 540.0f);
    CHECK(!parseScenarioPoint(std::vector<std::string>{}, 0, window, kNames), "no arguments");
    CHECK(!parseScenarioPoint(std::vector<std::string>{"10"}, 0, window, kNames), "one coordinate");
    CHECK(!parseScenarioPoint(std::vector<std::string>{"10", "y"}, 0, window, kNames), "not a number");
    CHECK(!parseScenarioPoint(std::vector<std::string>{"@menu:sky"}, 0, window, kNames), "unknown name");
    CHECK(!parseScenarioPoint(std::vector<std::string>{"@"}, 0, window, kNames), "empty name");
    CHECK(!parseScenarioPoint(std::vector<std::string>{"@menu:shadows"}, 0, window, nullptr), "no lookup");
}
