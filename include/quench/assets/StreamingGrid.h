#pragma once

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>
#include <vector>

// Streaming by distance on a grid of square cells in the XZ plane: says which cells to load (nearest first) and which
// to unload as focus points move. A cell is wanted when a focus is within loadRadius of its rectangle and dropped
// when every focus is farther than keepRadius (keepRadius > loadRadius keeps a cell on the border from loading and
// unloading every frame). What a cell holds is the game's: typically AssetRefs from an AssetManager, loaded with the
// distance as priority, so near cells load first and the manager's in-flight limit applies. There are no
// synchronous loads; a game that must not reach missing content checks its cells' state.
namespace quench::assets {

class StreamingGrid {
public:
    struct Settings {
        glm::vec2 origin{0.0f}; // world x, z of cell (0, 0)'s minimum corner
        float cellSize = 64.0f; // m
        int cellsX = 16;
        int cellsZ = 16;
        float loadRadius = 150.0f; // m from a focus to a cell's rectangle
        float keepRadius = 200.0f; // m; at least loadRadius
    };

    struct Cell {
        int x = 0;
        int z = 0;
        float distance = 0.0f; // to the nearest focus
        friend bool operator==(const Cell& a, const Cell& b) { return a.x == b.x && a.z == b.z; }
    };

    struct Changes {
        std::vector<Cell> load;   // nearest first
        std::vector<Cell> unload; // farthest first
    };

    struct Stats {
        std::uint32_t active = 0; // cells loaded and not unloaded yet
        std::uint64_t loads = 0;  // total
        std::uint64_t unloads = 0;
    };

    explicit StreamingGrid(const Settings& settings);

    // Activates the cells a focus came near and deactivates the cells every focus left.
    Changes update(std::span<const glm::vec2> focuses);
    Changes update(glm::vec2 focus) { return update(std::span<const glm::vec2>(&focus, 1)); }
    // Deactivates every cell (returns them all as unloads).
    Changes clear();

    bool active(int x, int z) const;
    // Distance from a point to the cell's rectangle, 0 inside.
    float distance(int x, int z, glm::vec2 point) const;
    // The cell containing a point; false outside the grid.
    bool cellAt(glm::vec2 point, int& x, int& z) const;
    glm::vec2 cellMin(int x, int z) const;
    glm::vec2 cellCenter(int x, int z) const;
    const Settings& settings() const { return m_settings; }
    const Stats& stats() const { return m_stats; }

private:
    Settings m_settings;
    std::vector<std::uint8_t> m_active;
    Stats m_stats;
};

} // namespace quench::assets
