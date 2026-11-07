#include "ScreenSpace.h"

#include <quench/core/Log.h>

#include <render_frame_shaders.h>
#include <render_screenspace_shaders.h>

#include <span>

namespace quench::render::detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureUsage;

constexpr ShaderStage kFragment = ShaderStage::Fragment;
constexpr TextureFormat kTermFormat = TextureFormat::Rg16Float;  // the term and its depth
constexpr TextureFormat kMaskFormat = TextureFormat::Rgba8Unorm; // RayTracing's mask, which it also writes into

} // namespace

bool ScreenSpace::init(gpu::Device& device, std::uint32_t& pipelines, const gpu::Sampler& point) {
    m_device = &device;
    m_point = &point;
    const ShaderBlob* fullscreen = shaders::render_frame.find("fullscreen_vs");
    const auto make = [&](gpu::GraphicsPipeline& out, const char* entry, const gpu::ColorTargetDesc& target,
                          const char* name) {
        const ShaderBlob* shader = shaders::render_screenspace.find(entry);
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
        make(m_blurPipeline, "ss_gtao_blur_fs", {kMaskFormat}, "render gtao blur"))
        return true;
    m_gtaoPipeline = {};
    m_blurAlphaPipeline = {};
    m_blurPipeline = {};
    log::error("render", "screen-space pipelines failed (logged); no screen-space ambient occlusion");
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

bool ScreenSpace::ensureTargets(std::uint32_t width, std::uint32_t height, TextureFormat depthFormat, bool ownDepth) {
    if (m_term && m_width == width && m_height == height && m_depthFormat == depthFormat && m_ownDepth == ownDepth)
        return true;
    releaseTargets();
    m_width = width;
    m_height = height;
    const TextureUsage target = TextureUsage::ColorTarget | TextureUsage::Sampled;
    // The term at half resolution: one texel per 2 x 2 pixels (their top-left pixel).
    bool ok = createTarget(m_term, kTermFormat, target, (width + 1) / 2, (height + 1) / 2, "render gtao term");
    if (ok && ownDepth)
        ok = createTarget(m_depth, depthFormat, TextureUsage::DepthStencilTarget | TextureUsage::Sampled, width, height,
                          "render screen-space depth") &&
             createTarget(m_mask, kMaskFormat, target, width, height, "render screen-space mask");
    if (!ok) {
        releaseTargets();
        return false;
    }
    m_depthFormat = depthFormat;
    m_ownDepth = ownDepth;
    return true;
}

void ScreenSpace::releaseTargets() {
    m_depth = {};
    m_term = {};
    m_mask = {};
    m_width = m_height = 0;
    m_depthFormat = TextureFormat::Invalid;
    m_ownDepth = false;
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

} // namespace quench::render::detail
