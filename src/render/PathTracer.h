#pragma once

// The reference path tracer (RenderSettings::pathTracer, PathTrace.hlsli): a running mean of one path per pixel and
// frame, restarted when what it sees changes. docs/render.md, "Path tracer".

#include "RayTracing.h"

#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>

#include <array>
#include <cstdint>

namespace quench::render::detail {

class PathTracer {
public:
    // false when the pipelines cannot be made (logged); `pipelines` counts them.
    bool init(gpu::Device& device, RayTracing& rays, std::uint32_t& pipelines);
    bool ready() const { return static_cast<bool>(m_pipeline); }
    bool ensureTargets(std::uint32_t width, std::uint32_t height);
    void release();

    // Adds a path per pixel to the mean (none once it holds `maxSamples`; a different `state` starts it again) and
    // returns it: linear radiance at the render size. The frame uniforms are pushed already.
    const gpu::Texture& render(gpu::CommandBuffer& commands, RayTracing& rays, shader::RayHitUniforms uniforms,
                               const shader::LightUniforms& lights, std::uint64_t state, int bounces, int maxSamples,
                               const gpu::Sampler& point);
    std::uint32_t samples() const { return m_samples; }

private:
    gpu::Device* m_device = nullptr;
    gpu::GraphicsPipeline m_pipeline, m_mapsPipeline; // the second's hits read normal maps (RayTracing::normalMaps)
    std::array<gpu::Texture, 2> m_accumulation;
    std::uint32_t m_width = 0, m_height = 0;
    int m_index = 0;
    std::uint32_t m_samples = 0;
    std::uint64_t m_state = 0;
};

} // namespace quench::render::detail
