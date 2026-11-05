#pragma once

// The data of a render::Scene, read by the Renderer.

#include "SlotMap.h"

#include <quench/gpu/Device.h>
#include <quench/render/Scene.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace quench::render::detail {

struct MeshEntry {
    gpu::Buffer vertices;
    gpu::Buffer indices;
    std::uint32_t indexCount = 0;
    glm::vec3 center{0.0f}; // bounding sphere in object space
    float radius = 0.0f;
    std::uint32_t users = 0; // instances
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
};

struct LightEntry {
    LightDesc desc;
};

struct SceneImpl {
    gpu::Device* device = nullptr;
    SlotMap<MeshId, MeshEntry> meshes;
    SlotMap<TextureId, TextureEntry> textures;
    SlotMap<MaterialId, MaterialEntry> materials;
    SlotMap<InstanceId, InstanceEntry> instances;
    SlotMap<LightId, LightEntry> lights;
    Environment environment;
};

} // namespace quench::render::detail
