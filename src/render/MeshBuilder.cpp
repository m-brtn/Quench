#include <quench/render/MeshBuilder.h>

#include <quench/assets/Tangents.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <numbers>

namespace quench::render {
namespace {

constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

// A grid of (rows + 1) x (columns + 1) vertices made into quads, counter-clockwise seen from the side that
// cross(step to the next row, step to the next column) points to.
void addGridIndices(MeshData& mesh, std::uint32_t first, int rows, int columns) {
    const auto at = [first, columns](int r, int c) { return first + std::uint32_t(r * (columns + 1) + c); };
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            mesh.indices.insert(mesh.indices.end(),
                                {at(r, c), at(r + 1, c), at(r, c + 1), at(r, c + 1), at(r + 1, c), at(r + 1, c + 1)});
}

// The vertex tangent from the surface's direction of +u and of -v (the normal map's +y), as MikkTSpace gives it.
glm::vec4 tangentOf(const glm::vec3& n, const glm::vec3& alongU, const glm::vec3& towardMinusV) {
    const glm::vec3 t = glm::normalize(alongU - n * glm::dot(n, alongU));
    return glm::vec4(t, glm::dot(glm::cross(n, t), towardMinusV) < 0.0f ? -1.0f : 1.0f);
}

} // namespace

MeshData boxMesh(const glm::vec3& halfExtents) {
    MeshData mesh;
    // Per face: normal, u axis, v axis (u x v = normal for counter-clockwise faces seen from outside).
    struct Face {
        glm::vec3 n, u, v;
    };
    const Face faces[] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const Face& f : faces) {
        const auto first = std::uint32_t(mesh.vertices.size());
        // The texture's u runs along f.u and its v against f.v.
        const glm::vec4 tangent(f.u, 1.0f);
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const float su = float(i) * 2.0f - 1.0f, sv = float(j) * 2.0f - 1.0f;
                const glm::vec3 p = (f.n + f.u * su + f.v * sv) * halfExtents;
                mesh.vertices.push_back({p, f.n, {float(i), 1.0f - float(j)}, glm::vec4(1.0f), tangent});
            }
        mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first + 2, first + 1, first + 3});
    }
    return mesh;
}

MeshData sphereMesh(float radius, int rings, int segments) {
    MeshData mesh;
    for (int r = 0; r <= rings; ++r) {
        const float v = float(r) / float(rings);
        const float theta = v * std::numbers::pi_v<float>; // from the top
        // Exact poles: in float, sin(pi) is not 0, and the pole triangles would get a random orientation.
        const bool pole = r == 0 || r == rings;
        const float sinTheta = pole ? 0.0f : std::sin(theta),
                    cosTheta = pole ? (r == 0 ? 1.0f : -1.0f) : std::cos(theta);
        for (int s = 0; s <= segments; ++s) {
            const float u = float(s) / float(segments);
            const float phi = u * kTau;
            const glm::vec3 n(sinTheta * std::sin(phi), cosTheta, sinTheta * std::cos(phi));
            // +u: eastward; -v: toward the top.
            const glm::vec3 east(std::cos(phi), 0.0f, -std::sin(phi));
            const glm::vec3 north(-cosTheta * std::sin(phi), sinTheta, -cosTheta * std::cos(phi));
            mesh.vertices.push_back({n * radius, n, {u, v}, glm::vec4(1.0f), tangentOf(n, east, north)});
        }
    }
    addGridIndices(mesh, 0, rings, segments);
    return mesh;
}

MeshData planeMesh(const glm::vec2& halfSize, float uvScale) {
    MeshData mesh;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const float x = (float(i) * 2.0f - 1.0f) * halfSize.x, z = (float(j) * 2.0f - 1.0f) * halfSize.y;
            mesh.vertices.push_back({{x, 0.0f, z},
                                     up,
                                     {float(i) * uvScale, float(j) * uvScale},
                                     glm::vec4(1.0f),
                                     glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)});
        }
    mesh.indices = {0, 2, 1, 1, 2, 3};
    return mesh;
}

MeshData torusMesh(float ringRadius, float tubeRadius, int rings, int sides) {
    MeshData mesh;
    for (int i = 0; i <= rings; ++i) {
        const float u = float(i) / float(rings);
        const float theta = u * kTau;
        for (int j = 0; j <= sides; ++j) {
            const float v = float(j) / float(sides);
            const float phi = v * kTau;
            const glm::vec3 n(std::cos(phi) * std::cos(theta), std::sin(phi), -std::cos(phi) * std::sin(theta));
            const glm::vec3 center(ringRadius * std::cos(theta), 0.0f, -ringRadius * std::sin(theta));
            // +u: along the ring; -v: back around the tube.
            const glm::vec3 alongRing(-std::sin(theta), 0.0f, -std::cos(theta));
            const glm::vec3 aroundTube(std::sin(phi) * std::cos(theta), -std::cos(phi),
                                       -std::sin(phi) * std::sin(theta));
            mesh.vertices.push_back(
                {center + n * tubeRadius, n, {u, v}, glm::vec4(1.0f), tangentOf(n, alongRing, aroundTube)});
        }
    }
    addGridIndices(mesh, 0, rings, sides);
    return mesh;
}

MeshData diskMesh(float radius, int segments) {
    MeshData mesh;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
    mesh.vertices.push_back({glm::vec3(0.0f), up, {0.5f, 0.5f}, glm::vec4(1.0f), tangent});
    for (int i = 0; i < segments; ++i) {
        const float a = kTau * float(i) / float(segments);
        const float x = std::cos(a), z = -std::sin(a);
        mesh.vertices.push_back(
            {{x * radius, 0.0f, z * radius}, up, {x * 0.5f + 0.5f, z * 0.5f + 0.5f}, glm::vec4(1.0f), tangent});
    }
    for (int i = 0; i < segments; ++i)
        mesh.indices.insert(mesh.indices.end(), {0u, 1u + std::uint32_t(i), 1u + std::uint32_t((i + 1) % segments)});
    return mesh;
}

MeshData cylinderMesh(float radius, float halfHeight, int segments) {
    MeshData mesh;
    for (int r = 0; r <= 1; ++r)
        for (int c = 0; c <= segments; ++c) {
            const float u = float(c) / float(segments);
            const float a = u * kTau;
            const glm::vec3 n(std::sin(a), 0.0f, std::cos(a));
            mesh.vertices.push_back({n * radius + glm::vec3(0.0f, r == 0 ? halfHeight : -halfHeight, 0.0f),
                                     n,
                                     {u, float(r)},
                                     glm::vec4(1.0f),
                                     tangentOf(n, glm::vec3(std::cos(a), 0.0f, -std::sin(a)), glm::vec3(0, 1, 0))});
        }
    addGridIndices(mesh, 0, 1, segments);
    return mesh;
}

void computeTangents(MeshData& mesh) {
    static_assert(sizeof(assets::ModelVertex) == sizeof(Vertex));
    std::vector<assets::ModelVertex> vertices(mesh.vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vertex& v = mesh.vertices[i];
        vertices[i] = {v.position, v.normal, v.uv, v.color, v.tangent};
    }
    assets::generateTangents(vertices, mesh.indices);
    mesh.vertices.resize(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const assets::ModelVertex& v = vertices[i];
        mesh.vertices[i] = {v.position, v.normal, v.uv, v.color, v.tangent};
    }
}

} // namespace quench::render
