#pragma once

// The filter of one traced signal (raydenoise.hlsl): with RayDenoiser::Temporal a history reprojected by the motion
// vectors, then a-trous passes weighted by the surface's plane; without history the a-trous passes only.
// docs/render.md, "Denoising".

#include "shaders/RayTracingUniforms.h"

#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>

#include <array>
#include <cstdint>

namespace quench::render::detail {

struct FilterParams {
    float maxHistory = 32.0f;     // frames
    float clampGamma = 2.0f;      // the history is clamped to mean +- gamma sigma of the new samples (0: no clamp)
    bool alphaScale = false;      // history and radius scale with the signal's alpha (reflections)
    int steps = 2;                // a-trous passes: 1 (step 1) or 2 (steps 1 and 2)
    float planeTolerance = 0.02f; // relative to the view depth
    float depthTolerance = 0.05f; // relative: a reprojected depth further off is another surface
};

struct FilterInputs {
    gpu::TextureRef depth;         // this frame's prepass depth
    gpu::TextureRef previousDepth; // the previous frame's
    gpu::TextureRef velocity;      // motion vectors (this frame's uv minus the previous frame's)
    bool reset = false;            // no history (a camera cut)
};

class TemporalFilter {
public:
    // false when a pipeline fails (logged); `pipelines` counts the ones made.
    bool init(gpu::Device& device, gpu::TextureFormat format, const char* name, std::uint32_t& pipelines);
    bool ready() const { return static_cast<bool>(m_atrous); }
    bool ensureTargets(std::uint32_t width, std::uint32_t height);
    void release();
    // A frame without run(): the next run starts without history.
    void invalidate() { m_valid = false; }

    // Filters `signal` into `output` (render size, the filter's format); temporal: through the history first.
    void run(gpu::CommandBuffer& commands, const gpu::Texture& signal, const gpu::Texture& output,
             const FilterInputs& inputs, const FilterParams& params, bool temporal, const gpu::Sampler& point,
             const gpu::Sampler& linear);

private:
    gpu::Device* m_device = nullptr;
    gpu::TextureFormat m_format = gpu::TextureFormat::Invalid;
    const char* m_name = "";
    gpu::GraphicsPipeline m_temporal, m_atrous;
    std::uint32_t m_width = 0, m_height = 0;
    std::array<gpu::Texture, 2> m_history, m_length;
    gpu::Texture m_scratch;
    int m_index = 0;
    bool m_valid = false; // the history holds a previous frame
};

} // namespace quench::render::detail
