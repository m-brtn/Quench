#pragma once

// The renderer's screen-space effects: what the surfaces read in place of a traced effect where the device cannot
// trace (RenderSettings::screenSpaceFallbacks) or where a game asks for them. Ambient occlusion: GTAO from the depth
// of the opaque surfaces (the ray tracing prepass's, or a depth prepass of its own where nothing is traced), blurred
// into alpha of the mask the surfaces read. Reflections: rays marched through that depth with the G-buffer's normal
// and roughness, their hits' color from the previous frame's image (kept here). docs/render.md, "Screen-space
// effects".

#include "shaders/ScreenSpaceUniforms.h"

#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace quench::render::detail {

class ScreenSpace {
public:
    // false when a pipeline fails (logged): the effects are left out. `pipelines` counts the ones made.
    bool init(gpu::Device& device, std::uint32_t& pipelines, const gpu::Sampler& point, const gpu::Sampler& linear);
    bool ready() const { return static_cast<bool>(m_resolvePipeline); }

    struct Targets {
        std::uint32_t width = 0, height = 0; // the render size
        gpu::TextureFormat depthFormat = gpu::TextureFormat::Invalid;
        gpu::TextureFormat imageFormat = gpu::TextureFormat::Invalid; // the HDR image's (the reflections' history)
        bool ownDepth = false; // a depth prepass of its own (frames where nothing is traced), and its G-buffer
        bool occlusion = false;
        bool reflections = false;
        friend bool operator==(const Targets&, const Targets&) = default;
    };
    bool ensureTargets(const Targets& targets);
    void releaseTargets();
    const gpu::Texture& depth() const { return m_depth; }
    const gpu::Texture& mask() const { return m_mask; }
    // The own prepass's G-buffer (with ownDepth and reflections), in RayTracing's formats.
    const gpu::Texture& gbufferNormal() const { return m_gbufferNormal; }
    const gpu::Texture& gbufferAlbedo() const { return m_gbufferAlbedo; }
    const gpu::Texture& reflections() const { return m_reflections; }

    // GTAO from `depth` (at half resolution) into alpha of `mask` (an Rgba8Unorm target of the render size, blurred
    // and upsampled): rgb 1, or kept with keepRgb (the traced sun's visibility).
    void occlusion(gpu::CommandBuffer& commands, const gpu::Texture& depth, const gpu::Texture& mask, bool keepRgb,
                   const shader::ScreenSpaceUniforms& uniforms);

    // The previous frame's image: valid when the last frame kept one at this size.
    bool historyValid() const { return m_historyValid; }
    // Its view-projection for camera-relative positions of a frame whose camera is at `cameraPos`.
    glm::mat4 historyViewProj(const glm::vec3& cameraPos) const;
    // Copies this frame's HDR image (the render size, imageFormat) as the next frame's history; `viewProj` is the
    // camera-relative view-projection it was drawn with.
    void keepHistory(gpu::CommandBuffer& commands, const gpu::Texture& image, const glm::mat4& viewProj,
                     const glm::vec3& cameraPos);
    void dropHistory() { m_historyValid = false; }
    // Reflections from `depth` and the G-buffer's `normal` (at half resolution, hits from the history) into
    // reflections(): rgb radiance, a confidence.
    void reflect(gpu::CommandBuffer& commands, const gpu::Texture& depth, const gpu::Texture& normal,
                 const shader::ScreenSpaceUniforms& uniforms);

private:
    bool createTarget(gpu::Texture& out, gpu::TextureFormat format, gpu::TextureUsage usage, std::uint32_t width,
                      std::uint32_t height, const char* name);

    gpu::Device* m_device = nullptr;
    const gpu::Sampler* m_point = nullptr;
    const gpu::Sampler* m_linear = nullptr;
    gpu::GraphicsPipeline m_gtaoPipeline;
    gpu::GraphicsPipeline m_blurPipeline;      // writes the mask's rgb (1) and alpha
    gpu::GraphicsPipeline m_blurAlphaPipeline; // writes its alpha only
    gpu::GraphicsPipeline m_reflectPipeline;
    gpu::GraphicsPipeline m_resolvePipeline;
    gpu::Texture m_depth, m_term, m_mask;
    gpu::Texture m_gbufferNormal, m_gbufferAlbedo, m_reflectTerm, m_reflections, m_history;
    Targets m_targets{};
    glm::mat4 m_historyViewProj{1.0f};
    glm::vec3 m_historyCamera{0.0f};
    bool m_historyValid = false;
};

} // namespace quench::render::detail
