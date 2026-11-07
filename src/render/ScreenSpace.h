#pragma once

// The renderer's screen-space effects: what the surfaces read in place of a traced effect where the device cannot
// trace (RenderSettings::screenSpaceFallbacks) or where a game asks for them. Ambient occlusion: GTAO from the depth
// of the opaque surfaces (the ray tracing prepass's, or a depth prepass of its own where nothing is traced), blurred
// into alpha of the mask the surfaces read. docs/render.md, "Screen-space effects".

#include "shaders/ScreenSpaceUniforms.h"

#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>

#include <cstdint>

namespace quench::render::detail {

class ScreenSpace {
public:
    // false when a pipeline fails (logged): the effects are left out. `pipelines` counts the ones made.
    bool init(gpu::Device& device, std::uint32_t& pipelines, const gpu::Sampler& point);
    bool ready() const { return static_cast<bool>(m_blurPipeline); }

    // The targets at the render size; ownDepth: also a depth prepass target and a mask of its own (frames where
    // nothing is traced).
    bool ensureTargets(std::uint32_t width, std::uint32_t height, gpu::TextureFormat depthFormat, bool ownDepth);
    void releaseTargets();
    const gpu::Texture& depth() const { return m_depth; }
    const gpu::Texture& mask() const { return m_mask; }

    // GTAO from `depth` (at half resolution) into alpha of `mask` (an Rgba8Unorm target of the render size, blurred
    // and upsampled): rgb 1, or kept with keepRgb (the traced sun's visibility).
    void occlusion(gpu::CommandBuffer& commands, const gpu::Texture& depth, const gpu::Texture& mask, bool keepRgb,
                   const shader::ScreenSpaceUniforms& uniforms);

private:
    bool createTarget(gpu::Texture& out, gpu::TextureFormat format, gpu::TextureUsage usage, std::uint32_t width,
                      std::uint32_t height, const char* name);

    gpu::Device* m_device = nullptr;
    const gpu::Sampler* m_point = nullptr;
    gpu::GraphicsPipeline m_gtaoPipeline;
    gpu::GraphicsPipeline m_blurPipeline;      // writes the mask's rgb (1) and alpha
    gpu::GraphicsPipeline m_blurAlphaPipeline; // writes its alpha only
    gpu::Texture m_depth, m_term, m_mask;
    std::uint32_t m_width = 0, m_height = 0;
    gpu::TextureFormat m_depthFormat = gpu::TextureFormat::Invalid;
    bool m_ownDepth = false;
};

} // namespace quench::render::detail
