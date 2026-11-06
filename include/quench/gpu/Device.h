#pragma once

// The GPU device: creates resources and command buffers, presents to windows. Backend: SDL GPU (Metal on Apple
// platforms, Vulkan, Direct3D 12). Device capabilities are in caps() and the supports*() queries; effects choose
// their fallbacks from them, never from the platform.

#include <quench/core/Result.h>
#include <quench/gpu/CommandBuffer.h>
#include <quench/gpu/Resources.h>
#include <quench/gpu/Types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace quench::gpu {

enum class ShaderFormat : std::uint8_t { Spirv, Msl, Metallib, Dxil };

const char* shaderFormatName(ShaderFormat format);

struct DeviceDesc {
    // "metal", "vulkan" or "direct3d12"; empty: the backend's choice (SDL: Metal, then Direct3D 12, then Vulkan; the
    // SDL_GPU_DRIVER environment variable overrides it).
    std::string_view driver{};
    bool debug = false; // backend validation
    bool preferLowPower = false;
};

struct Caps {
    std::string driver;     // "metal", "vulkan", "direct3d12"
    std::string deviceName; // as reported by the driver; may be empty
    ShaderFormat shaderFormat = ShaderFormat::Spirv;
    bool gpuTiming = false;      // Device::gpuMilliseconds (Metal only)
    bool depthClamp = false;     // RasterizerState::depthClip = false works (not in the iOS Simulator)
    bool depthBiasClamp = false; // RasterizerState::depthBiasClamp works (not on SDL's Vulkan backend)
    bool indirectDraw = false;   // RenderPass::draw*Indirect (not in the iOS Simulator)
    bool bcTextures = false;     // BC1-BC7 can be sampled
    bool astcTextures = false;   // ASTC 4x4 can be sampled
};

class Device {
public:
    // The shader formats offered to the driver are the ones the engine's shader build embeds on this platform. Create
    // and destroy the device on the main thread: it holds a reference to SDL's video subsystem, which SDL GPU needs.
    static Result<Device> create(const DeviceDesc& desc = {});

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    // Waits for the GPU, releases claimed windows. Every resource must be released before.
    ~Device();

    const Caps& caps() const;
    bool supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const;
    bool supportsSampleCount(TextureFormat format, std::uint32_t sampleCount) const;

    Result<Texture> createTexture(const TextureDesc& desc);
    Result<Buffer> createBuffer(const BufferDesc& desc);
    Result<Sampler> createSampler(const SamplerDesc& desc);
    Result<TransferBuffer> createTransferBuffer(const TransferBufferDesc& desc);
    // The shaders are created from the blobs for the device's shader format and released after the pipeline.
    Result<GraphicsPipeline> createPipeline(const GraphicsPipelineDesc& desc);
    Result<ComputePipeline> createComputePipeline(const ComputePipelineDesc& desc);

    // Empty on failure (logged).
    CommandBuffer acquireCommandBuffer();

    // Convenience transfers, each in a command buffer of its own. Uploads are submitted without waiting; commands
    // submitted later see the data. Texture data has tightly packed rows; a 3D region takes its depth slices one
    // after another.
    Result<void> upload(const BufferRef& destination, std::uint32_t offset, std::span<const std::byte> data);
    Result<void> upload(const TextureRegion& destination, std::span<const std::byte> data);
    // Blocking: waits for the GPU (captures, tests and tools, not per frame). Rows tightly packed.
    Result<std::vector<std::byte>> download(const TextureRegion& source);
    Result<std::vector<std::byte>> download(const BufferRef& source, std::uint32_t offset, std::uint32_t size);

    // Presentation. A window is claimed once; acquire its textures with CommandBuffer::acquireSwapchainTexture.
    Result<void> claimWindow(SDL_Window* window, PresentMode mode = PresentMode::Vsync);
    void releaseWindow(SDL_Window* window);
    bool supportsPresentMode(SDL_Window* window, PresentMode mode) const;
    // Falls back to Vsync when the mode is unsupported; returns the mode in use.
    PresentMode setPresentMode(SDL_Window* window, PresentMode mode);
    // Whether the backend can composite the window's swapchain so (Sdr: always). The display is not asked: an HDR
    // composition on a display without HDR range shows SDR values.
    bool supportsComposition(SDL_Window* window, SwapchainComposition composition) const;
    // Falls back to Sdr when the composition is unsupported or cannot be set (logged); returns the one in use. The
    // swapchain format changes with it. Waits for the GPU when it changes: call it between frames.
    SwapchainComposition setComposition(SDL_Window* window, SwapchainComposition composition);
    SwapchainComposition composition(SDL_Window* window) const;
    TextureFormat swapchainFormat(SDL_Window* window) const;

    bool isSignaled(const Fence& fence) const;
    void wait(const Fence& fence) const;
    // GPU execution time of the command buffer behind a signalled fence; empty where unavailable (!Caps::gpuTiming)
    // or not yet signalled.
    std::optional<double> gpuMilliseconds(const Fence& fence) const;
    void waitIdle();

    // Live resources created by this device (leak checks).
    std::int64_t liveResources() const;

private:
    Device() = default;
    std::unique_ptr<detail::DeviceImpl> m_impl;
};

} // namespace quench::gpu
