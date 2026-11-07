#include "Rain.h"

#include <quench/core/Log.h>

#include <render_frame_shaders.h>
#include <render_rain_shaders.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace quench::render::detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureType;
using gpu::TextureUsage;

constexpr ShaderStage kVertex = ShaderStage::Vertex;
constexpr ShaderStage kFragment = ShaderStage::Fragment;
// The occlusion map: 0.25 m texels over 128 m around the camera, its top 100 m above the camera along the rain and its
// depth 200 m (the region's half floats keep about 5 cm there). Casters above its top are clamped to it where the
// device has depth clamp; the ground more than 100 m below the camera is out of it (open).
constexpr std::uint32_t kMapSize = QUENCH_WETNESS_ROW - QUENCH_RAIN_MAP_ROW;
constexpr float kMapExtent = 128.0f;
constexpr float kMapAbove = 100.0f;
constexpr float kMapDepth = 200.0f;
// The wind the rain falls with is the wind at this height (the drops near the ground keep their speed from above).
constexpr float kWindHeight = 10.0f;
// The streaks: the distance a drop falls in this exposure (0.3 m at 7.5 m/s), this wide (a drop's width with its
// blur), this opaque at full rain; the light a drop sends toward the camera: the sky's radiance (the ambient terms
// are the radiance a white diffuse surface sends back) refracted from the wide cone above it, kSkyShare of it sky and
// the rest ground, and the sun's times kSunForward in a forward lobe.
constexpr float kStreakExposure = 0.04f;
constexpr float kStreakWidth = 0.006f;
constexpr float kStreakOpacity = 0.8f;
constexpr float kSkyShare = 0.7f;
constexpr float kSunForward = 0.06f;
// The splashes: each lives this long, this big.
constexpr float kSplashLife = 0.25f;
constexpr float kSplashSize = 0.12f;
// The shafts: the extinction of rain at precipitation 1 (a visibility of about 2.5 km, 5 km at 0.5), marched from
// this distance (nearer is the streaks'); the rainbow's strength against the sun's light the drops scatter evenly.
constexpr float kShaftExtinction = 0.0012f;
constexpr float kShaftNear = 20.0f;
constexpr float kRainbowStrength = 0.06f;
// The cover over the camera: the texels within this many of the camera's (0.75 m), covered where the highest surface
// is more than kCoverHeight above the camera along the rain.
constexpr int kCoverRadius = 3;
constexpr std::uint32_t kCoverSide = 2 * kCoverRadius + 1;
constexpr float kCoverHeight = 0.5f;

struct Quality {
    std::uint32_t streaks;
    float box; // half side (m)
    std::uint32_t splashes;
    float splashRadius; // m
    int shaftSteps;
    std::uint32_t shaftScale; // render pixels per shaft texel along each axis
};
constexpr Quality kQualities[] = {
    {0, 0.0f, 0, 0.0f, 0, 1},           // Off
    {8192, 9.0f, 0, 0.0f, 12, 4},       // Low
    {24576, 12.0f, 1024, 10.0f, 20, 2}, // Medium
    {49152, 15.0f, 3072, 15.0f, 32, 2}, // High
};

const Quality& qualityOf(Rain rain) { return kQualities[std::size_t(rain)]; }

} // namespace

bool RainRenderer::supported(gpu::Device& device, TextureFormat depthFormat) {
    return depthFormat != TextureFormat::Invalid &&
           device.supportsFormat(kWeatherMapsFormat, TextureType::Texture2D,
                                 TextureUsage::ColorTarget | TextureUsage::Sampled) &&
           device.supportsFormat(depthFormat, TextureType::Texture2D,
                                 TextureUsage::DepthStencilTarget | TextureUsage::Sampled);
}

bool RainRenderer::init(gpu::Device& device, TextureFormat hdrFormat, TextureFormat depthFormat,
                        std::uint32_t& pipelines) {
    m_device = &device;
    m_hdrFormat = hdrFormat;
    m_depthFormat = depthFormat;
    const auto& rain = shaders::render_rain;
    const ShaderBlob* fullscreen = shaders::render_frame.find("fullscreen_vs");
    const auto make = [&](gpu::GraphicsPipeline& out, const ShaderBlob* vertex, const char* fragment,
                          const gpu::ColorTargetDesc& target, const char* name) {
        const ShaderBlob* shader = rain.find(fragment);
        if (!vertex || !shader) {
            log::error("render", "pipeline '{}': the shader is not in this build", name);
            return false;
        }
        Result<gpu::GraphicsPipeline> p = device.createPipeline(
            {.vertexShader = vertex, .fragmentShader = shader, .colorTargets = {&target, 1}, .name = name});
        if (!p) {
            log::error("render", "pipeline '{}': {}", name, p.error());
            return false;
        }
        out = std::move(*p);
        ++pipelines;
        return true;
    };
    // The drops over the image without changing its alpha; the shafts (marched into a target of their own) as
    // color + destination * transmittance.
    gpu::BlendState drops = gpu::BlendState::premultiplied();
    drops.writeMask = gpu::ColorWrite::R | gpu::ColorWrite::G | gpu::ColorWrite::B;
    const gpu::BlendState shafts{true,
                                 gpu::BlendFactor::One,
                                 gpu::BlendFactor::SrcAlpha,
                                 gpu::BlendOp::Add,
                                 gpu::BlendFactor::Zero,
                                 gpu::BlendFactor::One,
                                 gpu::BlendOp::Add,
                                 gpu::ColorWrite::R | gpu::ColorWrite::G | gpu::ColorWrite::B};
    bool ok =
        make(m_mapPipeline, fullscreen, "rain_map_fs", {kWeatherMapsFormat}, "render rain map") &&
        make(m_streakPipeline, rain.find("rain_streak_vs"), "rain_streak_fs", {hdrFormat, drops},
             "render rain streaks") &&
        make(m_splashPipeline, rain.find("rain_splash_vs"), "rain_splash_fs", {hdrFormat, drops},
             "render rain splashes") &&
        make(m_shaftsPipeline, fullscreen, "rain_shafts_fs", {kWeatherMapsFormat}, "render rain shafts") &&
        make(m_shaftsUpPipeline, fullscreen, "rain_shafts_up_fs", {hdrFormat, shafts}, "render rain shafts upsample");
    if (ok) {
        Result<gpu::TransferBuffer> grid = device.createTransferBuffer(
            {.size = QUENCH_WETNESS_GRID * QUENCH_WETNESS_GRID * 8, .name = "render wetness grid"});
        ok = static_cast<bool>(grid);
        if (grid)
            m_gridTransfer = std::move(*grid);
        else
            log::error("render", "wetness grid transfer buffer: {}", grid.error());
    }
    if (!ok) {
        *this = RainRenderer{};
        log::error("render", "rain pipelines failed (logged); no rain");
        return false;
    }
    return true;
}

void RainRenderer::release() {
    m_occlusion = {};
    m_image = {};
    m_shafts = {};
    m_gridVersion = ~0ull;
    m_lastMapValid = false;
    m_readSerial = m_readbackSerial; // results still on their way are left unread
    m_cover = -1.0f;
    m_quality = Rain::Off;
}

bool RainRenderer::prepare(const Frame& frame, shader::FrameUniforms& fu, RenderStats& stats) {
    fu.rain = glm::vec4(0.0f);
    m_quality = Rain::Off;
    if (!frame.weather || frame.quality == Rain::Off || !ready()) {
        if (m_occlusion) release();
        return false;
    }
    if (!m_occlusion) {
        Result<gpu::Texture> t =
            m_device->createTexture({.format = m_depthFormat,
                                     .usage = TextureUsage::DepthStencilTarget | TextureUsage::Sampled,
                                     .width = kMapSize,
                                     .height = kMapSize,
                                     .name = "render rain occlusion"});
        if (!t) {
            log::error("render", "rain occlusion map {} x {}: {}; no rain", kMapSize, kMapSize, t.error());
            m_streakPipeline = {};
            return false;
        }
        m_occlusion = std::move(*t);
    }
    const weather::Weather& weather = *frame.weather;
    const glm::vec3 camera = frame.camera;
    m_camera = camera;
    m_quality = frame.quality;
    // Along the rain at the camera, on the map's texel grid (it does not swim as the camera moves).
    m_velocity = weather.rainVelocity({camera.x, kWindHeight, camera.z});
    const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), glm::normalize(m_velocity), glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::vec3 c(view * glm::vec4(camera, 1.0f));
    const float texel = kMapExtent / float(kMapSize);
    const glm::vec2 centre = glm::floor(glm::vec2(c) / texel + 0.5f) * texel;
    const float near = -c.z - kMapAbove;
    const float half = kMapExtent * 0.5f;
    m_viewProj =
        glm::orthoRH_ZO(centre.x - half, centre.x + half, centre.y - half, centre.y + half, near, near + kMapDepth) *
        view;
    fu.rainViewProj = m_viewProj;
    fu.rain = glm::vec4(1.0f, kMapDepth, weather.precipitation(camera), 1.0f);
    fu.rainGrid = glm::vec4(weather.wetnessOrigin(),
                            1.0f / (float(weather::Weather::kWetnessGrid) * weather::Weather::kWetnessCell), 0.0f);

    const Quality& q = qualityOf(frame.quality);
    m_uniforms = {};
    m_uniforms.invRainViewProj = glm::inverse(m_viewProj);
    m_uniforms.box = glm::vec4(camera + glm::vec3(0.0f, q.box * 0.3f, 0.0f), q.box);
    m_uniforms.fall = glm::vec4(m_velocity, float(std::fmod(frame.time, 4096.0)));
    m_uniforms.streaks = glm::vec4(float(q.streaks), kStreakExposure, kStreakWidth, kStreakOpacity);
    m_uniforms.splashes = glm::vec4(float(q.splashes), q.splashRadius, kSplashLife, kSplashSize);
    stats.rain = frame.quality;
    stats.rainAtCamera = fu.rain.z;
    stats.rainCover = m_cover;
    return true;
}

void RainRenderer::readCover(RenderStats& stats) {
    for (Readback& r : m_readbacks) {
        if (!r.pending || !m_device->isSignaled(r.fence)) continue;
        r.pending = false;
        r.fence = {};
        if (r.serial <= m_readSerial) continue;
        const std::span<std::byte> data = r.buffer.map(false);
        if (data.size() >= std::size_t(kCoverSide * kCoverSide * 8)) {
            float covered = 0.0f, total = 0.0f;
            for (int y = 0; y < int(kCoverSide); ++y)
                for (int x = 0; x < int(kCoverSide); ++x) {
                    const int dx = x - kCoverRadius, dy = y - kCoverRadius;
                    if (dx * dx + dy * dy > kCoverRadius * kCoverRadius + 1) continue;
                    std::uint16_t half = 0;
                    std::memcpy(&half, data.data() + (std::size_t(y) * kCoverSide + std::size_t(x)) * 8, 2);
                    const float top = glm::unpackHalf1x16(half);
                    covered += (r.cameraDepth - top) * kMapDepth > kCoverHeight ? 1.0f : 0.0f;
                    total += 1.0f;
                }
            m_cover = total > 0.0f ? covered / total : 0.0f;
            m_readSerial = r.serial;
        }
        r.buffer.unmap();
    }
    stats.rainCover = m_cover;
}

void RainRenderer::updateMaps(gpu::CommandBuffer& commands, const gpu::Texture& maps, const weather::Weather& weather,
                              const gpu::Sampler& pointSampler, RenderStats& stats) {
    readCover(stats);
    // Last frame's texels around the camera, in a command buffer submitted before this frame's: read a few frames
    // later, without waiting.
    if (m_lastMapValid && (m_frame++ & 1u) == 0) {
        Readback* slot = nullptr;
        for (Readback& r : m_readbacks)
            if (!r.pending) slot = &r;
        if (slot && !slot->buffer) {
            Result<gpu::TransferBuffer> b =
                m_device->createTransferBuffer({.direction = gpu::TransferDirection::Download,
                                                .size = kCoverSide * kCoverSide * 8,
                                                .name = "render rain cover"});
            if (b) slot->buffer = std::move(*b);
        }
        if (slot && slot->buffer) {
            if (gpu::CommandBuffer own = m_device->acquireCommandBuffer()) {
                const auto first = [](std::uint32_t texel) {
                    return std::min(std::uint32_t(std::max(int(texel) - kCoverRadius, 0)), kMapSize - kCoverSide);
                };
                {
                    gpu::CopyPass copy = own.beginCopyPass();
                    copy.download({.texture = maps,
                                   .x = first(m_lastCameraTexel.x),
                                   .y = QUENCH_RAIN_MAP_ROW + first(m_lastCameraTexel.y),
                                   .width = kCoverSide,
                                   .height = kCoverSide},
                                  slot->buffer, 0);
                }
                if (Result<gpu::Fence> fence = own.submitWithFence()) {
                    slot->fence = std::move(*fence);
                    slot->pending = true;
                    slot->cameraDepth = m_lastCameraDepth;
                    slot->serial = ++m_readbackSerial;
                }
            }
        }
    }

    // The occlusion map into its region.
    if (gpu::RenderPass pass = commands.beginRenderPass(gpu::ColorTarget{.texture = maps, .load = gpu::LoadOp::Load})) {
        pass.setViewport({0.0f, float(QUENCH_RAIN_MAP_ROW), float(kMapSize), float(kMapSize)});
        pass.setScissor({0, QUENCH_RAIN_MAP_ROW, int(kMapSize), int(kMapSize)});
        pass.bindPipeline(m_mapPipeline);
        pass.bindSampler(kFragment, 0, {m_occlusion, pointSampler});
        pass.draw(3);
        pass.end();
    }
    const glm::vec4 c = m_viewProj * glm::vec4(m_camera, 1.0f);
    m_lastCameraTexel = glm::uvec2(glm::clamp(glm::vec2(c.x * 0.5f + 0.5f, 0.5f - c.y * 0.5f) * float(kMapSize),
                                              glm::vec2(0.0f), glm::vec2(float(kMapSize - 1))));
    m_lastCameraDepth = c.z;
    m_lastMapValid = true;

    // The wetness grid into its region when the weather has changed it.
    if (weather.wetnessVersion() != m_gridVersion) {
        const std::span<const weather::WetnessCell> grid = weather.wetnessGrid();
        const std::span<std::byte> mapped = m_gridTransfer.map(true);
        if (mapped.size() >= grid.size() * 8) {
            for (std::size_t i = 0; i < grid.size(); ++i) {
                const std::uint16_t texel[4] = {glm::packHalf1x16(grid[i].wetness.ground),
                                                glm::packHalf1x16(grid[i].wetness.water),
                                                glm::packHalf1x16(grid[i].precipitation), glm::packHalf1x16(1.0f)};
                std::memcpy(mapped.data() + i * 8, texel, 8);
            }
        }
        m_gridTransfer.unmap();
        gpu::CopyPass copy = commands.beginCopyPass();
        copy.upload(m_gridTransfer, 0,
                    {.texture = maps,
                     .x = 0,
                     .y = QUENCH_WETNESS_ROW,
                     .width = QUENCH_WETNESS_GRID,
                     .height = QUENCH_WETNESS_GRID});
        m_gridVersion = weather.wetnessVersion();
    }
}

void RainRenderer::lightUniforms(const shader::FrameUniforms& fu) {
    const glm::vec3 sky(fu.skyAmbient), ground(fu.groundAmbient);
    m_uniforms.light = glm::vec4(glm::mix(ground, sky, kSkyShare), m_uniforms.light.w);
    m_uniforms.sun =
        fu.sunDir.w > 0.0f && fu.sunDir.y > 0.0f ? glm::vec4(glm::vec3(fu.sunColor), kSunForward) : glm::vec4(0.0f);
}

void RainRenderer::drawShafts(gpu::CommandBuffer& commands, const Shafts& shafts, const gpu::Texture& maps,
                              const gpu::Sampler& pointSampler, const gpu::Sampler& linearSampler,
                              const shader::FrameUniforms& fu) {
    if (m_quality == Rain::Off || !shafts.depth) return;
    const Quality& q = qualityOf(m_quality);
    const std::uint32_t w = (shafts.width + q.shaftScale - 1) / q.shaftScale;
    const std::uint32_t h = (shafts.height + q.shaftScale - 1) / q.shaftScale;
    if (!m_shafts || m_shafts.desc().width != w || m_shafts.desc().height != h) {
        m_shafts = {};
        Result<gpu::Texture> t = m_device->createTexture({.format = kWeatherMapsFormat,
                                                          .usage = TextureUsage::ColorTarget | TextureUsage::Sampled,
                                                          .width = w,
                                                          .height = h,
                                                          .name = "render rain shafts"});
        if (!t) {
            log::error("render", "rain shafts {} x {}: {}; no shafts", w, h, t.error());
            return;
        }
        m_shafts = std::move(*t);
    }
    lightUniforms(fu);
    m_uniforms.shafts = glm::vec4(float(q.shaftSteps), shafts.cloudBase, kShaftExtinction, kShaftNear);
    m_uniforms.weatherMap = shafts.weatherMapUv;
    m_uniforms.rainbow = glm::vec4(shafts.rainbow ? 1.0f : 0.0f, kRainbowStrength,
                                   float(std::fmod(double(shafts.sequence) * 0.6180339887, 1.0)), float(q.shaftScale));
    m_uniforms.target =
        glm::vec4(float(shafts.width), float(shafts.height), 1.0f / float(shafts.width), 1.0f / float(shafts.height));
    if (gpu::RenderPass pass = commands.beginRenderPass(
            gpu::ColorTarget{.texture = m_shafts, .load = gpu::LoadOp::DontCare, .cycle = true})) {
        const gpu::TextureSamplerBinding bindings[] = {
            {*shafts.depth, pointSampler}, shafts.weatherMap, {maps, linearSampler}};
        pass.bindPipeline(m_shaftsPipeline);
        pass.pushUniforms(kFragment, 0, fu);
        pass.pushUniforms(kFragment, 1, m_uniforms);
        pass.bindSamplers(kFragment, 0, bindings);
        pass.draw(3);
        pass.end();
    }
    if (gpu::RenderPass pass =
            commands.beginRenderPass(gpu::ColorTarget{.texture = shafts.hdr, .load = gpu::LoadOp::Load})) {
        const gpu::TextureSamplerBinding bindings[] = {{*shafts.depth, pointSampler}, {m_shafts, pointSampler}};
        pass.bindPipeline(m_shaftsUpPipeline);
        pass.pushUniforms(kFragment, 0, fu);
        pass.pushUniforms(kFragment, 1, m_uniforms);
        pass.bindSamplers(kFragment, 0, bindings);
        pass.draw(3);
        pass.end();
    }
}

gpu::TextureRef RainRenderer::drawDrops(gpu::CommandBuffer& commands, const Drops& drops, const gpu::Texture& maps,
                                        const gpu::Sampler& pointSampler, const gpu::Sampler& linearSampler,
                                        const shader::FrameUniforms& fu, RenderStats& stats) {
    gpu::TextureRef image = drops.image;
    if (m_quality == Rain::Off || !drops.depth || !image) return image;
    const std::uint32_t w = image.desc.width, h = image.desc.height;
    if (drops.keepImage) {
        if (!m_image || m_image.desc().width != w || m_image.desc().height != h) {
            m_image = {};
            Result<gpu::Texture> t =
                m_device->createTexture({.format = m_hdrFormat,
                                         .usage = TextureUsage::ColorTarget | TextureUsage::Sampled,
                                         .width = w,
                                         .height = h,
                                         .name = "render rain image"});
            if (!t) {
                log::error("render", "rain image {} x {}: {}; no streaks", w, h, t.error());
                return image;
            }
            m_image = std::move(*t);
        }
        {
            gpu::CopyPass copy = commands.beginCopyPass();
            copy.copy({.texture = image}, {.texture = m_image});
        }
        image = m_image;
    } else {
        m_image = {};
    }
    const Quality& q = qualityOf(m_quality);
    lightUniforms(fu);
    m_uniforms.light.w = drops.proj[1][1] * float(h) * 0.5f;
    m_uniforms.viewProjStable = drops.viewProjStable;
    m_uniforms.target = glm::vec4(float(w), float(h), 1.0f / float(w), 1.0f / float(h));
    gpu::RenderPass pass = commands.beginRenderPass(gpu::ColorTarget{.texture = image, .load = gpu::LoadOp::Load});
    if (!pass) return image;
    pass.pushUniforms(kVertex, 0, fu);
    pass.pushUniforms(kFragment, 0, fu);
    pass.pushUniforms(kVertex, 1, m_uniforms);
    pass.pushUniforms(kFragment, 1, m_uniforms);
    pass.bindPipeline(m_streakPipeline);
    pass.bindSampler(kVertex, 0, {maps, linearSampler});
    pass.bindSampler(kFragment, 0, {*drops.depth, pointSampler});
    pass.draw(6, q.streaks);
    if (q.splashes > 0) {
        pass.bindPipeline(m_splashPipeline);
        pass.bindSampler(kVertex, 0, {maps, linearSampler});
        pass.bindSampler(kFragment, 0, {*drops.depth, pointSampler});
        pass.draw(6, q.splashes);
    }
    pass.end();
    stats.rainStreaks = q.streaks;
    stats.rainSplashes = q.splashes;
    return image;
}

} // namespace quench::render::detail
