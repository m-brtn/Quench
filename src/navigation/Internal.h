#pragma once

// The navigation module's build steps and what its classes share. Recast and Detour stay behind these declarations:
// the public headers do not include them.

#include <quench/core/Result.h>
#include <quench/navigation/NavMesh.h>
#include <quench/navigation/NavQuery.h>

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace quench::navigation::detail {

inline constexpr const char* kLog = "navigation";

// Recast's area ids inside layers: 0 is "not walkable", a game area a is a + 1. Polygons get the game's id back.
inline std::uint8_t layerArea(AreaId area) { return area == kNotWalkable ? 0 : std::uint8_t(area + 1); }
inline std::uint16_t areaFlag(AreaId area) { return std::uint16_t(1u << area); }

// BuildSettings resolved for a tile grid: voxel sizes of the agent, the grid's extent.
struct Config {
    BuildSettings settings; // areas filled in, bounds: the grid's
    int tilesX = 0;
    int tilesZ = 0;
    float tileWorld = 0.0f; // tileSize * cellSize
    int walkableHeight = 0; // cells
    int walkableClimb = 0;
    int walkableRadius = 0;
    int borderSize = 0;
    float cosSlope = 0.0f;

    glm::vec3 tileMin(int tx, int tz) const;
    glm::vec3 tileMax(int tx, int tz) const;
};

// Checks the settings and lays a tile grid over [boundsMin, boundsMax] (the settings' bounds when they are set).
Result<Config> makeConfig(const BuildSettings& settings, glm::vec3 boundsMin, glm::vec3 boundsMax);
// Checks indices, areas and coordinates; returns the bounds of the triangles' vertices.
Result<std::pair<glm::vec3, glm::vec3>> checkGeometry(const Geometry& geometry, const BuildSettings& settings);

// Geometry ready for voxelizing: Recast's area per triangle (slopes resolved) and the triangles of each tile.
struct SourceGeometry {
    Geometry geometry;
    std::vector<std::uint8_t> triangleAreas;
    std::vector<std::vector<int>> tileTriangles; // by tile index tz * tilesX + tx; filled for the masked tiles
    int maskMinX = 0, maskMinZ = 0, maskMaxX = -1, maskMaxZ = -1;

    // Fills triangleAreas and tileTriangles (a job: the tiles' builds start after it).
    void prepare(const Config& config);
};

using LayerBytes = std::shared_ptr<const std::vector<std::uint8_t>>;

// Voxelizes one tile: its floors as DetourTileCache layers (at most maxLayersPerTile).
Result<std::vector<LayerBytes>> rasterizeTile(const Config& config, const SourceGeometry& source, int tx, int tz);

struct DtFree {
    void operator()(unsigned char* data) const;
};

// Detour tile data made from one layer: empty when the layer has no polygons left.
struct TileData {
    std::unique_ptr<unsigned char[], DtFree> data;
    int size = 0;
    std::uint64_t hash = 0;
    int polygons = 0;
};

// Builds a layer's polygons, its obstacles marked, with the links that start in it.
Result<TileData> buildTile(const Config& config, std::span<const std::uint8_t> layer,
                           std::span<const Obstacle> obstacles, std::span<const Link> links);

// Checks a layer read from a file: header, size, place in the grid.
Result<void> checkLayer(const Config& config, const NavMeshData::Layer& layer);

// The obstacle's footprint grown by the agent radius (x, z) and its height range.
void obstacleBounds(const Obstacle& obstacle, const AgentConfig& agent, glm::vec3& min, glm::vec3& max);

dtQueryFilter toDetour(const Filter& filter);

struct NavMeshAccess {
    static dtNavMesh* mesh(const NavMesh& mesh);
    static const Config& config(const NavMesh& mesh);
    static std::vector<Obstacle> obstacles(const NavMesh& mesh);
};

} // namespace quench::navigation::detail
