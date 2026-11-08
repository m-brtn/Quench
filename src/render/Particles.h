#pragma once

// The scene's particle systems and trails (RenderSettings::particles, docs/particles.md), after TAA and upscaling:
// each visible system's spawns made on the CPU (particles::Emitter) and its slots stepped by a compute pass (one
// dispatch per system) or on the CPU (particles::Simulation, the same functions, uploaded); the trails' strips built
// on the CPU; then systems and trails drawn back to front into the output-size image, tested against the scene's
// depth and faded where they near it. Every buffer grows by doubling and is kept: nothing is allocated per frame once
// the scene's particles have been seen.

#include "SceneImpl.h"

#include <quench/SceneUniforms.h>
#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>
#include <quench/particles/Particles.h>
#include <quench/particles/Trail.h>
#include <quench/render/Renderer.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace quench::render::detail {

class ParticleRenderer {
public:
    // The draws' pipelines (false when one fails, logged: no particles) and the step's (computes() false when it
    // fails, logged: the CPU simulation). `pipelines` counts the ones made.
    bool init(gpu::Device& device, gpu::TextureFormat hdrFormat, std::uint32_t& pipelines);
    bool ready() const { return static_cast<bool>(m_particlePipeline); }
    bool computes() const { return static_cast<bool>(m_stepPipeline); }

    // The scene has systems or trails.
    static bool any(const SceneImpl& scene) { return scene.particleSystems.size() > 0 || scene.trails.size() > 0; }

    struct Frame {
        ParticleSimulation mode = ParticleSimulation::Off; // Gpu or Cpu
        double time = 0.0;                                 // View::time
        float density = 1.0f;                              // RenderSettings::particleDensity
        gpu::TextureRef image;                             // after TAA and upscaling
        bool keepImage = false;                            // the image is read again (TAA's history): draw into a copy
        const gpu::Texture* depth = nullptr;               // the scene's (render size)
        glm::mat4 viewProjStable{1.0f};
        glm::mat4 proj{1.0f};
        glm::mat4 invView{1.0f};
        float nearPlane = 0.1f;
    };
    // Outside passes: steps the systems, builds the trails and draws both; returns the image they were drawn into.
    gpu::TextureRef render(gpu::CommandBuffer& commands, SceneImpl& scene, const Frame& frame,
                           const gpu::Sampler& pointSampler, const shader::FrameUniforms& fu, RenderStats& stats);

    // Frees the image copy and the frame's buffers (particles off; the scene keeps its particles' state).
    void release();

private:
    // A system or a trail, drawn in order of distance.
    struct Item {
        float distance;
        std::uint32_t index; // a system's or a trail's slot in the scene
        bool trail;
        std::uint32_t first, count; // a system's first slot and its slots, or a trail's strip in the trail buffer
        std::uint32_t head;         // a trail's head glow in the heads buffer (~0u: none)
    };

    bool ensurePool(gpu::CommandBuffer& commands, ParticlePool& pool);
    bool ensureBuffer(gpu::Buffer& buffer, std::uint32_t& capacity, std::uint32_t bytes, gpu::BufferUsage usage,
                      const char* name);
    void step(SceneImpl& scene, const Frame& frame, RenderStats& stats);
    void buildTrails(SceneImpl& scene, const Frame& frame, RenderStats& stats);
    bool upload(gpu::CommandBuffer& commands, ParticlePool& pool);

    gpu::Device* m_device = nullptr;
    gpu::TextureFormat m_hdrFormat = gpu::TextureFormat::Invalid;
    gpu::GraphicsPipeline m_particlePipeline, m_trailPipeline;
    gpu::ComputePipeline m_stepPipeline;
    gpu::Texture m_image; // the copy drawn into when the image must stay as it is

    // This frame's spawns for the GPU and the CPU systems, the CPU systems' instances (by slot of the pool, their
    // ranges listed in m_cpuRanges), the trails' vertices and the heads' instances.
    std::vector<particles::Particle> m_gpuSpawns, m_cpuSpawns;
    std::vector<particles::ParticleInstance> m_cpuInstances;
    std::vector<ParticlePool::Range> m_cpuRanges;
    std::vector<particles::TrailVertex> m_trailVertices;
    std::vector<particles::ParticleInstance> m_heads;
    std::vector<particles::ParticleUniforms> m_stepUniforms; // the GPU systems' steps this frame
    std::vector<Item> m_items;

    gpu::Buffer m_spawnBuffer, m_trailBuffer, m_headBuffer;
    std::uint32_t m_spawnCapacity = 0, m_trailCapacity = 0, m_headCapacity = 0; // bytes
    gpu::TransferBuffer m_transfer;
    std::uint32_t m_transferCapacity = 0;
};

} // namespace quench::render::detail
