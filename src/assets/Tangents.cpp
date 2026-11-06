#include <quench/assets/Tangents.h>

#include <glm/geometric.hpp>
#include <meshoptimizer.h>
#include <mikktspace.h>

#include <cmath>

namespace quench::assets {
namespace {

// MikkTSpace reads and writes the corners of the triangles (one vertex per index).
std::vector<ModelVertex>& corners(const SMikkTSpaceContext* context) {
    return *static_cast<std::vector<ModelVertex>*>(context->m_pUserData);
}

ModelVertex& corner(const SMikkTSpaceContext* context, int face, int vertex) {
    return corners(context)[std::size_t(face) * 3 + std::size_t(vertex)];
}

int faceCount(const SMikkTSpaceContext* context) { return int(corners(context).size() / 3); }
int verticesOfFace(const SMikkTSpaceContext*, int) { return 3; }

void position(const SMikkTSpaceContext* context, float out[], int face, int vertex) {
    const glm::vec3& p = corner(context, face, vertex).position;
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

void normal(const SMikkTSpaceContext* context, float out[], int face, int vertex) {
    const glm::vec3& n = corner(context, face, vertex).normal;
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}

void texCoord(const SMikkTSpaceContext* context, float out[], int face, int vertex) {
    const glm::vec2& uv = corner(context, face, vertex).uv;
    out[0] = uv.x;
    out[1] = uv.y;
}

// MikkTSpace's bitangent, sign * cross(normal, tangent), points toward +v; glTF's toward -v (the image's up).
void setTangent(const SMikkTSpaceContext* context, const float tangent[], float sign, int face, int vertex) {
    corner(context, face, vertex).tangent = glm::vec4(tangent[0], tangent[1], tangent[2], sign < 0.0f ? 1.0f : -1.0f);
}

glm::vec3 perpendicular(const glm::vec3& n) {
    return glm::normalize(std::abs(n.y) < 0.99f ? glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), n)
                                                : glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), n));
}

} // namespace

std::vector<std::uint32_t> generateTangents(std::vector<ModelVertex>& vertices, std::vector<std::uint32_t>& indices) {
    std::vector<ModelVertex> expanded(indices.size());
    std::vector<std::uint32_t> source(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        expanded[i] = vertices[indices[i]];
        expanded[i].tangent = glm::vec4(0.0f);
        source[i] = indices[i];
    }
    if (indices.size() >= 3) {
        SMikkTSpaceInterface callbacks{};
        callbacks.m_getNumFaces = faceCount;
        callbacks.m_getNumVerticesOfFace = verticesOfFace;
        callbacks.m_getPosition = position;
        callbacks.m_getNormal = normal;
        callbacks.m_getTexCoord = texCoord;
        callbacks.m_setTSpaceBasic = setTangent;
        SMikkTSpaceContext context{&callbacks, &expanded};
        genTangSpaceDefault(&context);
    }
    // Orthogonal to the normal and of unit length; a perpendicular where the uvs give no direction.
    for (ModelVertex& v : expanded) {
        glm::vec3 t(v.tangent);
        t -= v.normal * glm::dot(v.normal, t);
        const float length = glm::length(t);
        if (!(length > 1e-6f) || !std::isfinite(length)) {
            v.tangent = glm::vec4(perpendicular(v.normal), 1.0f);
        } else {
            v.tangent = glm::vec4(t / length, v.tangent.w < 0.0f ? -1.0f : 1.0f);
        }
    }

    // Weld the corners again; the source index keeps vertices apart that were apart (their skin weights may differ).
    const meshopt_Stream streams[2] = {{expanded.data(), sizeof(ModelVertex), sizeof(ModelVertex)},
                                       {source.data(), sizeof(std::uint32_t), sizeof(std::uint32_t)}};
    std::vector<std::uint32_t> remap(expanded.size());
    const std::size_t unique =
        meshopt_generateVertexRemapMulti(remap.data(), nullptr, expanded.size(), expanded.size(), streams, 2);
    vertices.resize(unique);
    meshopt_remapVertexBuffer(vertices.data(), expanded.data(), expanded.size(), sizeof(ModelVertex), remap.data());
    std::vector<std::uint32_t> from(unique);
    meshopt_remapVertexBuffer(from.data(), source.data(), source.size(), sizeof(std::uint32_t), remap.data());
    for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = remap[i];
    return from;
}

} // namespace quench::assets
