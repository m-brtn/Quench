#include "UiComposite.h"

#include "shaders/ImGuiUniforms.h"

#include <quench/core/Log.h>

#include <imgui_shaders.h>

#include <format>

namespace quench::app::detail {

namespace {

constexpr gpu::TextureFormat kUiFormat = gpu::TextureFormat::Rgba8Unorm;

Result<void> ensure(gpu::Device& device, gpu::Texture& texture, gpu::TextureFormat format, std::uint32_t width,
                    std::uint32_t height, const char* name) {
    if (texture && texture.desc().format == format && texture.desc().width == width && texture.desc().height == height)
        return {};
    texture.reset();
    Result<gpu::Texture> created =
        device.createTexture({.format = format,
                              .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::ColorTarget,
                              .width = width,
                              .height = height,
                              .name = name});
    if (!created) return Error{std::format("{} {}x{}: {}", name, width, height, created.error())};
    texture = std::move(*created);
    return {};
}

} // namespace

Result<gpu::TextureRef> UiComposite::frame(gpu::Device& device, gpu::TextureFormat format, std::uint32_t width,
                                           std::uint32_t height) {
    if (Result<void> made = ensure(device, m_frame, format, width, height, "hdr frame"); !made)
        return Error{made.error()};
    if (Result<void> made = ensure(device, m_ui, kUiFormat, width, height, "ui layer"); !made)
        return Error{made.error()};
    if (!m_sampler) {
        Result<gpu::Sampler> sampler = device.createSampler({.minFilter = gpu::Filter::Nearest,
                                                             .magFilter = gpu::Filter::Nearest,
                                                             .mipmapMode = gpu::MipmapMode::Nearest,
                                                             .addressU = gpu::AddressMode::ClampToEdge,
                                                             .addressV = gpu::AddressMode::ClampToEdge,
                                                             .addressW = gpu::AddressMode::ClampToEdge,
                                                             .name = "ui composite"});
        if (!sampler) return Error{sampler.error()};
        m_sampler = std::move(*sampler);
    }
    if (m_pipelineFormat != format) {
        m_pipeline.reset();
        m_pipelineFormat = format;
        const gpu::ColorTargetDesc color{format};
        Result<gpu::GraphicsPipeline> created =
            device.createPipeline({.vertexShader = shaders::imgui.find("fullscreen_vs"),
                                   .fragmentShader = shaders::imgui.find("ui_composite_fs"),
                                   .colorTargets = {&color, 1},
                                   .name = "ui composite"});
        if (!created) return Error{std::format("ui composite pipeline: {}", created.error())};
        m_pipeline = std::move(*created);
    }
    if (!m_pipeline) return Error{"ui composite pipeline unavailable"};
    return gpu::TextureRef(m_frame);
}

void UiComposite::composite(gpu::CommandBuffer& commands, ImGuiRenderer& imgui, ImDrawData* drawData,
                            const gpu::TextureRef& target, float paperWhite) {
    const std::uint32_t width = m_frame.desc().width, height = m_frame.desc().height;
    {
        const gpu::ColorTarget color{
            .texture = m_ui, .load = gpu::LoadOp::Clear, .clear = {0.0f, 0.0f, 0.0f, 0.0f}, .cycle = true};
        gpu::RenderPass pass = commands.beginRenderPass(color);
        if (!pass) return;
        imgui.draw(pass, drawData, kUiFormat, width, height);
        pass.end();
    }
    const gpu::ColorTarget color{.texture = target, .load = gpu::LoadOp::DontCare};
    gpu::RenderPass pass = commands.beginRenderPass(color);
    if (!pass) return;
    const shader::ImGuiUniforms uniforms{.transform = {}, .options = {1.0f, paperWhite, 0.0f, 0.0f}};
    const gpu::TextureSamplerBinding bindings[] = {{m_frame, m_sampler}, {m_ui, m_sampler}};
    pass.bindPipeline(m_pipeline);
    pass.bindSamplers(ShaderStage::Fragment, 0, bindings);
    pass.pushUniforms(ShaderStage::Fragment, 0, uniforms);
    pass.draw(3);
    pass.end();
}

void UiComposite::reset() {
    m_frame.reset();
    m_ui.reset();
    m_sampler.reset();
    m_pipeline.reset();
    m_pipelineFormat = gpu::TextureFormat::Invalid;
}

} // namespace quench::app::detail
