#include "Clouds.h"

#include "Rain.h"

#include <quench/core/Log.h>

#include <render_cloudnoise_shaders.h>
#include <render_clouds_shaders.h>
#include <render_frame_shaders.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace quench::render::detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureType;
using gpu::TextureUsage;

constexpr ShaderStage kFragment = ShaderStage::Fragment;
constexpr TextureUsage kTargetSampled = TextureUsage::ColorTarget | TextureUsage::Sampled;
constexpr TextureFormat kNoiseFormat = TextureFormat::Rgba8Unorm;
constexpr TextureFormat kWeatherFormat = TextureFormat::Rgba8Unorm;
constexpr TextureFormat kCloudFormat = TextureFormat::Rgba16Float; // premultiplied radiance and transmittance
// The noise (cloudnoise.hlsl): the shapes, their erosion and the 2D layer's.
constexpr std::uint32_t kShapeSize = 128;
constexpr std::uint32_t kDetailSize = 32;
constexpr std::uint32_t kLayerSize = 256;
// The weather map: 250 m texels over 128 km around the camera (clouds farther away fade out; a cloud base at 1.5 km
// meets the horizon at about 140 km); the shadow map, the first region of the weather maps: about 47 m texels over
// 24 km (shadows farther away take its edge).
constexpr std::uint32_t kWeatherMapSize = 512;
constexpr float kWeatherExtent = 128000.0f;
constexpr std::uint32_t kShadowMapSize = QUENCH_RAIN_MAP_ROW;
constexpr float kShadowExtent = 24000.0f;
constexpr float kPlanetRadius = 6360000.0f;
// One tile of the shape noise over 6 km (cells of 1.5 km: cumulus 0.5 to 2 km across), of the detail over 800 m; the
// extinction at density 1 (a mean free path of 25 m: a cloud's core is opaque, its edges let the sun through).
constexpr float kShapeScale = 1.0f / 6000.0f;
constexpr float kDetailScale = 1.0f / 800.0f;
constexpr float kExtinction = 0.04f;
constexpr float kLongestPath = 60000.0f; // m of a ray inside the layer
constexpr float kCurrentWeight = 0.1f;   // of a new frame in the accumulation
constexpr int kCubeSteps = 24;
constexpr int kCubeLightSteps = 3;
// How overcast it is: the mean coverage within this distance of the camera.
constexpr float kCoverRadius = 6000.0f;
// The share of the light above a closed cloud deck that reaches the ground, all of it diffuse.
constexpr float kOvercastTransmittance = 0.3f;
constexpr float kPi = 3.14159265f;
// The lightning (RenderSettings::lightning): flashes within this distance of the camera are lit; a flash's light on
// the scene halves at kFlashNear, its glow in the clouds at kGlowNear; the glow sits this far above the cloud base.
// Its light on the scene, as irradiance from the sky at a flash of 1: a fixed amount and a share of the light under
// the clouds; its glow's radiance: a fixed amount and a share of the cloud base's.
constexpr float kLightningRadius = 30000.0f;
constexpr float kFlashNear = 2000.0f;
constexpr float kGlowNear = 6000.0f;
constexpr float kFlashAboveBase = 800.0f;
constexpr float kFlashIrradiance = 0.6f;
constexpr float kGlowRadiance = 0.3f;
const glm::vec3 kLightningColor{0.85f, 0.9f, 1.0f};

// The corner of a map of `extent` around `centre`, on its texel grid (the map does not swim as the camera moves).
glm::vec2 snapped(glm::vec2 centre, float extent, std::uint32_t size) {
    const float texel = extent / float(size);
    return glm::floor((centre - extent * 0.5f) / texel) * texel;
}

float luminance(const glm::vec3& c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); }

} // namespace

// The 3D noise is drawn into the layers of a 2D array and copied into the slices of a 3D texture: rendering into a 3D
// texture's slices needs 2D views of it, which MoltenVK does not have (its portability subset).
bool CloudRenderer::volumeSupported(gpu::Device& device) {
    return device.supportsFormat(kNoiseFormat, TextureType::Texture3D, TextureUsage::Sampled) &&
           device.supportsFormat(kNoiseFormat, TextureType::Texture2DArray, TextureUsage::ColorTarget) &&
           device.supportsFormat(kCloudFormat, TextureType::Texture2D, kTargetSampled);
}

bool CloudRenderer::cubeSupported(gpu::Device& device) {
    return device.supportsFormat(kCloudFormat, TextureType::Cube, kTargetSampled);
}

bool CloudRenderer::createTexture(gpu::Texture& out, const gpu::TextureDesc& desc) {
    Result<gpu::Texture> t = m_device->createTexture(desc);
    if (!t) {
        log::error("render", "texture '{}' {} x {} x {}: {}", desc.name, desc.width, desc.height, desc.layers,
                   t.error());
        return false;
    }
    out = std::move(*t);
    return true;
}

bool CloudRenderer::init(gpu::Device& device, TextureFormat hdrFormat, TextureFormat sceneDepthFormat,
                         std::uint32_t& pipelines) {
    m_device = &device;
    const auto supports = [&](TextureFormat f) {
        return device.supportsFormat(f, TextureType::Texture2D, kTargetSampled);
    };
    m_depthFormat = supports(TextureFormat::R16Float) ? TextureFormat::R16Float : TextureFormat::Rgba16Float;
    const bool volume = volumeSupported(device);

    struct SamplerSpec {
        gpu::Sampler* out;
        gpu::SamplerDesc desc;
    };
    const SamplerSpec samplers[] = {
        {&m_weatherSampler,
         {.addressU = gpu::AddressMode::ClampToEdge,
          .addressV = gpu::AddressMode::ClampToEdge,
          .addressW = gpu::AddressMode::ClampToEdge,
          .name = "render clouds weather"}},
        {&m_noiseSampler, {.mipmapMode = gpu::MipmapMode::Nearest, .maxLod = 0.0f, .name = "render clouds noise"}},
        {&m_cubeSampler,
         {.addressU = gpu::AddressMode::ClampToEdge,
          .addressV = gpu::AddressMode::ClampToEdge,
          .addressW = gpu::AddressMode::ClampToEdge,
          .name = "render sky cube"}},
        {&m_pointSampler,
         {.minFilter = gpu::Filter::Nearest,
          .magFilter = gpu::Filter::Nearest,
          .mipmapMode = gpu::MipmapMode::Nearest,
          .addressU = gpu::AddressMode::ClampToEdge,
          .addressV = gpu::AddressMode::ClampToEdge,
          .addressW = gpu::AddressMode::ClampToEdge,
          .maxLod = 0.0f,
          .name = "render clouds point"}},
        {&m_linearSampler,
         {.mipmapMode = gpu::MipmapMode::Nearest,
          .addressU = gpu::AddressMode::ClampToEdge,
          .addressV = gpu::AddressMode::ClampToEdge,
          .addressW = gpu::AddressMode::ClampToEdge,
          .maxLod = 0.0f,
          .name = "render clouds linear"}},
    };
    for (const SamplerSpec& s : samplers) {
        Result<gpu::Sampler> sampler = device.createSampler(s.desc);
        if (!sampler) {
            log::error("render", "sampler '{}': {}; no clouds", s.desc.name, sampler.error());
            return false;
        }
        *s.out = std::move(*sampler);
    }

    const ShaderBlob* fullscreen = shaders::render_frame.find("fullscreen_vs");
    const auto make = [&](gpu::GraphicsPipeline& out, const ShaderTable& set, const char* entry,
                          std::span<const gpu::ColorTargetDesc> targets, bool sky, const char* name) {
        const ShaderBlob* shader = set.find(entry);
        if (!shader) {
            log::error("render", "pipeline '{}': the shader is not in this build", name);
            return false;
        }
        gpu::GraphicsPipelineDesc desc{
            .vertexShader = fullscreen, .fragmentShader = shader, .colorTargets = targets, .name = name};
        if (sky) {
            desc.depth = {.test = true, .write = false};
            desc.depthFormat = sceneDepthFormat;
        }
        Result<gpu::GraphicsPipeline> p = device.createPipeline(desc);
        if (!p) {
            log::error("render", "pipeline '{}': {}", name, p.error());
            return false;
        }
        out = std::move(*p);
        ++pipelines;
        return true;
    };
    const auto& clouds = shaders::render_clouds;
    const gpu::ColorTargetDesc noiseTarget{kNoiseFormat}, weatherTarget{kWeatherFormat},
        shadowTarget{kWeatherMapsFormat}, cloudTarget{kCloudFormat}, hdrTarget{hdrFormat};
    const gpu::ColorTargetDesc marchTargets[] = {{kCloudFormat}, {m_depthFormat}};
    bool ok =
        make(m_noisePipeline, shaders::render_cloudnoise, "cloud_noise_fs", {&noiseTarget, 1}, false,
             "render cloud noise") &&
        make(m_weatherPipeline, clouds, "weather_map_fs", {&weatherTarget, 1}, false, "render weather map") &&
        make(m_shadowLayerPipeline, clouds, "cloud_shadow_layer_fs", {&shadowTarget, 1}, false,
             "render cloud shadows (layer)") &&
        make(m_cubeLayerPipeline, clouds, "sky_cube_layer_fs", {&cloudTarget, 1}, false, "render sky cube (layer)") &&
        make(m_skyLayerPipeline, clouds, "sky_layer_fs", {&hdrTarget, 1}, true, "render sky (cloud layer)");
    if (ok && volume)
        ok = make(m_shadowPipeline, clouds, "cloud_shadow_fs", {&shadowTarget, 1}, false, "render cloud shadows") &&
             make(m_marchPipeline, clouds, "cloud_march_fs", marchTargets, false, "render clouds") &&
             make(m_temporalPipeline, clouds, "cloud_temporal_fs", {&cloudTarget, 1}, false,
                  "render clouds accumulation") &&
             make(m_cubePipeline, clouds, "sky_cube_fs", {&cloudTarget, 1}, false, "render sky cube") &&
             make(m_skyPipeline, clouds, "sky_clouds_fs", {&hdrTarget, 1}, true, "render sky (clouds)");
    // The 2D noise of the layer and the cirrus, drawn once.
    ok = ok && createTexture(m_layerNoise, {.format = kNoiseFormat,
                                            .usage = kTargetSampled,
                                            .width = kLayerSize,
                                            .height = kLayerSize,
                                            .name = "render cloud layer noise"});
    if (ok) {
        gpu::CommandBuffer commands = device.acquireCommandBuffer();
        if (gpu::RenderPass pass =
                commands.beginRenderPass(gpu::ColorTarget{.texture = m_layerNoise, .load = gpu::LoadOp::DontCare})) {
            shader::CloudUniforms u{};
            u.noise = glm::vec4(0.0f, 1.0f, 2.0f, 0.0f);
            pass.bindPipeline(m_noisePipeline);
            pass.pushUniforms(kFragment, 0, u);
            pass.draw(3);
            pass.end();
        }
        if (Result<void> r = commands.submit(); !r) {
            log::error("render", "cloud layer noise: {}", r.error());
            ok = false;
        }
    }
    if (!ok) {
        *this = CloudRenderer{};
        log::error("render", "cloud pipelines or textures failed (logged); no clouds");
        return false;
    }
    if (!volume)
        log::info("render", "clouds: the device cannot render to an {} 3D texture; the 2D cloud layer only",
                  gpu::formatInfo(kNoiseFormat).name);
    return true;
}

bool CloudRenderer::ensureNoise(gpu::CommandBuffer& commands) {
    if (m_shapeNoise) return true;
    if (m_noiseFailed || !m_marchPipeline) return false;
    const auto make = [&](gpu::Texture& out, TextureType type, TextureUsage usage, std::uint32_t size,
                          const char* name) {
        return createTexture(out, {.type = type,
                                   .format = kNoiseFormat,
                                   .usage = usage,
                                   .width = size,
                                   .height = size,
                                   .layers = size,
                                   .name = name});
    };
    gpu::Texture shapeLayers, detailLayers; // dropped once the copies are recorded (gpu/Resources.h)
    if (!make(m_shapeNoise, TextureType::Texture3D, TextureUsage::Sampled, kShapeSize, "render cloud shape noise") ||
        !make(m_detailNoise, TextureType::Texture3D, TextureUsage::Sampled, kDetailSize, "render cloud detail noise") ||
        !make(shapeLayers, TextureType::Texture2DArray, TextureUsage::ColorTarget, kShapeSize,
              "render cloud shape noise layers") ||
        !make(detailLayers, TextureType::Texture2DArray, TextureUsage::ColorTarget, kDetailSize,
              "render cloud detail noise layers")) {
        m_shapeNoise = {};
        m_detailNoise = {};
        m_noiseFailed = true;
        log::error("render", "volumetric clouds: no 3D noise (logged); the 2D cloud layer");
        return false;
    }
    commands.beginTiming("cloud_noise");
    const auto draw = [&](const gpu::Texture& layers, std::uint32_t size, float kind) {
        for (std::uint32_t z = 0; z < size; ++z) {
            gpu::RenderPass pass = commands.beginRenderPass(
                gpu::ColorTarget{.texture = layers, .layer = z, .load = gpu::LoadOp::DontCare});
            if (!pass) continue;
            shader::CloudUniforms u{};
            u.noise = glm::vec4(float(z), float(size), kind, 0.0f);
            pass.bindPipeline(m_noisePipeline);
            pass.pushUniforms(kFragment, 0, u);
            pass.draw(3);
            pass.end();
        }
    };
    draw(shapeLayers, kShapeSize, 0.0f);
    draw(detailLayers, kDetailSize, 1.0f);
    if (gpu::CopyPass copy = commands.beginCopyPass()) {
        for (std::uint32_t z = 0; z < kShapeSize; ++z)
            copy.copy({.texture = shapeLayers, .layer = z}, {.texture = m_shapeNoise, .z = z});
        for (std::uint32_t z = 0; z < kDetailSize; ++z)
            copy.copy({.texture = detailLayers, .layer = z}, {.texture = m_detailNoise, .z = z});
    }
    commands.endTiming();
    log::info("render", "clouds: 3D noise {}^3 and {}^3 drawn", kShapeSize, kDetailSize);
    return true;
}

bool CloudRenderer::ensureTargets(std::uint32_t width, std::uint32_t height) {
    if (m_march && m_width == width && m_height == height) return true;
    m_march = {};
    m_marchDepth = {};
    m_history = {};
    m_historyValid = false;
    m_width = m_height = 0;
    const auto target = [&](gpu::Texture& out, TextureFormat format, const char* name) {
        return createTexture(
            out, {.format = format, .usage = kTargetSampled, .width = width, .height = height, .name = name});
    };
    if (!target(m_march, kCloudFormat, "render clouds") ||
        !target(m_marchDepth, m_depthFormat, "render clouds depth") ||
        !target(m_history[0], kCloudFormat, "render clouds history 0") ||
        !target(m_history[1], kCloudFormat, "render clouds history 1")) {
        m_march = {};
        m_marchDepth = {};
        m_history = {};
        return false;
    }
    m_width = width;
    m_height = height;
    return true;
}

bool CloudRenderer::ensureCube(std::uint32_t size) {
    if (m_cube && m_cubeSize == size) return true;
    m_cube = {};
    m_cubeSize = 0;
    m_cubeFacesDrawn = 0;
    m_cubeFace = 0;
    if (!createTexture(m_cube, {.type = TextureType::Cube,
                                .format = kCloudFormat,
                                .usage = kTargetSampled,
                                .width = size,
                                .height = size,
                                .layers = 6,
                                .mipLevels = gpu::fullMipCount(size, size),
                                .name = "render sky cube"}))
        return false;
    m_cubeSize = size;
    return true;
}

void CloudRenderer::release() {
    m_shapeNoise = {};
    m_detailNoise = {};
    m_weatherMap = {};
    m_march = {};
    m_marchDepth = {};
    m_history = {};
    m_historyValid = false;
    m_width = m_height = 0;
    m_cube = {};
    m_cubeSize = 0;
    m_cubeFacesDrawn = 0;
    m_skyMode = m_lastMode = Clouds::Off;
    m_shadows = m_weatherValid = m_shadowValid = false;
}

void CloudRenderer::render(gpu::CommandBuffer& commands, const RenderSettings& settings, const Frame& frame,
                           shader::FrameUniforms& fu, RenderStats& stats) {
    m_skyMode = Clouds::Off;
    m_shadows = false;
    if (!frame.weather || frame.mode == Clouds::Off || !ready()) {
        if (m_lastMode != Clouds::Off) release();
        return;
    }
    const weather::Weather& weather = *frame.weather;
    const shader::WeatherUniforms& wu = weather.uniforms();
    Clouds mode = frame.mode;
    if (mode == Clouds::Volumetric && !ensureNoise(commands)) mode = Clouds::Layer;
    if (!m_weatherMap && !createTexture(m_weatherMap, {.format = kWeatherFormat,
                                                       .usage = kTargetSampled,
                                                       .width = kWeatherMapSize,
                                                       .height = kWeatherMapSize,
                                                       .mipLevels = gpu::fullMipCount(kWeatherMapSize, kWeatherMapSize),
                                                       .name = "render weather map"}))
        return;

    const glm::vec3 cam(fu.cameraPos);
    const glm::vec2 camXz(cam.x, cam.z);
    const glm::vec3 sunColor = fu.sunDir.w > 0.0f ? glm::vec3(fu.sunColor) : glm::vec3(0.0f);
    const float sinSun = std::max(fu.sunDir.y, 0.0f);
    const glm::vec3 skyAmbient(fu.skyAmbient);
    const float cover = weather.cover(camXz, kCoverRadius);
    // A closed deck passes about a third of the light above it, all of it diffuse, grey with a little of the sun's
    // hue: the radiance of its base, and the sky's ambient light moving toward it as the cover closes (also the light
    // from above on the clouds themselves: under a deck they do not see the blue sky).
    const float sunPeak = std::max({sunColor.r, sunColor.g, sunColor.b});
    const glm::vec3 hue = sunPeak > 0.0f ? glm::mix(glm::vec3(1.0f), sunColor / sunPeak, 0.3f) : glm::vec3(1.0f);
    const glm::vec3 overcastIrradiance = glm::vec3(0.93f, 0.96f, 1.0f) * hue * kOvercastTransmittance *
                                         (luminance(sunColor) * sinSun + luminance(skyAmbient));
    const float closing = std::pow(std::clamp(cover, 0.0f, 1.0f), 1.2f);
    const glm::vec3 ambient = glm::mix(skyAmbient, overcastIrradiance, closing);
    // The light under the clouds for the rest of the frame (and the cloud passes' haze): set before the passes.
    fu.overcast = glm::vec4(overcastIrradiance / kPi, cover);
    fu.skyAmbient = glm::vec4(ambient, fu.skyAmbient.w);
    fu.groundAmbient = glm::vec4(glm::vec3(fu.groundAmbient) * glm::mix(1.0f, 0.45f, cover), fu.groundAmbient.w);
    const glm::vec3 fog(fu.fogColor);
    fu.fogColor = glm::vec4(glm::mix(fog, glm::vec3(luminance(fog) * 0.85f), cover * 0.8f), fu.fogColor.w);

    shader::CloudUniforms& u = m_uniforms;
    u = {};
    u.invViewProjStable = glm::inverse(frame.viewProjStable);
    u.prevViewProjStable = frame.prevViewProjStable;
    // The weather and shadow maps change slowly: each is drawn every other frame, in turns, and at once when it has
    // moved with the camera, after a cut or a change of mode, or when it is not there yet.
    const bool fresh = frame.cut || mode != m_lastMode;
    const bool turn = (m_frame++ & 1u) == 0;
    const glm::vec2 mapOrigin = snapped(camXz, kWeatherExtent, kWeatherMapSize);
    const bool drawWeather = !m_weatherValid || fresh || turn || mapOrigin != m_weatherOrigin;
    if (drawWeather) m_weatherOrigin = mapOrigin;
    u.weatherMap = glm::vec4(m_weatherOrigin, 1.0f / kWeatherExtent, kWeatherExtent);
    const glm::vec2 shadowOrigin = snapped(camXz, kShadowExtent, kShadowMapSize);
    const bool drawShadow = !m_shadowValid || fresh || !turn || shadowOrigin != m_shadowOrigin;
    u.layer = glm::vec4(wu.layer.x, wu.layer.y, wu.layer.z, kPlanetRadius);
    // The march ends at the highest top on the map (a stratus deck is marched through its few hundred metres, not
    // through the layer's 7.5 km); a cumulonimbus's anvil reaches the layer's top.
    const float highest = weather.highestType();
    u.params.w = highest > 0.8f ? wu.layer.y : weather.cloudTop(highest);
    u.air = wu.air;
    u.shape = glm::vec4(kShapeScale, kDetailScale, wu.shape.y, kExtinction);
    u.sun = glm::vec4(sunColor, fu.sunDir.w > 0.0f ? 1.0f : 0.0f);
    // The tops see the clear sky; the bases the ground, lit by the sun where the clouds let it through.
    u.ambientTop = glm::vec4(ambient, 0.0f);
    u.ambientBottom = glm::vec4((sunColor * sinSun * (1.0f - 0.8f * cover) + ambient) * 0.15f, 0.0f);
    const int steps = std::clamp(settings.cloudSteps, 8, 256);
    const int lightSteps = steps >= 64 ? 6 : 4;
    u.march = glm::vec4(float(steps), float(lightSteps), kLongestPath,
                        float(std::fmod(double(frame.sequence) * 0.6180339887, 1.0)));

    commands.pushUniforms(kFragment, 0, fu);
    const gpu::TextureSamplerBinding weatherBinding{m_weatherMap, m_weatherSampler};

    // ---- The weather map and the clouds' shadow
    commands.beginTiming("weather");
    if (drawWeather) {
        if (gpu::RenderPass pass = commands.beginRenderPass(
                gpu::ColorTarget{.texture = m_weatherMap, .load = gpu::LoadOp::DontCare, .cycle = true})) {
            pass.bindPipeline(m_weatherPipeline);
            pass.pushUniforms(kFragment, 1, u);
            pass.pushUniforms(kFragment, 2, wu);
            pass.draw(3);
            pass.end();
        }
        commands.generateMipmaps(m_weatherMap);
        m_weatherValid = true;
    }
    const bool shadows = settings.cloudShadows && fu.sunDir.w > 0.0f && fu.sunDir.y > 0.0f && frame.maps;
    if (shadows && drawShadow) {
        m_shadowOrigin = shadowOrigin;
        u.shadowMap = glm::vec4(m_shadowOrigin, 1.0f / kShadowExtent, kShadowExtent);
        // Its region only: the rest of the weather maps stays.
        if (gpu::RenderPass pass =
                commands.beginRenderPass(gpu::ColorTarget{.texture = *frame.maps, .load = gpu::LoadOp::Load})) {
            pass.setViewport({0.0f, 0.0f, float(kShadowMapSize), float(kShadowMapSize)});
            pass.setScissor({0, 0, int(kShadowMapSize), int(kShadowMapSize)});
            const bool volume = mode == Clouds::Volumetric;
            const gpu::TextureSamplerBinding second{volume ? m_shapeNoise : m_layerNoise, m_noiseSampler};
            const gpu::TextureSamplerBinding bindings[] = {weatherBinding, second};
            pass.bindPipeline(volume ? m_shadowPipeline : m_shadowLayerPipeline);
            pass.bindSamplers(kFragment, 0, bindings);
            pass.pushUniforms(kFragment, 1, u);
            pass.draw(3);
            pass.end();
        }
        m_shadowValid = true;
    }
    if (!shadows) m_shadowValid = false;
    m_shadows = m_shadowValid;
    u.shadowMap = glm::vec4(m_shadowOrigin, 1.0f / kShadowExtent, kShadowExtent);
    commands.endTiming();

    // ---- The volumetric march at a share of the render size, accumulated over frames
    bool marched = false;
    if (mode == Clouds::Volumetric) {
        const float share = std::clamp(settings.cloudResolution, 0.25f, 1.0f);
        const auto w = std::max(16u, std::uint32_t(std::lround(float(frame.renderWidth) * share)));
        const auto h = std::max(16u, std::uint32_t(std::lround(float(frame.renderHeight) * share)));
        if (ensureTargets(w, h)) {
            commands.beginTiming("clouds");
            u.target = glm::vec4(float(w), float(h), 1.0f / float(w), 1.0f / float(h));
            const gpu::ColorTarget colors[] = {{.texture = m_march, .load = gpu::LoadOp::DontCare, .cycle = true},
                                               {.texture = m_marchDepth, .load = gpu::LoadOp::DontCare, .cycle = true}};
            if (gpu::RenderPass pass = commands.beginRenderPass(colors)) {
                const gpu::TextureSamplerBinding bindings[] = {weatherBinding,
                                                               {m_shapeNoise, m_noiseSampler},
                                                               {m_detailNoise, m_noiseSampler},
                                                               {m_layerNoise, m_noiseSampler}};
                pass.bindPipeline(m_marchPipeline);
                pass.bindSamplers(kFragment, 0, bindings);
                pass.pushUniforms(kFragment, 1, u);
                pass.draw(3);
                pass.end();
            }
            if (settings.cloudTemporal) {
                const bool valid = m_historyValid && !frame.cut && m_lastMode == Clouds::Volumetric;
                u.temporal = glm::vec4(kCurrentWeight, valid ? 1.0f : 0.0f, 0.0f, 0.0f);
                const int next = m_historyIndex ^ 1;
                if (gpu::RenderPass pass = commands.beginRenderPass(gpu::ColorTarget{
                        .texture = m_history[std::size_t(next)], .load = gpu::LoadOp::DontCare, .cycle = true})) {
                    const gpu::TextureSamplerBinding bindings[] = {
                        {m_march, m_pointSampler},
                        {m_marchDepth, m_pointSampler},
                        {m_history[std::size_t(m_historyIndex)], m_linearSampler}};
                    pass.bindPipeline(m_temporalPipeline);
                    pass.bindSamplers(kFragment, 0, bindings);
                    pass.pushUniforms(kFragment, 1, u);
                    pass.draw(3);
                    pass.end();
                }
                m_historyIndex = next;
                m_historyValid = true;
            } else {
                m_historyValid = false;
            }
            commands.endTiming();
            marched = true;
            stats.cloudWidth = w;
            stats.cloudHeight = h;
        }
        if (!marched) mode = Clouds::Layer;
    }

    // ---- The sky cube: the sky and its clouds for the surfaces' reflections, a few faces per frame
    std::uint32_t faces = 0;
    if (frame.cube && ensureCube(std::clamp(settings.skyCubeSize, 16u, 512u))) {
        const bool redraw = m_cubeFacesDrawn < 6 || mode != m_lastMode || frame.cut;
        faces = redraw ? 6u : std::uint32_t(std::clamp(settings.skyCubeFacesPerFrame, 1, 6));
        commands.beginTiming("sky_cube");
        shader::CloudUniforms cu = u;
        cu.march = glm::vec4(float(kCubeSteps), float(kCubeLightSteps), kLongestPath, 0.0f);
        cu.params = glm::vec4(0.0f, 0.0f, float(m_cubeSize), u.params.w);
        const bool volume = mode == Clouds::Volumetric;
        for (std::uint32_t i = 0; i < faces; ++i) {
            const auto face = std::uint32_t(m_cubeFace);
            gpu::RenderPass pass = commands.beginRenderPass(
                gpu::ColorTarget{.texture = m_cube, .layer = face, .load = gpu::LoadOp::DontCare});
            m_cubeFace = (m_cubeFace + 1) % 6;
            if (!pass) continue;
            cu.params.y = float(face);
            if (volume) {
                const gpu::TextureSamplerBinding bindings[] = {weatherBinding,
                                                               {m_shapeNoise, m_noiseSampler},
                                                               {m_detailNoise, m_noiseSampler},
                                                               {m_layerNoise, m_noiseSampler}};
                pass.bindPipeline(m_cubePipeline);
                pass.bindSamplers(kFragment, 0, bindings);
            } else {
                const gpu::TextureSamplerBinding bindings[] = {weatherBinding, {m_layerNoise, m_noiseSampler}};
                pass.bindPipeline(m_cubeLayerPipeline);
                pass.bindSamplers(kFragment, 0, bindings);
            }
            pass.pushUniforms(kFragment, 1, cu);
            pass.draw(3);
            pass.end();
        }
        m_cubeFacesDrawn = std::min(m_cubeFacesDrawn + int(faces), 6);
        commands.generateMipmaps(m_cube);
        commands.endTiming();
        fu.skyCube = glm::vec4(1.0f, float(gpu::fullMipCount(m_cubeSize, m_cubeSize) - 1), 0.0f, 0.0f);
    } else {
        m_cube = {};
        m_cubeSize = 0;
        m_cubeFacesDrawn = 0;
    }

    // ---- The rest of the frame: the clouds' shadow on the sun's light
    fu.cloudShadow = m_shadows ? glm::vec4(m_shadowOrigin, 1.0f / kShadowExtent, 1.0f) : glm::vec4(0.0f);

    // ---- The lightning: the flashes of the last kFlashLength light the scene (by their distance) and glow in the
    // clouds (the brightest one now). Not in the sky cube, whose faces are drawn in turns.
    fu.lightning = glm::vec4(0.0f);
    if (settings.lightning) {
        const double now = weather.time();
        weather.strikes(camXz, kLightningRadius, now - weather::Weather::kFlashLength, now, m_strikes);
        float light = 0.0f, brightest = 0.0f;
        glm::vec3 toward(0.0f, 1.0f, 0.0f);
        for (const weather::Strike& s : m_strikes) {
            const float flash = weather::Weather::flash(s, now);
            if (flash <= 0.0f) continue;
            ++stats.lightningFlashes;
            const float distance = glm::length(s.position - camXz);
            light += flash / (1.0f + distance * distance / (kFlashNear * kFlashNear));
            const float glow = flash / (1.0f + distance * distance / (kGlowNear * kGlowNear));
            if (glow > brightest) {
                brightest = glow;
                toward = glm::normalize(glm::vec3(s.position.x, wu.layer.x + kFlashAboveBase, s.position.y) - cam);
            }
        }
        if (brightest > 0.0f) {
            const glm::vec3 irradiance = kLightningColor * (light * (kFlashIrradiance + 1.5f * luminance(ambient)));
            fu.skyAmbient += glm::vec4(irradiance, 0.0f);
            fu.groundAmbient += glm::vec4(irradiance * 0.3f, 0.0f);
            fu.lightning = glm::vec4(toward, brightest);
            fu.lightningColor =
                glm::vec4(kLightningColor * (kGlowRadiance + 3.0f * luminance(overcastIrradiance) / kPi), 0.0f);
        }
        stats.lightning = light;
    }

    stats.clouds = mode;
    stats.cloudCover = cover;
    stats.skyCubeFaces = faces;
    m_skyMode = mode;
    m_lastMode = mode;
}

void CloudRenderer::drawSky(gpu::RenderPass& pass) {
    if (m_skyMode == Clouds::Volumetric) {
        const gpu::Texture& result = m_historyValid ? m_history[std::size_t(m_historyIndex)] : m_march;
        pass.bindPipeline(m_skyPipeline);
        pass.bindSampler(kFragment, 0, {result, m_linearSampler});
    } else {
        const gpu::TextureSamplerBinding bindings[] = {{m_weatherMap, m_weatherSampler},
                                                       {m_layerNoise, m_noiseSampler}};
        pass.bindPipeline(m_skyLayerPipeline);
        pass.bindSamplers(kFragment, 0, bindings);
    }
    pass.pushUniforms(kFragment, 1, m_uniforms);
    pass.draw(3);
}

glm::vec4 CloudRenderer::weatherMapPlace() const {
    const glm::vec4 u = m_uniforms.weatherMap;
    return {u.x, u.y, u.z, float(kWeatherMapSize) * u.z};
}

gpu::TextureSamplerBinding CloudRenderer::cubeBinding(const gpu::Texture& noCube, const gpu::Sampler& sampler) const {
    if (m_cube && m_cubeFacesDrawn >= 6 && m_skyMode != Clouds::Off) return {m_cube, m_cubeSampler};
    return {noCube, sampler};
}

} // namespace quench::render::detail
