#include "Test.h"

#include <quench/assets/Tangents.h>

#include <glm/geometric.hpp>

#include <cmath>

using namespace quench;
using namespace quench::assets;

namespace {

// A quad in the xz plane facing +y: u along +x (or -x), v along +z.
void addQuad(std::vector<ModelVertex>& vertices, std::vector<std::uint32_t>& indices, float x0, float u0, float u1) {
    const auto first = std::uint32_t(vertices.size());
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
            vertices.push_back(
                {{x0 + float(i), 0.0f, float(j)}, {0.0f, 1.0f, 0.0f}, {i == 0 ? u0 : u1, float(j)}, glm::vec4(1.0f)});
    indices.insert(indices.end(), {first, first + 2, first + 1, first + 1, first + 2, first + 3});
}

} // namespace

// glTF's convention: the tangent along +u, the bitangent w * cross(normal, tangent) toward -v.
QUENCH_TEST(tangents_follow_the_uv_directions) {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    addQuad(vertices, indices, 0.0f, 0.0f, 1.0f);
    const std::vector<std::uint32_t> from = generateTangents(vertices, indices);
    REQUIRE(vertices.size() == 4 && from.size() == 4 && indices.size() == 6, "%zu vertices", vertices.size());
    for (const ModelVertex& v : vertices) {
        const glm::vec3 bitangent = v.tangent.w * glm::cross(v.normal, glm::vec3(v.tangent));
        CHECK(glm::dot(glm::vec3(v.tangent), glm::vec3(1, 0, 0)) > 0.999f &&
                  glm::dot(bitangent, glm::vec3(0, 0, -1)) > 0.999f,
              "tangent %f %f %f w %f", double(v.tangent.x), double(v.tangent.y), double(v.tangent.z),
              double(v.tangent.w));
    }
}

// Two quads that share an edge, the second with its u mirrored: the shared vertices need two tangents and are split;
// `from` names the original of every vertex.
QUENCH_TEST(tangents_split_mirrored_seams) {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    addQuad(vertices, indices, 0.0f, 0.0f, 1.0f);
    addQuad(vertices, indices, 1.0f, 1.0f, 0.0f);
    // Weld the seam: vertices 4 and 6 of the second quad are 1 and 3 of the first (same position, normal and uv).
    for (std::uint32_t& i : indices) i = i == 4 ? 1 : i == 6 ? 3 : i;
    const std::vector<ModelVertex> original = vertices;
    const std::vector<std::uint32_t> originalIndices = indices;
    const std::vector<std::uint32_t> from = generateTangents(vertices, indices);
    CHECK(vertices.size() == 8, "%zu vertices after the split (6 used, 2 split)", vertices.size());
    REQUIRE(from.size() == vertices.size() && indices.size() == originalIndices.size(), "sizes");
    int wrong = 0;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const ModelVertex& v = vertices[indices[i]];
        if (from[indices[i]] != originalIndices[i] || v.position != original[originalIndices[i]].position) ++wrong;
        // The first quad's u runs along +x, the second's along -x.
        const float expected = i < 6 ? 1.0f : -1.0f;
        if (glm::dot(glm::vec3(v.tangent), glm::vec3(expected, 0, 0)) < 0.999f) ++wrong;
    }
    CHECK(wrong == 0, "%d corners with a wrong source or tangent", wrong);
}

// Without uv area MikkTSpace has no direction: a unit tangent perpendicular to the normal.
QUENCH_TEST(tangents_without_uvs) {
    std::vector<ModelVertex> vertices = {{{0, 0, 0}, {0, 0, 1}}, {{1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}}};
    std::vector<std::uint32_t> indices = {0, 1, 2};
    generateTangents(vertices, indices);
    for (const ModelVertex& v : vertices) {
        const glm::vec3 t(v.tangent);
        CHECK(std::isfinite(t.x + t.y + t.z) && std::abs(glm::length(t) - 1.0f) < 1e-5f &&
                  std::abs(glm::dot(t, v.normal)) < 1e-5f && std::abs(v.tangent.w) == 1.0f,
              "tangent %f %f %f w %f", double(t.x), double(t.y), double(t.z), double(v.tangent.w));
    }
}
