#pragma once

// HDR output with ImGui on top: the game renders into `frame`, ImGui into the 8-bit `ui` layer (blended on display
// values, as on an SDR target), and one full-screen pass composites both into the frame's target at paper white
// (src/app/shaders/imgui.hlsl, ui_composite_fs). SDR frames do not use it: ImGui is drawn onto the target directly.

#include <quench/app/ImGuiRenderer.h>
#include <quench/core/Result.h>
#include <quench/gpu/Device.h>

#include <cstdint>

struct ImDrawData;

namespace quench::app::detail {

class UiComposite {
public:
    // The game's color target for a frame of `width` x `height` in `format` (recreated when either changes).
    Result<gpu::TextureRef> frame(gpu::Device& device, gpu::TextureFormat format, std::uint32_t width,
                                  std::uint32_t height);
    // Draws ImGui into the UI layer and composites the frame and the UI into `target` (the size of frame()).
    void composite(gpu::CommandBuffer& commands, ImGuiRenderer& imgui, ImDrawData* drawData,
                   const gpu::TextureRef& target, float paperWhite);
    void reset();

private:
    gpu::Texture m_frame;
    gpu::Texture m_ui;
    gpu::Sampler m_sampler;
    gpu::GraphicsPipeline m_pipeline;
    gpu::TextureFormat m_pipelineFormat = gpu::TextureFormat::Invalid;
};

} // namespace quench::app::detail
