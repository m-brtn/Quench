#pragma once

// Debug geometry of navigation as plain lists, for any renderer: the mesh's polygons colored by area type with their
// outlines and links, obstacles, paths and agents. Colors are linear RGBA (alpha for blending).

#include <quench/navigation/Crowd.h>
#include <quench/navigation/NavMesh.h>
#include <quench/navigation/NavQuery.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <span>
#include <vector>

namespace quench::navigation {

struct DebugVertex {
    glm::vec3 position{0.0f};
    glm::vec4 color{1.0f};
};

struct DebugGeometry {
    std::vector<DebugVertex> triangles; // three vertices per triangle, counter-clockwise seen from above
    std::vector<DebugVertex> lines;     // two vertices per line

    void clear() {
        triangles.clear();
        lines.clear();
    }
};

// A color per area type: distinct for the first eight ids, red for kNotWalkable.
glm::vec4 areaColor(AreaId area);

// The polygons as triangles in their area's color (their detail triangles, at the surface's heights), their walls
// (edges without a neighbor) and inner edges as lines, links as arcs from start to end (a circle of their radius at
// the start), all raised by `lift` meters.
void addNavMesh(DebugGeometry& out, const NavMesh& mesh, float lift = 0.05f);
// Obstacle outlines (grown by nothing: the shapes as given).
void addObstacles(DebugGeometry& out, const NavMesh& mesh, glm::vec4 color);
// A polyline through points.
void addPath(DebugGeometry& out, std::span<const glm::vec3> points, glm::vec4 color, float lift = 0.1f);
void addPath(DebugGeometry& out, const Path& path, glm::vec4 color, float lift = 0.1f);
// Each agent's radius as a circle at its feet and its velocity as a line: every agent of the crowd, or some of them.
void addAgents(DebugGeometry& out, const Crowd& crowd, glm::vec4 color, float lift = 0.05f);
void addAgents(DebugGeometry& out, const Crowd& crowd, std::span<const AgentId> agents, glm::vec4 color,
               float lift = 0.05f);

} // namespace quench::navigation
