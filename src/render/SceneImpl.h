#pragma once

// The data of a render::Scene, read by the Renderer.

#include "SlotMap.h"

#include <quench/gpu/Device.h>
#include <quench/render/Scene.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

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
    // Ray tracing (gpu::Caps::rayTracing): the bottom level of a rigid mesh, built when it first casts a ray-traced
    // shadow, then replaced by its compacted copy.
    gpu::AccelerationStructure rayStructure;
    enum class Compaction : std::uint8_t { None, Queried, Done } rayCompaction = Compaction::None;
};

struct TextureEntry {
    gpu::Texture texture;
};

struct MaterialEntry {
    MaterialDesc desc; // without the name
    std::uint32_t users = 0;
};

struct InstanceEntry {
    InstanceDesc desc;
    glm::mat4 previous{1.0f}; // the transform at the last render
    bool moved = false;       // desc.transform differs from `previous`
    // Skinned meshes
    std::vector<glm::mat4> palette; // empty: the bind pose
    glm::vec3 center{0.0f};         // object-space bounds of the current pose (the mesh's until a palette is set)
    float radius = 0.0f;
    bool posed = false;                 // the palette changed since the last render
    std::uint32_t skinnedOffset = 0;    // first vertex in the renderer's skinned vertex buffer of the last render
    std::uint64_t skinnedFrame = ~0ull; // the renderer's frame index of that render; ~0: none
    // Ray tracing of a skinned instance: the bottom level of its pose, refitted every frame it is traced.
    gpu::AccelerationStructure rayStructure;
    std::uint32_t rayRefits = 0; // since its last build
};

struct LightEntry {
    LightDesc desc;
};

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
};

} // namespace quench::render::detail
