#pragma once

// Navigation meshes, built with Recast and DetourTileCache's layer builder, read by Detour. A NavMesh is a grid of
// square tiles; each tile holds one layer per walkable floor above another (a bridge over a road: two layers). It is
// built at run time from triangles (NavMesh::build), tiles in parallel on the job system, or made from cooked data
// (NavMesh::create: a .qnav asset, NavMeshAsset.h, written by quench_cook's navmesh command). Dynamic obstacles and
// geometry changes rebuild the tiles they touch, also on jobs. NavMesh::update(), called once per frame on the thread
// that owns the mesh, adds the finished tiles and starts the next builds: it never waits.
//
// Queries (NavQuery.h) and crowds (Crowd.h) read the mesh on that thread, or in jobs between two update() calls (a
// NavQuery per job). Tiles change only inside update().
//
// Units: meters, Y up. Triangles face up when counter-clockwise seen from above (the engine's front faces).

#include <quench/core/Result.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quench {
class JobSystem;
}

namespace quench::navigation {

// Area types: ids 0 to kMaxAreas - 1, named and priced by the game (BuildSettings::areas); kNotWalkable marks
// triangles, boxes and obstacles that never carry the mesh.
using AreaId = std::uint8_t;
inline constexpr AreaId kDefaultArea = 0;
inline constexpr AreaId kNotWalkable = 255;
inline constexpr int kMaxAreas = 16;

struct AreaType {
    std::string name;
    // Multiplies the length of a path through the area. Costs of at least 1 keep the paths found the cheapest ones
    // (the search estimates the rest of a path by its straight distance).
    float cost = 1.0f;
};

// The agents a mesh is built for: the walkable surface keeps `radius` from walls, has `height` of headroom, steps up
// to `maxClimb` and slopes of up to `maxSlope` degrees.
struct AgentConfig {
    float radius = 0.4f;
    float height = 1.8f;
    float maxClimb = 0.4f;
    float maxSlope = 45.0f;
};

struct BuildSettings {
    AgentConfig agent;
    float cellSize = 0.15f;   // the horizontal voxel size: about a third to a half of the agent radius
    float cellHeight = 0.1f;  // the vertical voxel size; heights on the mesh are this close to the surface
    int tileSize = 64;        // cells per tile side, 16 to 255
    float maxError = 1.3f;    // how far polygon edges may leave the voxels' outline, in cells
    int maxLayersPerTile = 8; // floors above one another in one tile; more are dropped with a warning
    // Area types by id; empty: one type, "default" at cost 1. At most kMaxAreas.
    std::vector<AreaType> areas;
    // The tile grid's extent in x and z; empty (not max > min in x and z): the geometry's bounds. Heights come from
    // the triangles in each tile. Geometry that NavMesh::rebuild() adds outside the grid is not covered.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// An axis-aligned box whose walkable surface gets another area type (mud, shallow water) or, with kNotWalkable, is cut
// out of the mesh.
struct AreaBox {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    AreaId area = kDefaultArea;
};

// An off-mesh link: a way the surface does not have (a jump down a ledge, a ladder, a teleporter). Agents enter it
// within `radius` of `start`; one way unless bidirectional. Both ends must lie on the mesh (within the agent's
// maxClimb above or below it). `id` is the game's: paths and crowds report it.
struct Link {
    glm::vec3 start{0.0f};
    glm::vec3 end{0.0f};
    float radius = 0.5f;
    bool bidirectional = false;
    AreaId area = kDefaultArea;
    std::uint32_t id = 0;
};

// The level as navigation sees it.
struct Geometry {
    std::vector<glm::vec3> vertices;
    std::vector<std::uint32_t> indices; // triangle list
    std::vector<AreaId> areas;          // one per triangle; empty: kDefaultArea everywhere
    std::vector<AreaBox> boxes;         // applied in order, after the triangles
    std::vector<Link> links;
};

// Cooked navigation data: every tile's voxel layers (before obstacles), with the settings and links that go with them.
// Made by buildNavMeshData() or read from a .qnav file; NavMesh::create() makes a mesh of it.
struct NavMeshData {
    BuildSettings settings; // areas resolved, bounds: the tile grid's
    std::vector<Link> links;
    int tilesX = 0;
    int tilesZ = 0;
    struct Layer {
        std::int32_t tileX = 0;
        std::int32_t tileZ = 0;
        std::int32_t index = 0;          // the floor in its tile
        std::vector<std::uint8_t> bytes; // DetourTileCache's layer format, uncompressed
    };
    std::vector<Layer> layers; // by tile (z, then x), then index

    std::uint64_t memoryBytes() const;
};

// Voxelizes `geometry` tile by tile on the job system and waits for it: for tools, tests and loading screens, not
// inside a frame (NavMesh::build does the same across frames).
Result<NavMeshData> buildNavMeshData(const Geometry& geometry, const BuildSettings& settings, JobSystem& jobs);

// The .qnav file format (little endian, versioned, with a checksum).
std::vector<std::uint8_t> writeNavMeshData(const NavMeshData& data);
Result<std::shared_ptr<const NavMeshData>> readNavMeshData(std::span<const std::uint8_t> file);

enum class ObstacleShape : std::uint8_t { Cylinder, Box };

// A dynamic obstacle: it cuts the walkable cells under it, grown by the agent radius, whose floor lies between its
// bottom (less the agent's maxClimb) and its top. With an area other than kNotWalkable it marks them instead (a fire:
// a costly area).
struct Obstacle {
    ObstacleShape shape = ObstacleShape::Cylinder;
    glm::vec3 position{0.0f};    // cylinder: the center of its base; box: its center
    float radius = 0.5f;         // cylinder
    float height = 2.0f;         // cylinder
    glm::vec3 halfExtents{0.5f}; // box
    float yaw = 0.0f;            // box: its turn about +Y, radians
    AreaId area = kNotWalkable;
};

struct ObstacleId {
    std::uint32_t index = ~0u;
    std::uint32_t generation = 0;

    bool valid() const { return index != ~0u; }
    bool operator==(const ObstacleId&) const = default;
};

struct NavMeshStats {
    int tilesX = 0;
    int tilesZ = 0;
    int tiles = 0; // mesh tiles (layers with polygons)
    int polygons = 0;
    int pendingTiles = 0; // tile columns queued or building
    int obstacles = 0;
    std::uint64_t memoryBytes = 0; // layers and mesh tiles
    // Wall time of the last batch of tile builds, from its first request to its last tile added (in update()).
    double lastBuildSeconds = 0.0;
};

namespace detail {
struct NavMeshAccess;
}

class NavMesh {
public:
    // Starts building every tile of `geometry` on the job system: the mesh fills in as update() adds them.
    static Result<NavMesh> build(Geometry geometry, const BuildSettings& settings, JobSystem& jobs);
    // Starts making the tiles of cooked data (no obstacles yet).
    static Result<NavMesh> create(std::shared_ptr<const NavMeshData> data, JobSystem& jobs);

    NavMesh(NavMesh&&) noexcept;
    NavMesh& operator=(NavMesh&&) noexcept;
    // Running tile builds finish on their own and are dropped.
    ~NavMesh();

    // Once per frame: adds the finished tiles and starts the builds that changes asked for. Without worker threads it
    // also runs queued jobs for up to `jobSeconds`. Never waits.
    void update(double jobSeconds = 0.002);
    // Every tile is built and no change waits for a rebuild.
    bool ready() const;
    // Builds until ready(), waiting for the jobs: tools, tests and loading screens.
    void finish();

    ObstacleId addObstacle(const Obstacle& obstacle);
    // Moves or changes an obstacle: the tiles it leaves and the tiles it enters are rebuilt.
    void setObstacle(ObstacleId id, const Obstacle& obstacle);
    void removeObstacle(ObstacleId id);
    // Null for an unknown or removed id.
    const Obstacle* obstacle(ObstacleId id) const;

    // The level changed inside [min, max]: the tiles that touch the box (grown by the agent radius) are voxelized again
    // from `geometry` (the whole level or the part around the box; its links are ignored: they are the mesh's).
    Result<void> rebuild(Geometry geometry, glm::vec3 min, glm::vec3 max);

    const BuildSettings& settings() const;
    std::span<const Link> links() const;
    NavMeshStats stats() const;
    // Bumped whenever update() changes a tile (for redrawing debug geometry, for example).
    std::uint64_t revision() const;
    // A hash of every tile's data as built (before tiles are joined): equal for equal inputs and settings, whatever
    // the number of threads.
    std::uint64_t contentHash() const;
    // The current layers (obstacles not included) as cooked data, for saving a run-time build.
    NavMeshData data() const;

private:
    friend struct detail::NavMeshAccess;
    struct Impl;
    explicit NavMesh(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace quench::navigation
