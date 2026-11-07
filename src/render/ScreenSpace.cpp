#include "ScreenSpace.h"

#include "RayTracing.h"

#include <quench/core/Log.h>

#include <render_frame_shaders.h>
#include <render_localshadow_shaders.h>
#include <render_screenspace_shaders.h>

#include <glm/gtc/matrix_transform.hpp>

#include <span>

namespace quench::render::detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureUsage;

constexpr ShaderStage kFragment = ShaderStage::Fragment;
constexpr TextureFormat kTermFormat = TextureFormat::Rg16Float;         // the term and its depth
constexpr TextureFormat kMaskFormat = TextureFormat::Rgba8Unorm;        // RayTracing's mask, which it also writes into
constexpr TextureFormat kReflectionFormat = TextureFormat::Rgba16Float; // radiance and confidence

} // namespace

bool ScreenSpace::init(gpu::Device& device, std::uint32_t& pipelines, const gpu::Sampler& point,
                       const gpu::Sampler& linear) {
    m_device = &device;
    m_point = &point;
    m_linear = &linear;
    const ShaderBlob* fullscreen = shaders::render_frame.find("fullscreen_vs");
    const auto make = [&](gpu::GraphicsPipeline& out, const char* entry, const gpu::ColorTargetDesc& target,
                          const char* name) {
        const ShaderBlob* shader = shaders::render_screenspace.find(entry);
        if (!shader) shader = shaders::render_localshadow.find(entry);
        if (!shader) {
            log::error("render", "pipeline '{}': the shader is not in this build", name);
            return false;
        }
        Result<gpu::GraphicsPipeline> p = device.createPipeline(
            {.vertexShader = fullscreen, .fragmentShader = shader, .colorTargets = {&target, 1}, .name = name});
        if (!p) {
            log::error("render", "pipeline '{}': {}", name, p.error());
            return false;
        }
        out = std::move(*p);
        ++pipelines;
        return true;
    };
    gpu::ColorTargetDesc alphaOnly{kMaskFormat};
    alphaOnly.blend.writeMask = gpu::ColorWrite::A;
    if (make(m_gtaoPipeline, "ss_gtao_fs", {kTermFormat}, "render gtao") &&
        make(m_blurAlphaPipeline, "ss_gtao_blur_fs", alphaOnly, "render gtao blur (alpha)") &&
        make(m_blurPipeline, "ss_gtao_blur_fs", {kMaskFormat}, "render gtao blur") &&
        make(m_reflectPipeline, "ss_reflect_fs", {kReflectionFormat}, "render ssr") &&
        make(m_resolvePipeline, "ss_reflect_resolve_fs", {kReflectionFormat}, "render ssr resolve") &&
        make(m_localPipeline, "local_shadow_fs", {kMaskFormat}, "render local shadows") &&
        make(m_localBlurPipeline, "local_shadow_blur_fs", {kMaskFormat}, "render local shadows blur"))
        return true;
    m_gtaoPipeline = {};
    m_blurAlphaPipeline = {};
    m_blurPipeline = {};
    m_reflectPipeline = {};
    m_resolvePipeline = {};
    m_localPipeline = {};
    m_localBlurPipeline = {};
    log::error("render", "screen-space pipelines failed (logged); no screen-space ambient occlusion or reflections, "
                         "no shadow maps of area lights");
    return false;
}

bool ScreenSpace::createTarget(gpu::Texture& out, TextureFormat format, TextureUsage usage, std::uint32_t width,
                               std::uint32_t height, const char* name) {
    Result<gpu::Texture> t =
        m_device->createTexture({.format = format, .usage = usage, .width = width, .height = height, .name = name});
    if (!t) {
        log::error("render", "target '{}' {} x {}: {}", name, width, height, t.error());
        return false;
    }
    out = std::move(*t);
    return true;
}

bool ScreenSpace::ensureTargets(const Targets& targets) {
    if (m_targets == targets && m_targets.width > 0) return true;
    releaseTargets();
    const std::uint32_t w = targets.width, h = targets.height;
    const TextureUsage target = TextureUsage::ColorTarget | TextureUsage::Sampled;
    // The terms at half resolution: one texel per 2 x 2 pixels (their top-left pixel).
    bool ok = true;
    if (targets.ownDepth)
        ok = createTarget(m_depth, targets.depthFormat, TextureUsage::DepthStencilTarget | TextureUsage::Sampled, w, h,
                          "render screen-space depth");
    if (ok && targets.occlusion)
        ok = createTarget(m_term, kTermFormat, target, (w + 1) / 2, (h + 1) / 2, "render gtao term") &&
             (!targets.ownDepth || createTarget(m_mask, kMaskFormat, target, w, h, "render screen-space mask"));
    if (ok && targets.reflections)
        ok = createTarget(m_reflectTerm, kReflectionFormat, target, (w + 1) / 2, (h + 1) / 2, "render ssr term") &&
             createTarget(m_reflections, kReflectionFormat, target, w, h, "render ssr") &&
             createTarget(m_history, targets.imageFormat, TextureUsage::Sampled, w, h, "render ssr history") &&
             (!targets.ownDepth || (createTarget(m_gbufferNormal, RayTracing::gbufferNormalFormat(), target, w, h,
                                                 "render screen-space gbuffer normal") &&
                                    createTarget(m_gbufferAlbedo, RayTracing::gbufferAlbedoFormat(), target, w, h,
                                                 "render screen-space gbuffer albedo")));
    if (ok && targets.localShadows)
        ok = createTarget(m_localTerm, kMaskFormat, target, (w + 1) / 2, (h + 1) / 2, "render local shadows term") &&
             createTarget(m_localShadows, kMaskFormat, target, w, h, "render local shadows");
    if (!ok) {
        releaseTargets();
        return false;
    }
    m_targets = targets;
    return true;
}

void ScreenSpace::releaseTargets() {
    m_depth = {};
    m_term = {};
    m_mask = {};
    m_gbufferNormal = {};
    m_gbufferAlbedo = {};
    m_reflectTerm = {};
    m_reflections = {};
    m_history = {};
    m_localTerm = {};
    m_localShadows = {};
    m_targets = {};
    m_historyValid = false;
}

void ScreenSpace::occlusion(gpu::CommandBuffer& commands, const gpu::Texture& depth, const gpu::Texture& mask,
                            bool keepRgb, const shader::ScreenSpaceUniforms& uniforms) {
    {
        const gpu::ColorTarget color{.texture = m_term, .load = gpu::LoadOp::DontCare, .cycle = true};
        if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
            pass.bindPipeline(m_gtaoPipeline);
            pass.bindSampler(kFragment, 0, {depth, *m_point});
            pass.pushUniforms(kFragment, 1, uniforms);
            pass.draw(3);
            pass.end();
        }
    }
    const gpu::ColorTarget color{
        .texture = mask, .load = keepRgb ? gpu::LoadOp::Load : gpu::LoadOp::DontCare, .cycle = !keepRgb};
    if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
        const gpu::TextureSamplerBinding samplers[] = {{m_term, *m_point}, {depth, *m_point}};
        pass.bindPipeline(keepRgb ? m_blurAlphaPipeline : m_blurPipeline);
        pass.bindSamplers(kFragment, 0, samplers);
        pass.pushUniforms(kFragment, 1, uniforms);
        pass.draw(3);
        pass.end();
    }
}

glm::mat4 ScreenSpace::historyViewProj(const glm::vec3& cameraPos) const {
    // The history's matrix takes positions relative to its own camera: shifted by how far the camera moved.
    return m_historyViewProj * glm::translate(glm::mat4(1.0f), cameraPos - m_historyCamera);
}

void ScreenSpace::keepHistory(gpu::CommandBuffer& commands, const gpu::Texture& image, const glm::mat4& viewProj,
                              const glm::vec3& cameraPos) {
    {
        gpu::CopyPass copy = commands.beginCopyPass();
        copy.copy({.texture = image}, {.texture = m_history}, true);
    }
    m_historyViewProj = viewProj;
    m_historyCamera = cameraPos;
    m_historyValid = true;
}

void ScreenSpace::reflect(gpu::CommandBuffer& commands, const gpu::Texture& depth, const gpu::Texture& normal,
                          const shader::ScreenSpaceUniforms& uniforms) {
    {
        const gpu::ColorTarget color{.texture = m_reflectTerm, .load = gpu::LoadOp::DontCare, .cycle = true};
        if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
            const gpu::TextureSamplerBinding samplers[] = {
                {depth, *m_point}, {normal, *m_point}, {m_history, *m_linear}};
            pass.bindPipeline(m_reflectPipeline);
            pass.bindSamplers(kFragment, 0, samplers);
            pass.pushUniforms(kFragment, 1, uniforms);
            pass.draw(3);
            pass.end();
        }
    }
    const gpu::ColorTarget color{.texture = m_reflections, .load = gpu::LoadOp::DontCare, .cycle = true};
    if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
        const gpu::TextureSamplerBinding samplers[] = {
            {m_reflectTerm, *m_point}, {depth, *m_point}, {normal, *m_point}};
        pass.bindPipeline(m_resolvePipeline);
        pass.bindSamplers(kFragment, 0, samplers);
        pass.pushUniforms(kFragment, 1, uniforms);
        pass.draw(3);
        pass.end();
    }
}

void ScreenSpace::resolveLocalShadows(gpu::CommandBuffer& commands, const gpu::Texture& depth, const gpu::Texture& maps,
                                      const gpu::Sampler& mapSampler, const shader::LightUniforms& lights) {
    {
        const gpu::ColorTarget color{.texture = m_localTerm, .load = gpu::LoadOp::DontCare, .cycle = true};
        if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
            const gpu::TextureSamplerBinding samplers[] = {{depth, *m_point}, {maps, mapSampler}};
            pass.bindPipeline(m_localPipeline);
            pass.bindSamplers(kFragment, 0, samplers);
            pass.pushUniforms(kFragment, 1, lights);
            pass.draw(3);
            pass.end();
        }
    }
    const gpu::ColorTarget color{.texture = m_localShadows, .load = gpu::LoadOp::DontCare, .cycle = true};
    if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
        const gpu::TextureSamplerBinding samplers[] = {{m_localTerm, *m_point}, {depth, *m_point}};
        pass.bindPipeline(m_localBlurPipeline);
        pass.bindSamplers(kFragment, 0, samplers);
        pass.draw(3);
        pass.end();
    }
}

} // namespace quench::render::detail
