#pragma once

// The renderer's ray tracing (gpu::Caps::rayTracing): the acceleration structures of the scene's shadow casters, the
// tables raytrace.hlsl reads for cut-out and translucent ones, and the passes that trace the prepass depth into the
// mask the surfaces read (sun visibility and ambient occlusion). docs/render.md, "Ray-traced shadows".

#include "SceneImpl.h"
#include "shaders/RayTracingUniforms.h"

#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>

#include <cstdint>
#include <span>
#include <vector>

namespace quench::render::detail {

// A caster of this frame.
struct RayCaster {
    std::uint32_t slot = 0; // instance slot
    InstanceEntry* instance = nullptr;
    MeshEntry* mesh = nullptr;
    const MaterialEntry* material = nullptr;
    gpu::BufferBinding skinned{}; // this frame's posed vertices (Vertex); empty: the mesh's own
    bool translucent = false;     // ShadowMode::Translucent: tints the rays it crosses
};

struct RayTracingStats {
    std::uint32_t instances = 0;
    std::uint32_t builds = 0;
    std::uint32_t refits = 0;
};

class RayTracing {
public:
    // The pipelines; false when the device or the build cannot trace (logged once). `pipelines` counts the ones made.
    bool init(gpu::Device& device, std::uint32_t& pipelines);
    bool available() const { return m_available; }

    // The depth prepass target and the masks at the render size; false on failure (logged).
    bool ensureTargets(std::uint32_t width, std::uint32_t height, gpu::TextureFormat depthFormat);
    void releaseTargets();
    const gpu::Texture& depth() const { return m_depth; }
    // rgb: the sun's visibility (translucent casters tint it), a: ambient occlusion. Valid after trace().
    const gpu::Texture& mask() const { return m_mask; }

    // The bottom levels, the top level and the tables of the casters; false when there is nothing to trace against.
    bool build(gpu::CommandBuffer& commands, const SceneImpl& scene, std::span<const RayCaster> casters,
               RayTracingStats& stats);
    // Traces the prepass depth into the mask (the frame uniforms are pushed already).
    void trace(gpu::CommandBuffer& commands, const shader::RayTracingUniforms& uniforms, const gpu::Sampler& point,
               const gpu::Sampler& material, const gpu::Texture& white);

private:
    struct TableMesh {
        MeshId id{};
        const MeshEntry* mesh = nullptr;
        std::uint32_t firstVertex = 0;
        std::uint32_t firstIndex = 0;
    };

    bool ensureBuffer(gpu::Buffer& buffer, std::uint32_t& capacity, std::uint32_t bytes, const char* name);
    bool updateTables(gpu::CommandBuffer& commands, std::span<const RayCaster> casters);

    gpu::Device* m_device = nullptr;
    bool m_available = false;
    gpu::GraphicsPipeline m_tracePipeline, m_denoisePipeline;
    std::uint32_t m_width = 0, m_height = 0;
    gpu::Texture m_depth, m_raw, m_mask;

    gpu::AccelerationStructure m_top;
    std::vector<gpu::RayInstance> m_instances;
    // Cut-out and translucent casters: their meshes' vertices and indices, and per instance id what a hit reads.
    std::vector<TableMesh> m_tableMeshes;
    gpu::Buffer m_vertices, m_indices, m_instanceTable;
    std::uint32_t m_vertexBytes = 0, m_indexBytes = 0, m_instanceBytes = 0;
    gpu::TransferBuffer m_transfer;
    std::uint32_t m_transferBytes = 0;
    std::vector<shader::RayInstanceData> m_data;
    std::vector<const gpu::Texture*> m_alphaTextures;
    bool m_alphaTexturesLogged = false;
    std::uint32_t m_skinnedBuilds = 0; // staggers the periodic builds of skinned bottom levels made together
};

} // namespace quench::render::detail
