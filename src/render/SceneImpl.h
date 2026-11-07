#pragma once

// The data of a render::Scene, read by the Renderer.

#include "SlotMap.h"

#include <quench/gpu/Device.h>
#include <quench/render/Scene.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace quench::render::detail {

struct MeshEntry {
    gpu::Buffer vertices; // skinned meshes: in the bind pose, also read by the skinning pass
    gpu::Buffer indices;
    std::uint32_t indexCount = 0;
    glm::vec3 center{0.0f}; // bounding sphere in object space (skinned: of the bind pose)
    float radius = 0.0f;
    std::uint32_t users = 0; // instances
    std::uint32_t vertexCount = 0;
    // Skinned meshes
    gpu::Buffer skin; // SkinVertex per vertex
    std::uint32_t jointCount = 0;
    std::vector<glm::vec4> jointBounds; // per joint: the bind-pose sphere of the vertices it moves (w < 0: none)
    // Blend shapes: three float4 per offset (position.xyz normal.x | normal.yz tangent.xy | tangent.z), target-major,
    // read by the skinning pass; per target the length of its largest position offset.
    gpu::Buffer morphs;
    std::uint32_t morphTargetCount = 0;
    std::vector<float> morphReach;
    // Ray tracing (gpu::Caps::rayTracing): the bottom level of a rigid mesh, built when it first casts a ray-traced
    // shadow, then replaced by its compacted copy.
    gpu::AccelerationStructure rayStructure;
    enum class Compaction : std::uint8_t { None, Queried, Done } rayCompaction = Compaction::None;
};

// Posed by the skinning pass: skinned, with blend shapes, or both.
inline bool deformed(const MeshEntry& mesh) { return mesh.jointCount > 0 || mesh.morphTargetCount > 0; }

struct TextureEntry {
    gpu::Texture texture;
};

struct MaterialEntry {
    MaterialDesc desc; // without the name
    std::uint32_t users = 0;
    std::uint32_t light = 0;    // the shape of an area light: 1 + the light's slot (LocalLight::shape.w), else 0
    bool lightTwoSided = false; // the shape emits from both faces (else its back is black)
};

struct InstanceEntry {
    InstanceDesc desc;
    glm::mat4 previous{1.0f}; // the transform at the last render
    bool moved = false;       // desc.transform differs from `previous`
    // Skinned meshes
    std::vector<glm::mat4> palette; // empty: the bind pose
    glm::vec3 center{0.0f};         // object-space bounds of the current pose (the mesh's until a palette is set)
    float radius = 0.0f;
    std::vector<float> morphWeights;    // per target of the mesh; empty: all 0
    bool posed = false;                 // the palette or the morph weights changed since the last render
    std::uint32_t skinnedOffset = 0;    // first vertex in the renderer's skinned vertex buffer of the last render
    std::uint64_t skinnedFrame = ~0ull; // the renderer's frame index of that render; ~0: none
    // Ray tracing of a skinned instance: the bottom level of its pose, refitted every frame it is traced.
    gpu::AccelerationStructure rayStructure;
    std::uint32_t rayRefits = 0; // since its last build
};

struct LightEntry {
    LightDesc desc;
    // An area light's shape, drawn with its radiance: the scene's own (not in its counts).
    InstanceId shape{};
    MaterialId shapeMaterial{};
};

bool isAreaLight(LightType type);

// An area light's unit frame (tangent: rect along the width, tube the axis; normal: rect and disk; bitangent:
// cross(normal, tangent)), its half extents (rect: half width and height; disk: the radius twice; tube: half length
// and radius) and the radius of the sphere around its shape.
struct AreaLightFrame {
    glm::vec3 tangent{1.0f, 0.0f, 0.0f};
    glm::vec3 normal{0.0f, -1.0f, 0.0f};
    glm::vec3 bitangent{0.0f, 0.0f, 1.0f};
    glm::vec2 half{0.5f};
    float radius = 0.0f;
};
AreaLightFrame areaLightFrame(const LightDesc& desc);

struct SceneImpl {
    gpu::Device* device = nullptr;
    // Unique per scene in the process: handles of two scenes can be equal, caches keyed by them need it too.
    std::uint64_t serial = 0;
    SlotMap<MeshId, MeshEntry> meshes;
    SlotMap<TextureId, TextureEntry> textures;
    SlotMap<MaterialId, MaterialEntry> materials;
    SlotMap<InstanceId, InstanceEntry> instances;
    SlotMap<LightId, LightEntry> lights;
    Environment environment;
    // The area lights' shapes (rect, disk, tube), made with the first light of each, and the scene's own entries
    // in the maps above.
    std::array<MeshId, 3> areaShapes{};
    std::uint32_t ownMeshes = 0, ownMaterials = 0, ownInstances = 0;
};

} // namespace quench::render::detail
