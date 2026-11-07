#pragma once

#include <quench/core/Result.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Cooked models (.qmodel): the meshes of a glTF scene with the node transforms applied, their materials, and the
// level-of-detail index ranges quench_cook made with meshoptimizer. Vertex and index streams are compressed with
// meshoptimizer's codecs. Materials name their textures by asset path (cooked KTX2 files). Skinned meshes keep their
// bind-pose vertices (glTF ignores the transform of a skinned mesh's node) with four joints and weights per vertex,
// and name their skin's joints, which bind to an animation skeleton by name at run time. Meshes can have blend shapes
// (morph targets): per-vertex offsets of the position, normal and tangent, applied before skinning.
namespace quench::assets {

// The engine's 64-byte vertex (render::Vertex has the same layout).
struct ModelVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f}; // linear
    // xyz: unit tangent along +u; w: +-1, the bitangent (toward -v, the normal map's +y in glTF's convention) is
    // w * cross(normal, tangent). MikkTSpace tangents (generateTangents) or the glTF file's TANGENT.
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};
static_assert(sizeof(ModelVertex) == 64);

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
    std::string normalTexture;            // linear, tangent space (glTF's: +y toward -v)
    std::string emissiveTexture;          // sRGB; multiplied with emissive
};

// A range of ModelMesh::indices drawn at one level of detail; error is meshoptimizer's relative simplification error.
struct ModelLod {
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    float error = 0.0f;
};

// The joints that move one vertex of a skinned mesh: indices into its skin's joints, weights in 1/65535 that sum to
// 65535.
struct ModelSkinVertex {
    std::uint16_t joints[4] = {0, 0, 0, 0};
    std::uint16_t weights[4] = {65535, 0, 0, 0};
};
static_assert(sizeof(ModelSkinVertex) == 16);

// One vertex's offsets in a blend shape (glTF's morph target attributes), added to the base vertex times the target's
// weight. The normal and tangent offsets are those of the unit vectors: the renderer normalizes the sums.
struct ModelMorphVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec3 tangent{0.0f};
};
static_assert(sizeof(ModelMorphVertex) == 36);

// A blend shape (glTF morph target) of one mesh.
struct ModelMorphTarget {
    std::string name;                      // the glTF mesh's extras.targetNames, else "<mesh>.target<index>"
    float weight = 0.0f;                   // its default (the glTF mesh's weights)
    std::vector<ModelMorphVertex> offsets; // one per vertex of the mesh
};

// A glTF skin: its joints by name and their inverse bind matrices (model space at bind time to joint space).
struct ModelSkin {
    std::string name;
    std::vector<std::string> joints;
    std::vector<glm::mat4> inverseBindMatrices;
};

struct ModelMesh {
    std::string name;
    std::uint32_t material = 0;         // index into ModelData::materials
    std::vector<ModelVertex> vertices;  // in model space (skinned: in the bind pose)
    std::vector<std::uint32_t> indices; // triangle lists of every LOD, LOD 0 first
    std::vector<ModelLod> lods;         // at least one
    glm::vec3 boundsMin{0.0f};          // skinned: of the bind pose
    glm::vec3 boundsMax{0.0f};
    int skin = -1;                         // index into ModelData::skins; -1: not skinned
    std::vector<ModelSkinVertex> skinning; // one per vertex when skinned
    std::vector<ModelMorphTarget> morphTargets;
};

struct ModelData {
    std::vector<ModelMesh> meshes;
    std::vector<ModelMaterial> materials;
    std::vector<ModelSkin> skins;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// Checks the header, the counts and every stream. Reads versions 4 and 3 (without blend shapes); versions 1 and 2 (no
// tangents) are an error that says to cook the source again.
Result<ModelData> readModel(std::span<const std::uint8_t> file);
// Writes version 4. The output depends only on the model. The index codec keeps the order and winding of triangles but
// may rotate the indices within one.
Result<std::vector<std::uint8_t>> writeModel(const ModelData& model);

} // namespace quench::assets
