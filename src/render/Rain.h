#pragma once

// The rain and snow (RenderSettings::rain and snow, docs/weather.md, "Rain" and "Snow"): every frame with a weather
// and either on, the rain occlusion map around the camera (the depth along the rain, or the snow, of the highest
// surfaces: the renderer draws its shadow casters into occlusion() with viewProj()), copied with the weather's wetness
// grid and snow cover into the weather maps that surfaces read; the shafts of rain and snow under the clouds and the
// rainbow in the HDR image before TAA; the streaks and splashes of the drops and the snow's flakes near the camera
// after TAA and upscaling; and the cover over the camera, read back a few frames late for the weather's sound
// (RenderStats::rainCover).

#include "shaders/RainUniforms.h"

#include <quench/SceneUniforms.h>
#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Device.h>
#include <quench/render/Renderer.h>
#include <quench/weather/Weather.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace quench::render::detail {

// The weather maps (surface sampler 14, QUENCH_WEATHER_MAPS_* in quench/SceneUniforms.h): the clouds' shadow, the
// rain occlusion map and the wetness grid, regions of one texture.
inline constexpr gpu::TextureFormat kWeatherMapsFormat = gpu::TextureFormat::Rgba16Float;

class RainRenderer {
public:
    // What the device can do: render to and sample kWeatherMapsFormat, sample a 2D depth texture of `depthFormat`.
    static bool supported(gpu::Device& device, gpu::TextureFormat depthFormat);
    // Pipelines and samplers; false when one fails (logged): no rain. `pipelines` counts the ones made.
    bool init(gpu::Device& device, gpu::TextureFormat hdrFormat, gpu::TextureFormat depthFormat,
              std::uint32_t& pipelines);
    bool ready() const { return static_cast<bool>(m_streakPipeline); }

    struct Frame {
        const weather::Weather* weather = nullptr;
        Rain quality = Rain::Off;
        Snow snow = Snow::Off;
        glm::vec3 camera{0.0f};
        double time = 0.0; // View::time: the drops' fall and the ripples
    };
    // Before the frame's draw lists: places this frame's occlusion map and fills the rain's and snow's fields of `fu`
    // (rain.x 0 without either) and of `stats`. True when rain or snow is drawn this frame: the renderer then draws
    // its casters into occlusion() with viewProj() and calls the passes below.
    bool prepare(const Frame& frame, shader::FrameUniforms& fu, RenderStats& stats);
    const glm::mat4& viewProj() const { return m_viewProj; }
    const gpu::Texture& occlusion() const { return m_occlusion; }
    // The snow's flakes are drawn this frame (drawFlakes): the snow is on and some falls near the camera.
    bool snowing() const { return m_flakesNear; }

    // Outside passes, after the casters were drawn: the occlusion map and the wetness grid into their regions of
    // `maps`, and the cover over the camera read back.
    void updateMaps(gpu::CommandBuffer& commands, const gpu::Texture& maps, const weather::Weather& weather,
                    const gpu::Sampler& pointSampler, RenderStats& stats);

    struct Shafts {
        gpu::TextureRef hdr;
        std::uint32_t width = 0, height = 0;
        const gpu::Texture* depth = nullptr;
        gpu::TextureSamplerBinding weatherMap; // the clouds' (precipitation in b)
        glm::vec4 weatherMapUv{0.0f};          // xy: its corner (world xz), z: 1 / its size, w: texels per metre
        float cloudBase = 1500.0f;
        bool rainbow = false;
        std::uint64_t sequence = 0;
    };
    // Outside passes, after the HDR pass: the shafts and the rainbow, marched at a reduced size and blended over the
    // HDR image.
    void drawShafts(gpu::CommandBuffer& commands, const Shafts& shafts, const gpu::Texture& maps,
                    const gpu::Sampler& pointSampler, const gpu::Sampler& linearSampler,
                    const shader::FrameUniforms& fu);

    struct Drops {
        gpu::TextureRef image;               // after TAA and upscaling
        bool keepImage = false;              // the image is read again (TAA's history): draw into a copy
        const gpu::Texture* depth = nullptr; // the scene's (render size)
        glm::mat4 viewProjStable{1.0f};
        glm::mat4 proj{1.0f};
    };
    // Outside passes, after TAA and upscaling: the streaks and splashes; returns the image they were drawn into (the
    // flakes' too, also without rain).
    gpu::TextureRef drawDrops(gpu::CommandBuffer& commands, const Drops& drops, const gpu::Texture& maps,
                              const gpu::Sampler& pointSampler, const gpu::Sampler& linearSampler,
                              const shader::FrameUniforms& fu, RenderStats& stats);
    // Then the snow's flakes into drawDrops' image (drops.keepImage is not read).
    void drawFlakes(gpu::CommandBuffer& commands, const Drops& drops, const gpu::Texture& maps,
                    const gpu::Sampler& pointSampler, const gpu::Sampler& linearSampler,
                    const shader::FrameUniforms& fu, RenderStats& stats);

    // Frees the targets (rain off).
    void release();

private:
    void readCover(RenderStats& stats);
    // The light of the drops, the veil and the flakes; deck: the share of the overcast's mean radiance they see.
    void lightUniforms(const shader::FrameUniforms& fu, float deck);

    gpu::Device* m_device = nullptr;
    gpu::TextureFormat m_depthFormat = gpu::TextureFormat::Invalid;
    gpu::TextureFormat m_hdrFormat = gpu::TextureFormat::Invalid;
    gpu::GraphicsPipeline m_mapPipeline, m_streakPipeline, m_splashPipeline, m_flakePipeline, m_shaftsPipeline,
        m_shaftsBlurPipeline, m_shaftsUpPipeline;
    gpu::Texture m_occlusion;
    gpu::Texture m_image;        // the copy drawn into when the image must stay as it is
    gpu::Texture m_shafts;       // the shafts at a reduced size (color, transmittance)
    gpu::Texture m_shaftsSmooth; // the same, smoothed
    gpu::TransferBuffer m_gridTransfer;
    std::uint64_t m_gridVersion = ~0ull;
    glm::vec2 m_gridOrigin{0.0f};
    // The cover over the camera: a few texels of last frame's occlusion map around the camera, copied in a command
    // buffer of its own and read once its fence is signalled.
    struct Readback {
        gpu::TransferBuffer buffer;
        gpu::Fence fence;
        float cameraDepth = 0.0f; // the camera's depth (0..1) in the map the texels come from
        bool pending = false;
        std::uint64_t serial = 0;
    };
    std::array<Readback, 3> m_readbacks;
    std::uint64_t m_readbackSerial = 0, m_readSerial = 0;
    glm::uvec2 m_lastCameraTexel{0u};
    float m_lastCameraDepth = 0.0f;
    bool m_lastMapValid = false;
    float m_cover = -1.0f;
    std::uint64_t m_frame = 0;

    Rain m_quality = Rain::Off;
    Snow m_snow = Snow::Off;
    float m_snowShare = 0.0f; // of the precipitation at the camera, where the snow is drawn
    // The streaks and splashes, and the flakes, are drawn: on, and some of their kind falls near the camera.
    bool m_dropsNear = false, m_flakesNear = false;
    shader::WeatherUniforms m_field{};
    glm::vec3 m_camera{0.0f};
    glm::mat4 m_viewProj{1.0f};
    glm::vec3 m_velocity{0.0f};
    shader::RainUniforms m_uniforms{};
};

} // namespace quench::render::detail
