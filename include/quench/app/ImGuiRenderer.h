#pragma once

// Dear ImGui renderer over quench::gpu (ImGuiBackendFlags_RendererHasTextures, RendererHasVtxOffset). The SDL3
// platform backend from ImGui handles windowing and input; this replaces ImGui's SDL GPU renderer backend, so ImGui
// goes through the same GPU layer as the engine and games.
//
// The app shell (App.h) creates and drives it. A program with its own window and loop can use it directly: create an
// ImGui context and ImGui_ImplSDL3_InitForOther(window), then create(device); per frame, after ImGui::Render(),
// prepare() before the render pass and draw() inside it; shutdown() before ImGui::DestroyContext().

#include <quench/core/Result.h>
#include <quench/gpu/Device.h>

#include <cstdint>
#include <vector>

struct ImDrawData;
struct ImTextureData;

namespace quench::app {

class ImGuiRenderer {
public:
    // Sets the renderer backend of the current ImGui context.
    static Result<ImGuiRenderer> create(gpu::Device& device);
    ImGuiRenderer(ImGuiRenderer&&) noexcept;
    ImGuiRenderer& operator=(ImGuiRenderer&&) noexcept;
    ~ImGuiRenderer();

    // Texture requests and geometry upload, in a copy pass of `commands`; before any render pass that draws ImGui.
    void prepare(gpu::CommandBuffer& commands, ImDrawData* drawData);
    // Draws the prepared lists into an open render pass on a target of `format`, `width` x `height` pixels. `white`:
    // the value of white in the target, for HDR targets the paper white of gpu::DisplayOutput (the UI stays at SDR
    // brightness).
    void draw(gpu::RenderPass& pass, ImDrawData* drawData, gpu::TextureFormat format, std::uint32_t width,
              std::uint32_t height, float white = 1.0f);
    // Destroys the textures ImGui asked for; before the ImGui context is destroyed.
    void shutdown();

private:
    ImGuiRenderer() = default;
    const gpu::GraphicsPipeline* pipeline(gpu::TextureFormat format);
    void updateTexture(gpu::CopyPass& copy, ImTextureData& texture);
    void setupState(gpu::RenderPass& pass, const gpu::GraphicsPipeline& pipeline, ImDrawData* drawData,
                    gpu::TextureFormat format, std::uint32_t width, std::uint32_t height, float white);

    struct Pipeline {
        gpu::TextureFormat format;
        gpu::GraphicsPipeline pipeline;
    };
    gpu::Device* m_device = nullptr;
    std::vector<Pipeline> m_pipelines;
    gpu::Sampler m_sampler;
    gpu::Buffer m_vertices;
    gpu::Buffer m_indices;
    gpu::TransferBuffer m_staging;
    bool m_prepared = false;
};

// ImGui texture id of a sampled texture (ImGui::Image). The texture must outlive the frame that draws it.
std::uint64_t imguiTexture(const gpu::Texture& texture);

} // namespace quench::app
