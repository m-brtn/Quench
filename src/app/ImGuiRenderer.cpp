#include "ImGuiRenderer.h"

#include "shaders/ImGuiUniforms.h"

#include <quench/app/App.h>
#include <quench/core/Log.h>

#include <imgui.h>
#include <imgui_shaders.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace quench::app {
namespace {

gpu::TextureRef textureRef(ImTextureID id) {
    gpu::TextureRef ref;
    ref.native = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    return ref;
}

std::uint32_t grow(std::uint32_t needed) { return std::max<std::uint32_t>(64 * 1024, needed + needed / 2); }

constexpr std::uint32_t align16(std::uint32_t size) { return (size + 15u) & ~15u; }

} // namespace

std::uint64_t imguiTexture(const gpu::Texture& texture) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(gpu::detail::Access::native(texture)));
}

Result<ImGuiRenderer> ImGuiRenderer::create(gpu::Device& device) {
    ImGuiRenderer renderer;
    renderer.m_device = &device;
    Result<gpu::Sampler> sampler = device.createSampler({.mipmapMode = gpu::MipmapMode::Nearest,
                                                         .addressU = gpu::AddressMode::ClampToEdge,
                                                         .addressV = gpu::AddressMode::ClampToEdge,
                                                         .addressW = gpu::AddressMode::ClampToEdge,
                                                         .name = "imgui"});
    if (!sampler) return Error{sampler.error()};
    renderer.m_sampler = std::move(*sampler);

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "quench_gpu";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = 8192;
    return renderer;
}

ImGuiRenderer::ImGuiRenderer(ImGuiRenderer&&) noexcept = default;
ImGuiRenderer& ImGuiRenderer::operator=(ImGuiRenderer&&) noexcept = default;
ImGuiRenderer::~ImGuiRenderer() = default;

const gpu::GraphicsPipeline* ImGuiRenderer::pipeline(gpu::TextureFormat format) {
    for (const Pipeline& p : m_pipelines)
        if (p.format == format) return p.pipeline ? &p.pipeline : nullptr;
    const gpu::VertexBufferLayout layout{.slot = 0, .stride = sizeof(ImDrawVert)};
    const gpu::VertexAttribute attributes[] = {
        {0, 0, gpu::VertexFormat::Float2, offsetof(ImDrawVert, pos)},
        {1, 0, gpu::VertexFormat::Float2, offsetof(ImDrawVert, uv)},
        {2, 0, gpu::VertexFormat::Ubyte4Norm, offsetof(ImDrawVert, col)},
    };
    const gpu::ColorTargetDesc target{format, gpu::BlendState::alpha()};
    Result<gpu::GraphicsPipeline> created = m_device->createPipeline({
        .vertexShader = shaders::imgui.find("imgui_vs"),
        .fragmentShader = shaders::imgui.find("imgui_fs"),
        .vertexBuffers = {&layout, 1},
        .vertexAttributes = attributes,
        .colorTargets = {&target, 1},
        .name = "imgui",
    });
    if (!created) log::error("imgui", "pipeline for {}: {}", gpu::formatInfo(format).name, created.error());
    m_pipelines.push_back({format, created ? std::move(*created) : gpu::GraphicsPipeline{}});
    return m_pipelines.back().pipeline ? &m_pipelines.back().pipeline : nullptr;
}

void ImGuiRenderer::updateTexture(gpu::CopyPass& copy, ImTextureData& tex) {
    if (tex.Status == ImTextureStatus_WantDestroy && tex.UnusedFrames > 0) {
        delete static_cast<gpu::Texture*>(tex.BackendUserData);
        tex.BackendUserData = nullptr;
        tex.SetTexID(ImTextureID_Invalid);
        tex.SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    if (tex.Status != ImTextureStatus_WantCreate && tex.Status != ImTextureStatus_WantUpdates) return;
    IM_ASSERT(tex.Format == ImTextureFormat_RGBA32);

    if (tex.Status == ImTextureStatus_WantCreate) {
        Result<gpu::Texture> created = m_device->createTexture({.format = gpu::TextureFormat::Rgba8Unorm,
                                                                .width = std::uint32_t(tex.Width),
                                                                .height = std::uint32_t(tex.Height),
                                                                .name = "imgui texture"});
        if (!created) {
            log::error("imgui", "texture {}x{}: {}", tex.Width, tex.Height, created.error());
            return;
        }
        auto* texture = new gpu::Texture(std::move(*created));
        tex.BackendUserData = texture;
        tex.SetTexID(imguiTexture(*texture));
    }
    const bool whole = tex.Status == ImTextureStatus_WantCreate;
    const int x = whole ? 0 : tex.UpdateRect.x;
    const int y = whole ? 0 : tex.UpdateRect.y;
    const int w = whole ? tex.Width : tex.UpdateRect.w;
    const int h = whole ? tex.Height : tex.UpdateRect.h;
    const std::uint32_t rowBytes = std::uint32_t(w * tex.BytesPerPixel);
    Result<gpu::TransferBuffer> staging =
        m_device->createTransferBuffer({.size = rowBytes * std::uint32_t(h), .name = "imgui texture upload"});
    if (!staging) {
        log::error("imgui", "texture upload: {}", staging.error());
        return;
    }
    std::span<std::byte> mapped = staging->map();
    for (int row = 0; row < h; ++row)
        std::memcpy(mapped.data() + std::size_t(row) * rowBytes, tex.GetPixelsAt(x, y + row), rowBytes);
    staging->unmap();
    copy.upload(*staging, 0,
                {.texture = *static_cast<gpu::Texture*>(tex.BackendUserData),
                 .x = std::uint32_t(x),
                 .y = std::uint32_t(y),
                 .width = std::uint32_t(w),
                 .height = std::uint32_t(h)});
    tex.SetStatus(ImTextureStatus_OK);
}

void ImGuiRenderer::prepare(gpu::CommandBuffer& commands, ImDrawData* drawData) {
    m_prepared = false;
    bool textureWork = false;
    if (drawData->Textures)
        for (ImTextureData* tex : *drawData->Textures) textureWork |= tex->Status != ImTextureStatus_OK;

    const std::uint32_t vertexBytes = std::uint32_t(drawData->TotalVtxCount) * sizeof(ImDrawVert);
    const std::uint32_t indexBytes = std::uint32_t(drawData->TotalIdxCount) * sizeof(ImDrawIdx);
    const bool geometry = vertexBytes > 0 && indexBytes > 0;
    if (!textureWork && !geometry) return;

    if (geometry) {
        if (!m_vertices || m_vertices.desc().size < vertexBytes) {
            Result<gpu::Buffer> buffer = m_device->createBuffer(
                {.usage = gpu::BufferUsage::Vertex, .size = grow(vertexBytes), .name = "imgui vertices"});
            if (!buffer) return log::error("imgui", "vertex buffer: {}", buffer.error());
            m_vertices = std::move(*buffer);
        }
        if (!m_indices || m_indices.desc().size < indexBytes) {
            Result<gpu::Buffer> buffer = m_device->createBuffer(
                {.usage = gpu::BufferUsage::Index, .size = grow(indexBytes), .name = "imgui indices"});
            if (!buffer) return log::error("imgui", "index buffer: {}", buffer.error());
            m_indices = std::move(*buffer);
        }
        const std::uint32_t stagingBytes = align16(vertexBytes) + indexBytes;
        if (!m_staging || m_staging.desc().size < stagingBytes) {
            Result<gpu::TransferBuffer> buffer =
                m_device->createTransferBuffer({.size = grow(stagingBytes), .name = "imgui geometry upload"});
            if (!buffer) return log::error("imgui", "geometry upload buffer: {}", buffer.error());
            m_staging = std::move(*buffer);
        }
        std::span<std::byte> mapped = m_staging.map(true);
        std::byte* vertices = mapped.data();
        std::byte* indices = mapped.data() + align16(vertexBytes);
        for (const ImDrawList* list : drawData->CmdLists) {
            const std::size_t v = std::size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert);
            const std::size_t i = std::size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
            std::memcpy(vertices, list->VtxBuffer.Data, v);
            std::memcpy(indices, list->IdxBuffer.Data, i);
            vertices += v;
            indices += i;
        }
        m_staging.unmap();
    }

    gpu::CopyPass copy = commands.beginCopyPass();
    if (!copy) return;
    if (textureWork)
        for (ImTextureData* tex : *drawData->Textures)
            if (tex->Status != ImTextureStatus_OK) updateTexture(copy, *tex);
    if (geometry) {
        copy.upload(m_staging, 0, m_vertices, 0, vertexBytes, true);
        copy.upload(m_staging, align16(vertexBytes), m_indices, 0, indexBytes, true);
        m_prepared = true;
    }
    copy.end();
}

void ImGuiRenderer::setupState(gpu::RenderPass& pass, const gpu::GraphicsPipeline& pipeline, ImDrawData* drawData,
                               gpu::TextureFormat format, std::uint32_t width, std::uint32_t height) {
    pass.bindPipeline(pipeline);
    pass.setViewport({0.0f, 0.0f, float(width), float(height)});
    pass.bindVertexBuffer(0, {m_vertices});
    pass.bindIndexBuffer({m_indices}, sizeof(ImDrawIdx) == 2 ? gpu::IndexFormat::Uint16 : gpu::IndexFormat::Uint32);
    const ImVec2 size = drawData->DisplaySize;
    const ImVec2 origin = drawData->DisplayPos;
    const glm::vec2 scale(2.0f / size.x, -2.0f / size.y);
    const shader::ImGuiUniforms uniforms{
        .transform = {scale.x, scale.y, -1.0f - origin.x * scale.x, 1.0f - origin.y * scale.y},
        .options = {gpu::formatInfo(format).srgb ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f},
    };
    pass.pushUniforms(ShaderStage::Vertex, 0, uniforms);
}

void ImGuiRenderer::draw(gpu::RenderPass& pass, ImDrawData* drawData, gpu::TextureFormat format, std::uint32_t width,
                         std::uint32_t height) {
    if (!m_prepared || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f || width == 0 || height == 0)
        return;
    const gpu::GraphicsPipeline* pipe = pipeline(format);
    if (!pipe) return;
    setupState(pass, *pipe, drawData, format, width, height);

    // Clip rectangles are in display coordinates; the target may be larger than the window (capture sizes).
    const ImVec2 origin = drawData->DisplayPos;
    const ImVec2 scale(float(width) / drawData->DisplaySize.x, float(height) / drawData->DisplaySize.y);
    std::uint32_t vertexBase = 0;
    std::uint32_t indexBase = 0;
    for (const ImDrawList* list : drawData->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback == ImDrawCallback_ResetRenderState)
                    setupState(pass, *pipe, drawData, format, width, height);
                else
                    cmd.UserCallback(list, &cmd);
                continue;
            }
            const float x0 = std::max(0.0f, (cmd.ClipRect.x - origin.x) * scale.x);
            const float y0 = std::max(0.0f, (cmd.ClipRect.y - origin.y) * scale.y);
            const float x1 = std::min(float(width), (cmd.ClipRect.z - origin.x) * scale.x);
            const float y1 = std::min(float(height), (cmd.ClipRect.w - origin.y) * scale.y);
            if (x1 <= x0 || y1 <= y0) continue;
            pass.setScissor({std::int32_t(x0), std::int32_t(y0), std::int32_t(x1 - x0), std::int32_t(y1 - y0)});
            pass.bindSampler(ShaderStage::Fragment, 0, {textureRef(cmd.GetTexID()), m_sampler});
            pass.drawIndexed(cmd.ElemCount, 1, indexBase + cmd.IdxOffset, std::int32_t(vertexBase + cmd.VtxOffset));
        }
        vertexBase += std::uint32_t(list->VtxBuffer.Size);
        indexBase += std::uint32_t(list->IdxBuffer.Size);
    }
}

void ImGuiRenderer::shutdown() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount != 1 || !tex->BackendUserData) continue;
        delete static_cast<gpu::Texture*>(tex->BackendUserData);
        tex->BackendUserData = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
    m_pipelines.clear();
    m_vertices.reset();
    m_indices.reset();
    m_staging.reset();
    m_sampler.reset();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
}

} // namespace quench::app
