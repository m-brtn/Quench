#pragma once

#include <quench/core/Result.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Cooked models (.qmodel): the meshes of a glTF scene with the node transforms applied, their materials, and the
// level-of-detail index ranges quench_cook made with meshoptimizer. Vertex and index streams are compressed with
// meshoptimizer's codecs. Materials name their textures by asset path (cooked KTX2 files).
namespace quench::assets {

// The engine's 48-byte vertex (render::Vertex has the same layout).
struct ModelVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f}; // linear
};
static_assert(sizeof(ModelVertex) == 48);

enum class AlphaMode : std::uint8_t { Opaque, Mask, Blend };

// glTF's metallic-roughness material. Empty texture paths: none.
struct ModelMaterial {
    std::string name;
    glm::vec4 baseColor{1.0f}; // linear rgb, a: opacity
    glm::vec3 emissive{0.0f};  // linear
    float roughness = 1.0f;
    float metallic = 1.0f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f; // AlphaMode::Mask
    bool doubleSided = false;
    std::string baseColorTexture;         // sRGB
    std::string metallicRoughnessTexture; // linear; green: roughness, blue: metallic
    std::string normalTexture;            // linear; cooked, not read by the standard surface yet
    std::string emissiveTexture;          // sRGB; cooked, not read by the standard surface yet
};

// A range of ModelMesh::indices drawn at one level of detail; error is meshoptimizer's relative simplification error.
struct ModelLod {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    float error = 0.0f;
};

struct ModelMesh {
    std::string name;
    std::uint32_t material = 0;         // index into ModelData::materials
    std::vector<ModelVertex> vertices;  // in model space
    std::vector<std::uint32_t> indices; // triangle lists of every LOD, LOD 0 first
    std::vector<ModelLod> lods;         // at least one
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

struct ModelData {
    std::vector<ModelMesh> meshes;
    std::vector<ModelMaterial> materials;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// Checks the header, the counts and every stream.
Result<ModelData> readModel(std::span<const std::uint8_t> file);
// The output depends only on the model. The index codec keeps the order and winding of triangles but may rotate the
// indices within one.
Result<std::vector<std::uint8_t>> writeModel(const ModelData& model);

} // namespace quench::assets
