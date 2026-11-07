#pragma once

// The scene's clouds (RenderSettings::clouds, docs/weather.md): every frame with a weather, before the HDR pass, the
// weather map around the camera (quench_weather's field, drawn by the GPU), the clouds' shadow on the ground, the
// volumetric march at a share of the render size with its accumulation over frames, and the faces of the sky cube
// that surfaces reflect; in the HDR pass the sky with the clouds over it. The 2D cloud layer is the fallback where
// the device cannot render to a 3D texture, and the cheap setting.

#include "shaders/CloudUniforms.h"

#include <quench/SceneUniforms.h>
#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>
#include <quench/render/Renderer.h>
#include <quench/weather/Weather.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace quench::render::detail {

class CloudRenderer {
public:
    // Pipelines, samplers and the 2D noise; false when one fails (logged): no clouds. `pipelines` counts the ones
    // made. The 3D noise is made when volumetric clouds are first drawn.
    bool init(gpu::Device& device, gpu::TextureFormat hdrFormat, gpu::TextureFormat depthFormat,
              std::uint32_t& pipelines);
    bool ready() const { return static_cast<bool>(m_skyLayerPipeline); }
    // What the device can do (before init): sample an Rgba8Unorm 3D texture, render to an Rgba8Unorm 2D array (its
    // layers are copied into the 3D noise) and to Rgba16Float targets (the volumetric clouds); render to and sample an
    // Rgba16Float cube (the sky cube).
    static bool volumeSupported(gpu::Device& device);
    static bool cubeSupported(gpu::Device& device);

    struct Frame {
        const weather::Weather* weather = nullptr;
        Clouds mode = Clouds::Off;
        bool cube = false;
        std::uint32_t renderWidth = 0, renderHeight = 0;
        glm::mat4 viewProjStable{1.0f};
        glm::mat4 prevViewProjStable{1.0f};
        bool cut = false;
        std::uint64_t sequence = 0; // frames since the last cut
    };
    // Outside passes, before the HDR pass: the cloud passes with `fu` in fragment slot 0 (pushed here), then the
    // cloud fields of `fu` (cloudShadow, skyCube, overcast) and the light under the clouds (its ambient and fog
    // colors) for the rest of the frame. Fills the clouds' numbers of `stats`.
    void render(gpu::CommandBuffer& commands, const RenderSettings& settings, const Frame& frame,
                shader::FrameUniforms& fu, RenderStats& stats);
    // In the HDR pass in place of the plain sky (after render() drew this frame's clouds).
    bool drawsSky() const { return m_skyMode != Clouds::Off; }
    void drawSky(gpu::RenderPass& pass);
    // Surface samplers 14 (the cloud shadow map) and 15 (the sky cube); placeholders when there are none.
    gpu::TextureSamplerBinding shadowBinding(const gpu::Texture& white, const gpu::Sampler& sampler) const;
    gpu::TextureSamplerBinding cubeBinding(const gpu::Texture& noCube, const gpu::Sampler& sampler) const;
    // Frees the targets and the 3D noise (clouds off).
    void release();

private:
    bool ensureNoise(gpu::CommandBuffer& commands);
    bool ensureTargets(std::uint32_t width, std::uint32_t height);
    bool ensureCube(std::uint32_t size);
    bool createTexture(gpu::Texture& out, const gpu::TextureDesc& desc);

    gpu::Device* m_device = nullptr;
    gpu::TextureFormat m_shadowFormat = gpu::TextureFormat::Invalid;
    gpu::TextureFormat m_depthFormat = gpu::TextureFormat::Invalid;
    gpu::Sampler m_weatherSampler, m_noiseSampler, m_cubeSampler, m_pointSampler, m_linearSampler;
    gpu::GraphicsPipeline m_noisePipeline, m_weatherPipeline, m_shadowPipeline, m_shadowLayerPipeline, m_marchPipeline,
        m_temporalPipeline, m_skyPipeline, m_skyLayerPipeline, m_cubePipeline, m_cubeLayerPipeline;
    gpu::Texture m_layerNoise, m_shapeNoise, m_detailNoise;
    bool m_noiseFailed = false;
    gpu::Texture m_weatherMap, m_shadowMap;
    glm::vec2 m_weatherOrigin{0.0f}, m_shadowOrigin{0.0f}; // of the maps' contents
    bool m_weatherValid = false, m_shadowValid = false;
    std::uint64_t m_frame = 0; // the maps are drawn in turns
    gpu::Texture m_march, m_marchDepth;
    std::array<gpu::Texture, 2> m_history;
    int m_historyIndex = 0;
    bool m_historyValid = false;
    std::uint32_t m_width = 0, m_height = 0;
    gpu::Texture m_cube;
    std::uint32_t m_cubeSize = 0;
    int m_cubeFace = 0;              // the next face to draw
    int m_cubeFacesDrawn = 0;        // since the cube was made: valid at 6
    Clouds m_skyMode = Clouds::Off;  // what drawSky draws this frame
    Clouds m_lastMode = Clouds::Off; // last frame's (the history and cube belong to it)
    bool m_shadows = false;
    shader::CloudUniforms m_uniforms{};
};

} // namespace quench::render::detail
