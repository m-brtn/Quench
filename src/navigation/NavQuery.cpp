#include <quench/navigation/NavQuery.h>

#include "Internal.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cfloat>
#include <format>

namespace quench::navigation {
namespace {

struct QueryFree {
    void operator()(dtNavMeshQuery* query) const { dtFreeNavMeshQuery(query); }
};

constexpr int kMaxPathPolygons = 4096;

// Detour draws random numbers through a plain function: it reads the seed of the query running on this thread.
thread_local std::uint64_t* t_seed = nullptr;

float nextRandom() {
    std::uint64_t z = (*t_seed += 0x9E3779B97F4A7C15ull); // SplitMix64
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return float(z >> 40) * (1.0f / 16777216.0f);
}

glm::vec3 vec(const float* v) { return {v[0], v[1], v[2]}; }

} // namespace

Filter Filter::of(const NavMesh& mesh) {
    Filter filter;
    const std::vector<AreaType>& areas = mesh.settings().areas;
    for (std::size_t i = 0; i < areas.size() && i < filter.costs.size(); ++i) filter.costs[i] = areas[i].cost;
    return filter;
}

float Path::length() const {
    float total = 0.0f;
    for (std::size_t i = 1; i < points.size(); ++i) total += glm::distance(points[i - 1].position, points[i].position);
    return total;
}

struct NavQuery::Impl {
    const dtNavMesh* mesh = nullptr;
    std::unique_ptr<dtNavMeshQuery, QueryFree> query;
    dtQueryFilter defaultFilter;

    dtQueryFilter filter(const Filter* f) const { return f ? detail::toDetour(*f) : defaultFilter; }

    AreaId area(dtPolyRef ref) const {
        unsigned char a = 0;
        mesh->getPolyArea(ref, &a);
        return AreaId(a);
    }
};

NavQuery::NavQuery(const NavMesh& mesh, int maxNodes) : m_impl(std::make_unique<Impl>()) {
    m_impl->mesh = detail::NavMeshAccess::mesh(mesh);
    m_impl->query.reset(dtAllocNavMeshQuery());
    // Detour's node pool indexes nodes with 16 bits. init fails only when memory runs out.
    const int nodes = std::clamp(maxNodes, 16, 65534);
    const bool ready = m_impl->query && !dtStatusFailed(m_impl->query->init(m_impl->mesh, nodes));
    QUENCH_ASSERT(ready, "dtNavMeshQuery::init failed");
    (void)ready;
    m_impl->defaultFilter = detail::toDetour(Filter::of(mesh));
}

NavQuery::NavQuery(NavQuery&&) noexcept = default;
NavQuery& NavQuery::operator=(NavQuery&&) noexcept = default;
NavQuery::~NavQuery() = default;

Result<NavPoint> NavQuery::nearestPoint(glm::vec3 position, const Filter* filter) {
    const dtQueryFilter f = m_impl->filter(filter);
    dtPolyRef ref = 0;
    float point[3];
    if (dtStatusFailed(m_impl->query->findNearestPoly(&position.x, &m_extents.x, &f, &ref, point)) || ref == 0)
        return Error{std::format("no navmesh within ({}, {}, {}) of ({}, {}, {})", m_extents.x, m_extents.y,
                                 m_extents.z, position.x, position.y, position.z)};
    return NavPoint{vec(point), ref, m_impl->area(ref)};
}

Result<Path> NavQuery::findPath(glm::vec3 start, glm::vec3 end, const Filter* filter) {
    Result<NavPoint> from = nearestPoint(start, filter);
    if (!from) return Error{std::format("path start: {}", from.error())};
    Result<NavPoint> to = nearestPoint(end, filter);
    if (!to) return Error{std::format("path end: {}", to.error())};
    const dtQueryFilter f = m_impl->filter(filter);
    dtNavMeshQuery& q = *m_impl->query;

    Path path;
    path.corridor.resize(kMaxPathPolygons);
    int count = 0;
    const dtStatus status = q.findPath(from->polygon, to->polygon, &from->position.x, &to->position.x, &f,
                                       path.corridor.data(), &count, kMaxPathPolygons);
    if (dtStatusFailed(status) || count == 0) return Error{std::format("no path (status {:#x})", status)};
    path.corridor.resize(std::size_t(count));
    glm::vec3 target = to->position;
    if (path.corridor.back() != to->polygon || dtStatusDetail(status, DT_PARTIAL_RESULT)) {
        path.partial = true;
        float closest[3];
        if (!dtStatusFailed(q.closestPointOnPoly(path.corridor.back(), &to->position.x, closest, nullptr)))
            target = vec(closest);
    }

    const int maxPoints = count * 2 + 4;
    const std::size_t pointSlots = std::size_t(maxPoints);
    std::vector<float> points(pointSlots * 3);
    std::vector<unsigned char> flags(pointSlots);
    std::vector<dtPolyRef> refs(pointSlots);
    int pointCount = 0;
    if (dtStatusFailed(q.findStraightPath(&from->position.x, &target.x, path.corridor.data(), count, points.data(),
                                          flags.data(), refs.data(), &pointCount, maxPoints, 0)))
        return Error{"no straight path through the corridor"};
    for (int i = 0; i < pointCount; ++i) {
        PathPoint p;
        p.position = vec(&points[std::size_t(i) * 3]);
        p.polygon = refs[std::size_t(i)];
        if (flags[std::size_t(i)] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) {
            p.linkStart = true;
            if (const dtOffMeshConnection* link = m_impl->mesh->getOffMeshConnectionByRef(p.polygon))
                p.link = link->userId;
        }
        path.points.push_back(p);
    }
    return path;
}

Result<RaycastHit> NavQuery::raycast(glm::vec3 start, glm::vec3 end, const Filter* filter) {
    Result<NavPoint> from = nearestPoint(start, filter);
    if (!from) return Error{std::format("raycast start: {}", from.error())};
    const dtQueryFilter f = m_impl->filter(filter);
    dtNavMeshQuery& q = *m_impl->query;
    std::vector<dtPolyRef> polygons(256);
    dtRaycastHit hit{};
    hit.path = polygons.data();
    hit.maxPath = int(polygons.size());
    if (dtStatusFailed(q.raycast(from->polygon, &from->position.x, &end.x, &f, 0, &hit, 0)))
        return Error{"raycast failed"};
    RaycastHit out;
    out.hit = hit.t <= 1.0f;
    out.t = out.hit ? hit.t : 1.0f;
    out.normal = vec(hit.hitNormal);
    out.polygons.assign(polygons.begin(), polygons.begin() + hit.pathCount);
    glm::vec3 position = from->position + (end - from->position) * out.t;
    if (!out.polygons.empty()) {
        float onMesh[3];
        if (!dtStatusFailed(q.closestPointOnPoly(out.polygons.back(), &position.x, onMesh, nullptr)))
            position = vec(onMesh);
    }
    out.position = position;
    return out;
}

Result<NavPoint> NavQuery::randomPoint(std::uint64_t& seed, const Filter* filter) {
    const dtQueryFilter f = m_impl->filter(filter);
    dtPolyRef ref = 0;
    float point[3];
    t_seed = &seed;
    const dtStatus status = m_impl->query->findRandomPoint(&f, nextRandom, &ref, point);
    t_seed = nullptr;
    if (dtStatusFailed(status) || ref == 0) return Error{"no navmesh to pick a point on"};
    return NavPoint{vec(point), ref, m_impl->area(ref)};
}

Result<NavPoint> NavQuery::randomPointAround(glm::vec3 center, float radius, std::uint64_t& seed,
                                             const Filter* filter) {
    Result<NavPoint> from = nearestPoint(center, filter);
    if (!from) return from;
    const dtQueryFilter f = m_impl->filter(filter);
    dtPolyRef ref = 0;
    float point[3];
    t_seed = &seed;
    const dtStatus status = m_impl->query->findRandomPointAroundCircle(from->polygon, &from->position.x, radius, &f,
                                                                       nextRandom, &ref, point);
    t_seed = nullptr;
    if (dtStatusFailed(status) || ref == 0) return Error{"no navmesh around the point"};
    return NavPoint{vec(point), ref, m_impl->area(ref)};
}

} // namespace quench::navigation
