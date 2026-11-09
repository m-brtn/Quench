#include <quench/navigation/DebugGeometry.h>

#include "Internal.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/vec2.hpp>

#include <cmath>
#include <utility>

namespace quench::navigation {
namespace {

glm::vec3 vec(const float* v) { return {v[0], v[1], v[2]}; }

void line(DebugGeometry& out, glm::vec3 a, glm::vec3 b, glm::vec4 color) {
    out.lines.push_back({a, color});
    out.lines.push_back({b, color});
}

void circle(DebugGeometry& out, glm::vec3 center, float radius, glm::vec4 color, int segments = 16) {
    for (int i = 0; i < segments; ++i) {
        const float a0 = glm::two_pi<float>() * float(i) / float(segments);
        const float a1 = glm::two_pi<float>() * float(i + 1) / float(segments);
        line(out, center + radius * glm::vec3(std::cos(a0), 0.0f, std::sin(a0)),
             center + radius * glm::vec3(std::cos(a1), 0.0f, std::sin(a1)), color);
    }
}

// p within a centimeter of the segment ab, in x and z.
bool onSegment2D(glm::vec3 p, glm::vec3 a, glm::vec3 b) {
    const glm::vec2 ab(b.x - a.x, b.z - a.z), ap(p.x - a.x, p.z - a.z);
    const float length2 = glm::dot(ab, ab);
    const float t = length2 > 0.0f ? glm::clamp(glm::dot(ap, ab) / length2, 0.0f, 1.0f) : 0.0f;
    const glm::vec2 d = ap - ab * t;
    return glm::dot(d, d) < 0.01f * 0.01f;
}

glm::vec3 hue(float h) {
    const glm::vec3 k = glm::abs(glm::fract(glm::vec3(h) + glm::vec3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f);
    return glm::clamp(k - 1.0f, 0.0f, 1.0f);
}

} // namespace

glm::vec4 areaColor(AreaId area) {
    static const glm::vec3 kColors[8] = {
        {0.05f, 0.55f, 1.0f}, {0.45f, 0.25f, 0.08f}, {0.1f, 0.85f, 0.75f}, {1.0f, 0.55f, 0.0f},
        {0.3f, 0.9f, 0.2f},   {0.65f, 0.3f, 1.0f},   {1.0f, 0.9f, 0.15f},  {1.0f, 0.35f, 0.65f},
    };
    if (area == kNotWalkable) return {1.0f, 0.05f, 0.05f, 0.5f};
    const glm::vec3 c = area < 8 ? kColors[area] : hue(float(area) * 0.618034f);
    return {c, 0.5f};
}

void addNavMesh(DebugGeometry& out, const NavMesh& mesh, float lift) {
    const dtNavMesh* nav = detail::NavMeshAccess::mesh(mesh);
    const glm::vec3 up(0.0f, lift, 0.0f);
    for (int tileIndex = 0; tileIndex < nav->getMaxTiles(); ++tileIndex) {
        const dtMeshTile* tile = nav->getTile(tileIndex);
        if (!tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly& poly = tile->polys[p];
            const glm::vec4 color = areaColor(poly.getArea());
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            // The detail triangles: the polygon at the surface's heights (a polygon over stairs is a flat slope).
            const dtPolyDetail& detail = tile->detailMeshes[p];
            const auto vertex = [&](unsigned char i) {
                return i < poly.vertCount ? vec(&tile->verts[poly.verts[i] * 3])
                                          : vec(&tile->detailVerts[(detail.vertBase + i - poly.vertCount) * 3]);
            };
            for (unsigned k = 0; k < detail.triCount; ++k) {
                const unsigned char* t = &tile->detailTris[(detail.triBase + k) * 4];
                const glm::vec3 a = vertex(t[0]) + up;
                glm::vec3 b = vertex(t[1]) + up, c = vertex(t[2]) + up;
                if (glm::cross(b - a, c - a).y < 0.0f) std::swap(b, c);
                out.triangles.push_back({a, color});
                out.triangles.push_back({b, color});
                out.triangles.push_back({c, color});
            }
            // The polygon's edges along the detail triangles' boundary edges on them. Inner edges are drawn by both
            // polygons; walls once.
            for (int e = 0; e < poly.vertCount; ++e) {
                const glm::vec4 edgeColor = poly.neis[e] == 0 ? glm::vec4(0.0f, 0.05f, 0.15f, 0.9f)
                                                              : glm::vec4(color.x, color.y, color.z, 0.35f);
                const glm::vec3 e0 = vec(&tile->verts[poly.verts[e] * 3]);
                const glm::vec3 e1 = vec(&tile->verts[poly.verts[(e + 1) % poly.vertCount] * 3]);
                for (unsigned k = 0; k < detail.triCount; ++k) {
                    const unsigned char* t = &tile->detailTris[(detail.triBase + k) * 4];
                    for (int m = 0; m < 3; ++m) {
                        if (!(dtGetDetailTriEdgeFlags(t[3], m) & DT_DETAIL_EDGE_BOUNDARY)) continue;
                        const glm::vec3 a = vertex(t[m]), b = vertex(t[(m + 1) % 3]);
                        if (onSegment2D(a, e0, e1) && onSegment2D(b, e0, e1)) line(out, a + up, b + up, edgeColor);
                    }
                }
            }
        }
        for (int c = 0; c < tile->header->offMeshConCount; ++c) {
            const dtOffMeshConnection& link = tile->offMeshCons[c];
            const dtPoly& poly = tile->polys[link.poly];
            const glm::vec4 color(glm::vec3(areaColor(poly.getArea())) * 0.5f + 0.5f, 1.0f);
            const glm::vec3 a = vec(&link.pos[0]) + up, b = vec(&link.pos[3]) + up;
            const float height = 0.25f * glm::distance(a, b) + 0.2f;
            const glm::vec3 mid = (a + b) * 0.5f + glm::vec3(0.0f, height, 0.0f);
            glm::vec3 previous = a;
            for (int k = 1; k <= 12; ++k) {
                const float s = float(k) / 12.0f;
                const glm::vec3 p = (1 - s) * (1 - s) * a + 2 * (1 - s) * s * mid + s * s * b;
                line(out, previous, p, color);
                previous = p;
            }
            circle(out, a, link.rad, color, 12);
            if (link.flags & DT_OFFMESH_CON_BIDIR) circle(out, b, link.rad, color, 12);
        }
    }
}

void addObstacles(DebugGeometry& out, const NavMesh& mesh, glm::vec4 color) {
    for (const Obstacle& o : detail::NavMeshAccess::obstacles(mesh)) {
        if (o.shape == ObstacleShape::Cylinder) {
            const glm::vec3 top = o.position + glm::vec3(0.0f, o.height, 0.0f);
            circle(out, o.position, o.radius, color);
            circle(out, top, o.radius, color);
            for (int k = 0; k < 4; ++k) {
                const float a = glm::half_pi<float>() * float(k);
                const glm::vec3 d = o.radius * glm::vec3(std::cos(a), 0.0f, std::sin(a));
                line(out, o.position + d, top + d, color);
            }
            continue;
        }
        const glm::vec3 x = glm::vec3(std::cos(o.yaw), 0.0f, -std::sin(o.yaw)) * o.halfExtents.x;
        const glm::vec3 z = glm::vec3(std::sin(o.yaw), 0.0f, std::cos(o.yaw)) * o.halfExtents.z;
        const glm::vec3 y(0.0f, o.halfExtents.y, 0.0f);
        const glm::vec3 corners[4] = {o.position - x - z, o.position + x - z, o.position + x + z, o.position - x + z};
        for (int k = 0; k < 4; ++k) {
            const glm::vec3 a = corners[k], b = corners[(k + 1) % 4];
            line(out, a - y, b - y, color);
            line(out, a + y, b + y, color);
            line(out, a - y, a + y, color);
        }
    }
}

void addPath(DebugGeometry& out, std::span<const glm::vec3> points, glm::vec4 color, float lift) {
    const glm::vec3 up(0.0f, lift, 0.0f);
    for (std::size_t i = 1; i < points.size(); ++i) line(out, points[i - 1] + up, points[i] + up, color);
}

void addPath(DebugGeometry& out, const Path& path, glm::vec4 color, float lift) {
    std::vector<glm::vec3> points;
    points.reserve(path.points.size());
    for (const PathPoint& p : path.points) points.push_back(p.position);
    addPath(out, points, color, lift);
}

void addAgents(DebugGeometry& out, const Crowd& crowd, glm::vec4 color, float lift) {
    addAgents(out, crowd, crowd.agents(), color, lift);
}

void addAgents(DebugGeometry& out, const Crowd& crowd, std::span<const AgentId> agents, glm::vec4 color, float lift) {
    const glm::vec3 up(0.0f, lift, 0.0f);
    for (const AgentId id : agents) {
        if (!crowd.contains(id)) continue;
        const AgentState s = crowd.state(id);
        const AgentParams* params = crowd.params(id);
        circle(out, s.position + up, params ? params->radius : 0.4f, color);
        line(out, s.position + up, s.position + up + s.velocity * 0.5f, color);
    }
}

} // namespace quench::navigation
