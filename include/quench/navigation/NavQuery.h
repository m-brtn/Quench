#pragma once

// Queries on a NavMesh (Detour's dtNavMeshQuery underneath): the nearest point on the mesh, paths (the corridor of
// polygons and the straightened path through it), raycasts along the surface and random points. A NavQuery keeps
// search memory, so it is used by one thread at a time (one per job for queries in parallel, between two
// NavMesh::update() calls).

#include <quench/core/Result.h>
#include <quench/navigation/NavMesh.h>

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace quench::navigation {

// Which polygons a search may cross and what each area type costs. Queries without a filter use the mesh's area
// costs and every area.
struct Filter {
    std::array<float, kMaxAreas> costs{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                       1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    std::uint16_t includeAreas = 0xffff; // a bit per area id
    std::uint16_t excludeAreas = 0;

    // The mesh's area costs, every area included.
    static Filter of(const NavMesh& mesh);
    Filter& exclude(AreaId area) {
        excludeAreas = std::uint16_t(excludeAreas | (1u << area));
        return *this;
    }
};

// A point on the mesh and its polygon.
struct NavPoint {
    glm::vec3 position{0.0f};
    std::uint64_t polygon = 0;
    AreaId area = kDefaultArea;
};

struct PathPoint {
    glm::vec3 position{0.0f};
    std::uint64_t polygon = 0;
    // The point where a link starts (the next point is its end): its Link::id.
    bool linkStart = false;
    std::uint32_t link = 0;
};

struct Path {
    std::vector<std::uint64_t> corridor; // the polygons from start to end
    std::vector<PathPoint> points;       // the straightened path: start, corners, links, end
    // The end was not reachable: the path ends at the reachable point nearest to it.
    bool partial = false;

    float length() const;
};

struct RaycastHit {
    bool hit = false;                    // a wall (or a polygon the filter excludes) stopped the ray
    float t = 1.0f;                      // where along start..end it stopped (1 when it did not)
    glm::vec3 position{0.0f};            // the point on the mesh where it stopped
    glm::vec3 normal{0.0f};              // the wall's normal (horizontal) on a hit
    std::vector<std::uint64_t> polygons; // the polygons crossed
};

class NavQuery {
public:
    // maxNodes bounds a path search: longer searches return partial paths.
    explicit NavQuery(const NavMesh& mesh, int maxNodes = 4096);
    NavQuery(NavQuery&&) noexcept;
    NavQuery& operator=(NavQuery&&) noexcept;
    ~NavQuery();

    // How far around a position the start and end of a query are looked for on the mesh (half sizes).
    void setSearchExtents(glm::vec3 halfExtents) { m_extents = halfExtents; }
    glm::vec3 searchExtents() const { return m_extents; }

    // The point on the mesh nearest to `position` within the search extents.
    Result<NavPoint> nearestPoint(glm::vec3 position, const Filter* filter = nullptr);
    // The cheapest path between the points of the mesh nearest to start and end. An unreachable end gives a partial
    // path; no mesh near start or end is an error.
    Result<Path> findPath(glm::vec3 start, glm::vec3 end, const Filter* filter = nullptr);
    // Walks from start toward end along the surface (heights ignored) until a wall stops it.
    Result<RaycastHit> raycast(glm::vec3 start, glm::vec3 end, const Filter* filter = nullptr);
    // A random point on the mesh, uniform by area; `seed` advances, so a seed repeats the same points.
    Result<NavPoint> randomPoint(std::uint64_t& seed, const Filter* filter = nullptr);
    // A random point reachable from `center` within about `radius`.
    Result<NavPoint> randomPointAround(glm::vec3 center, float radius, std::uint64_t& seed,
                                       const Filter* filter = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    glm::vec3 m_extents{2.0f, 4.0f, 2.0f};
};

} // namespace quench::navigation
