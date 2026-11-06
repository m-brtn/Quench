#pragma once

// The renderer's ray tracing (gpu::Caps::rayTracing): the acceleration structures of the scene, the tables the
// shaders read for every instance (triangles, materials, the hit texture arrays), the prepass targets, and the passes
// that trace into what the surfaces read: the sun's visibility and ambient occlusion, local light shadows,
// reflections, indirect light and the probe grid, each with its filter. docs/render.md, "Ray-traced shadows" and
// "Ray-traced lighting".

#include "SceneImpl.h"
#include "TemporalFilter.h"
#include "shaders/RayTracingUniforms.h"

#include <quench/SceneUniforms.h>
#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace quench::render::detail {

// An instance in the top level this frame.
struct RayCaster {
    std::uint32_t slot = 0; // instance slot
    InstanceEntry* instance = nullptr;
    MeshEntry* mesh = nullptr;
    const MaterialEntry* material = nullptr;
    gpu::BufferBinding skinned{}; // this frame's posed vertices (Vertex); empty: the mesh's own
    bool caster = true;           // casts shadows (else only seen by reflection, indirect and path rays)
    bool translucent = false;     // ShadowMode::Translucent: tints the rays it crosses
    bool doubleSided = false;
};

struct RayTracingStats {
    std::uint32_t instances = 0;
    std::uint32_t builds = 0; // bottom levels
    std::uint32_t refits = 0;
    std::uint32_t compactions = 0;
    std::uint64_t compactedBytes = 0; // saved by this frame's compactions
    bool topRefit = false;
    std::uint64_t memoryBytes = 0;
    std::uint32_t hitTextures = 0;
};

// The targets of a frame besides the depth and the sun's mask.
struct RayTargets {
    bool gbuffer = false; // normal and roughness, albedo and metallic (reflections, indirect light, MetalFX)
    bool local = false;   // local light shadows
    bool reflections = false;
    bool indirect = false;
    bool probes = false;        // the probe grid's atlases
    bool temporal = false;      // the histories of RayDenoiser::Temporal
    bool denoiseInputs = false; // the albedos and roughness of the denoising upscaler (with gbuffer)
    friend bool operator==(const RayTargets&, const RayTargets&) = default;
};

// The samplers and placeholder the renderer owns, bound by the passes.
struct RaySamplers {
    const gpu::Sampler* point = nullptr;
    const gpu::Sampler* linear = nullptr;
    const gpu::Sampler* material = nullptr;
    const gpu::Texture* white = nullptr;
    const gpu::Texture* array = nullptr;     // a texture array, bound in place of the hit textures before there are any
    const gpu::Texture* areaTable = nullptr; // the area lights' table (quench/AreaLights.hlsli)
};

// How the traced signals are filtered before the surfaces read them.
enum class RayFiltering : std::uint8_t {
    Raw,      // as traced (the denoising upscaler filters them with the frame)
    Spatial,  // RayDenoiser::Spatial
    Temporal, // RayDenoiser::Temporal
};

// The probe grid's layout this frame (FrameUniforms probeGrid, probeCounts, probeAtlas).
struct ProbeGridState {
    glm::ivec3 first{0};
    glm::ivec3 counts{16, 8, 16};
    float spacing = 2.0f;
};

class RayTracing {
public:
    static constexpr int kProbeRays = 64;
    static constexpr int kIrradianceTile = 8; // texels per probe side with the border
    static constexpr int kDistanceTile = 16;
    static constexpr glm::ivec3 kProbeCounts{16, 8, 16};

    // The pipelines; false when the device or the build cannot trace (logged once). `pipelines` counts the ones made.
    bool init(gpu::Device& device, std::uint32_t& pipelines, const RaySamplers& samplers);
    bool available() const { return m_available; }
    // The reflection, indirect light, probe and path tracer passes (they need the rayquery shaders of raylight.hlsl).
    bool lighting() const { return m_lighting; }

    // The prepass and the traced targets at the render size; false on failure (logged).
    bool ensureTargets(std::uint32_t width, std::uint32_t height, gpu::TextureFormat depthFormat,
                       const RayTargets& targets);
    void releaseTargets();
    // A new frame: the depth written last frame becomes the previous depth of the temporal filters.
    void beginFrame() {
        m_depthIndex ^= 1;
        m_frameReset = m_recreated;
        m_recreated = false;
    }
    const gpu::Texture& depth() const { return m_depth[std::size_t(m_depthIndex)]; }
    const gpu::Texture& previousDepth() const { return m_depth[std::size_t(m_depthIndex ^ 1)]; }
    const gpu::Texture& gbufferNormal() const { return m_gbufferNormal; }
    const gpu::Texture& gbufferAlbedo() const { return m_gbufferAlbedo; }
    static gpu::TextureFormat gbufferNormalFormat() { return gpu::TextureFormat::Rgba16Float; }
    static gpu::TextureFormat gbufferAlbedoFormat() { return gpu::TextureFormat::Rgba8Unorm; }
    // rgb: the sun's visibility (translucent casters tint it), a: ambient occlusion. Valid after traceShadows().
    const gpu::Texture& mask() const { return m_mask; }
    const gpu::Texture& localShadows() const { return m_local; }
    const gpu::Texture& reflections() const { return m_reflections; }
    const gpu::Texture& indirect() const { return m_indirect; }
    // The probe atlases of this frame (valid after updateProbes()).
    const gpu::Texture& probeIrradiance() const { return m_probeIrradiance[std::size_t(m_probeIndex)]; }
    const gpu::Texture& probeDistance() const { return m_probeDistance[std::size_t(m_probeIndex)]; }
    // Upscaling::TemporalDenoised's inputs besides the G-buffer normal, valid after writeDenoiseInputs().
    const gpu::Texture& diffuseAlbedo() const { return m_diffuseAlbedo; }
    const gpu::Texture& specularAlbedo() const { return m_specularAlbedo; }
    const gpu::Texture& roughness() const { return m_roughness; }
    static constexpr gpu::TextureFormat kAlbedoFormat = gpu::TextureFormat::Rgba8Unorm;
    static constexpr gpu::TextureFormat kRoughnessFormat = gpu::TextureFormat::R16Float;

    // The bottom levels, the top level and the tables; false when there is nothing to trace against. hitData: the
    // triangles and textures of every instance for the hit passes (else of cut-out and translucent casters only).
    // skinned: this frame's skinned vertex buffer (read by hit shading).
    bool build(gpu::CommandBuffer& commands, const SceneImpl& scene, std::span<const RayCaster> casters, bool hitData,
               const gpu::BufferRef& skinned, RayTracingStats& stats);

    // The passes below run after the prepass, with the frame uniforms pushed; lights: the frame's, with the channels
    // of the local lights with ray-traced shadows (LocalLight::cone.y).
    // The prepass depth traced into the mask and, with uniforms.local.x > 0, the local light shadows; then filtered.
    void traceShadows(gpu::CommandBuffer& commands, const shader::RayTracingUniforms& uniforms,
                      const shader::LightUniforms& lights, const FilterInputs& inputs, RayFiltering filtering);
    // The probe grid's rays, then its irradiance and distance maps (a grid that moved keeps the probes it still
    // holds). reset: drop the history of every probe.
    void updateProbes(gpu::CommandBuffer& commands, shader::RayHitUniforms uniforms,
                      const shader::LightUniforms& lights, bool reset);
    void traceReflections(gpu::CommandBuffer& commands, const shader::RayHitUniforms& uniforms,
                          const shader::LightUniforms& lights, const FilterInputs& inputs, RayFiltering filtering);
    void traceIndirect(gpu::CommandBuffer& commands, const shader::RayHitUniforms& uniforms,
                       const shader::LightUniforms& lights, const FilterInputs& inputs, RayFiltering filtering);
    // The denoising upscaler's albedos and roughness from the G-buffer of this frame's prepass; prepass false (no
    // prepass ran): the sky's values everywhere, the G-buffer normal cleared. False without the targets.
    bool writeDenoiseInputs(gpu::CommandBuffer& commands, bool prepass);
    // Binds what RayHit.hlsli reads (samplers 0..5, the tables and the top level); probes: the grid's atlases
    // (else placeholders).
    void bindHits(gpu::RenderPass& pass, bool probes);
    // A pipeline of a hit pass of raylight.hlsl into one target of `format`.
    Result<gpu::GraphicsPipeline> hitPipeline(const char* entry, gpu::TextureFormat format, const char* name);

    // The probe grid around a camera position for this frame (FrameUniforms probeGrid, probeCounts, probeAtlas).
    ProbeGridState placeProbes(const glm::vec3& camera, float spacing);
    // The first cell of the grid the probes hold (the previous frame's placement until updateProbes()).
    glm::ivec3 probeHistoryFirst() const { return m_previousFirst; }
    bool probesValid() const { return m_probesValid; }
    std::uint64_t memoryBytes() const;

private:
    struct TableMesh {
        MeshId id{};
        const MeshEntry* mesh = nullptr;
        std::uint32_t firstVertex = 0;
        std::uint32_t firstIndex = 0;
    };
    // A material texture in a layer of a hit texture array: the scene, its handle and the texture object it had
    // (Scene::setTexture replaces it).
    struct HitLayer {
        std::uint64_t scene = 0;
        TextureId id{};
        void* native = nullptr;
        bool used = false; // this frame
    };
    struct HitArray {
        gpu::Texture texture;
        std::vector<HitLayer> layers;
        gpu::TextureFormat format = gpu::TextureFormat::Invalid;
        const char* name = "";
    };

    bool ensureBuffer(gpu::Buffer& buffer, std::uint32_t& capacity, std::uint32_t bytes, const char* name);
    bool updateTables(gpu::CommandBuffer& commands, std::uint64_t scene, std::span<const RayCaster> casters, bool all);
    // The layers of this frame's textures (-1: none) after copying new ones in; `layers` per texture of `wanted`.
    void updateHitArray(gpu::CommandBuffer& commands, HitArray& array, std::uint64_t scene,
                        std::span<const std::pair<TextureId, const gpu::Texture*>> wanted, std::vector<int>& layers);
    bool createTarget(gpu::Texture& out, gpu::TextureFormat format, std::uint32_t width, std::uint32_t height,
                      const char* name,
                      gpu::TextureUsage usage = gpu::TextureUsage::ColorTarget | gpu::TextureUsage::Sampled);
    void filter(gpu::CommandBuffer& commands, TemporalFilter& filter, const gpu::Texture& raw,
                const gpu::Texture& output, const FilterInputs& inputs, const FilterParams& params,
                RayFiltering filtering);

    gpu::Device* m_device = nullptr;
    RaySamplers m_samplers;
    bool m_available = false;
    bool m_lighting = false;
    gpu::GraphicsPipeline m_tracePipeline, m_traceLocalPipeline, m_denoisePipeline, m_denoiseInputsPipeline;
    gpu::GraphicsPipeline m_reflectPipeline, m_indirectPipeline, m_probeTracePipeline, m_probeIrradiancePipeline,
        m_probeDistancePipeline;
    TemporalFilter m_maskFilter, m_localFilter, m_reflectionFilter, m_indirectFilter;
    std::uint32_t m_width = 0, m_height = 0;
    RayTargets m_targets;
    std::array<gpu::Texture, 2> m_depth;
    int m_depthIndex = 0;
    bool m_recreated = false;  // the targets were made since the last frame
    bool m_frameReset = false; // this frame has no history
    gpu::Texture m_gbufferNormal, m_gbufferAlbedo, m_diffuseAlbedo, m_specularAlbedo, m_roughness;
    gpu::Texture m_raw, m_mask, m_localRaw, m_local, m_reflectionsRaw, m_reflections, m_indirectRaw, m_indirect;

    gpu::AccelerationStructure m_top;
    std::vector<gpu::RayInstance> m_instances;
    std::vector<const gpu::AccelerationStructure*> m_topBottoms; // per instance, of the top level's last build
    std::uint32_t m_topRefits = 0;                               // since that build
    std::uint64_t m_structureBytes = 0;                          // the acceleration structures in use
    // The meshes' vertices and indices, and per instance id what a hit reads.
    std::vector<TableMesh> m_tableMeshes;
    std::uint64_t m_tableScene = 0; // SceneImpl::serial of the scene the tables were copied from
    bool m_tableAll = false;        // the tables hold every mesh of the top level (hit data)
    gpu::Buffer m_vertices, m_indices, m_instanceTable;
    std::uint32_t m_vertexBytes = 0, m_indexBytes = 0, m_instanceBytes = 0;
    gpu::BufferRef m_skinned;
    gpu::TransferBuffer m_transfer;
    std::uint32_t m_transferBytes = 0;
    std::vector<shader::RayInstanceData> m_data;
    std::vector<const gpu::Texture*> m_alphaTextures;
    bool m_alphaTexturesLogged = false;
    HitArray m_hitColor, m_hitLinear;
    bool m_hitLayersLogged = false;
    std::uint32_t m_skinnedBuilds = 0; // staggers the periodic builds of skinned bottom levels made together

    // The probe grid
    std::array<gpu::Texture, 2> m_probeIrradiance, m_probeDistance;
    gpu::Texture m_probeRays;
    int m_probeIndex = 0;
    bool m_probesValid = false; // the atlases hold a grid
    ProbeGridState m_grid;
    glm::ivec3 m_previousFirst{0};
};

} // namespace quench::render::detail
