#pragma once

// Procedural meshes in the engine's vertex format, for samples, tests and placeholder content. Counter-clockwise
// front faces, outward normals, white vertex color, tangents along +u (the frame computeTangents gives them).

#include <quench/render/Scene.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace quench::render {

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    MeshDesc desc(std::string_view name = {}) const { return {vertices, indices, name}; }
};

// Centered on the origin; uv 0..1 on each face.
MeshData boxMesh(const glm::vec3& halfExtents);
// UV sphere: u around the y axis, v from the top.
MeshData sphereMesh(float radius, int rings = 24, int segments = 48);
// In the xz plane facing +y; uv repeats uvScale times over the plane.
MeshData planeMesh(const glm::vec2& halfSize, float uvScale = 1.0f);
// Around the y axis.
MeshData torusMesh(float ringRadius, float tubeRadius, int rings = 64, int sides = 32);
// In the xz plane facing +y; uv 0..1 over the square around it.
MeshData diskMesh(float radius, int segments = 48);
// Around the y axis from -halfHeight to +halfHeight, open at both ends; u around, v from the top.
MeshData cylinderMesh(float radius, float halfHeight, int segments = 48);

// MikkTSpace tangents (assets::generateTangents) for a mesh made elsewhere: from its positions, normals and uvs.
// Vertices at mirrored uv seams may be split; vertices no triangle uses are dropped.
void computeTangents(MeshData& mesh);

} // namespace quench::render
