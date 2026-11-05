#include "Test.h"

#include <quench/assets/StreamingGrid.h>

#include <algorithm>

using namespace quench;
using namespace quench::assets;

namespace {

StreamingGrid::Settings gridSettings() {
    StreamingGrid::Settings s;
    s.origin = {-50.0f, -50.0f};
    s.cellSize = 10.0f;
    s.cellsX = 10;
    s.cellsZ = 10;
    s.loadRadius = 12.0f;
    s.keepRadius = 22.0f;
    return s;
}

bool has(const std::vector<StreamingGrid::Cell>& cells, int x, int z) {
    return std::find(cells.begin(), cells.end(), StreamingGrid::Cell{x, z, 0.0f}) != cells.end();
}

} // namespace

QUENCH_TEST(streaming_grid_geometry) {
    const StreamingGrid grid(gridSettings());
    int x = -1, z = -1;
    CHECK(grid.cellAt({-50.0f, -50.0f}, x, z) && x == 0 && z == 0, "corner cell %d %d", x, z);
    CHECK(grid.cellAt({1.0f, -1.0f}, x, z) && x == 5 && z == 4, "cell %d %d", x, z);
    CHECK(!grid.cellAt({50.0f, 0.0f}, x, z) && !grid.cellAt({0.0f, -50.5f}, x, z), "outside the grid");
    CHECK(grid.distance(5, 5, {5.0f, 5.0f}) == 0.0f, "inside: %f", grid.distance(5, 5, {5.0f, 5.0f}));
    CHECK(grid.distance(5, 5, {13.0f, 14.0f}) == 5.0f, "corner: %f", grid.distance(5, 5, {13.0f, 14.0f}));
    CHECK(grid.cellCenter(0, 9) == glm::vec2(-45.0f, 45.0f), "center");
}

QUENCH_TEST(streaming_grid_load_unload) {
    StreamingGrid grid(gridSettings());
    // In the middle of cell (5, 5): the 3 x 3 cells around it are within 12 m, the next ring at 15 m is not.
    StreamingGrid::Changes c = grid.update(glm::vec2(5.0f, 5.0f));
    CHECK(c.load.size() == 9 && c.unload.empty(), "%zu loads, %zu unloads", c.load.size(), c.unload.size());
    CHECK(!c.load.empty() && c.load.front().x == 5 && c.load.front().z == 5 && c.load.front().distance == 0.0f,
          "nearest first");
    CHECK(std::is_sorted(c.load.begin(), c.load.end(),
                         [](const auto& a, const auto& b) { return a.distance < b.distance; }),
          "loads not sorted by distance");
    CHECK(grid.active(4, 4) && grid.active(6, 6) && !grid.active(7, 5), "active cells");

    // Moving within the hysteresis band unloads nothing.
    c = grid.update(glm::vec2(15.0f, 5.0f));
    CHECK(c.unload.empty() && c.load.size() == 3 && has(c.load, 7, 4) && has(c.load, 7, 5) && has(c.load, 7, 6),
          "%zu loads, %zu unloads", c.load.size(), c.unload.size());
    c = grid.update(glm::vec2(5.0f, 5.0f));
    CHECK(c.load.empty() && c.unload.empty(), "back and forth churns: %zu loads, %zu unloads", c.load.size(),
          c.unload.size());

    // Far away: everything goes, farthest first.
    c = grid.update(glm::vec2(-45.0f, -45.0f));
    CHECK(c.unload.size() == 12 && !has(c.unload, 0, 0) && has(c.load, 0, 0), "%zu unloads", c.unload.size());
    CHECK(std::is_sorted(c.unload.begin(), c.unload.end(),
                         [](const auto& a, const auto& b) { return a.distance > b.distance; }),
          "unloads not sorted");
    CHECK(grid.stats().active == 4 && grid.stats().loads == 16 && grid.stats().unloads == 12, "stats %u %llu %llu",
          grid.stats().active, (unsigned long long)grid.stats().loads, (unsigned long long)grid.stats().unloads);
    c = grid.clear();
    CHECK(c.unload.size() == 4 && grid.stats().active == 0, "clear: %zu", c.unload.size());
}

QUENCH_TEST(streaming_grid_two_focuses) {
    StreamingGrid grid(gridSettings());
    const glm::vec2 focuses[] = {{-45.0f, -45.0f}, {45.0f, 45.0f}};
    const StreamingGrid::Changes c = grid.update(focuses);
    CHECK(c.load.size() == 8 && grid.active(0, 0) && grid.active(9, 9) && grid.active(8, 9) && !grid.active(5, 5),
          "%zu loads", c.load.size());
}
