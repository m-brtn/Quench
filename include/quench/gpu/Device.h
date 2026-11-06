#pragma once

// The GPU device: creates resources and command buffers, presents to windows. Backends: SDL GPU (Metal on Apple
// platforms, Vulkan, Direct3D 12; the default) and native Metal (Apple platforms). Device capabilities are in caps()
// and the supports*() queries; effects choose their fallbacks from them, never from the platform or the backend.

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

// Default: the QUENCH_GPU_BACKEND environment variable ("sdl" or "metal"), else SDL GPU. Metal is the native backend
// (Apple platforms only; elsewhere the device falls back to SDL GPU and logs it).
enum class Backend : std::uint8_t { Default, SdlGpu, Metal };

const char* backendName(Backend backend); // "default", "sdl", "metal"
// Whether this build can create a device on the backend (Metal: Apple platforms only), e.g. for a backend choice.
bool backendAvailable(Backend backend);

struct DeviceDesc {
    // "metal", "vulkan" or "direct3d12"; empty: the backend's choice (SDL: Metal, then Direct3D 12, then Vulkan; the
    // SDL_GPU_DRIVER environment variable overrides it). The native Metal backend has only "metal".
    std::string_view driver{};
    bool debug = false; // backend validation
    bool preferLowPower = false;
    Backend backend = Backend::Default;
};

struct Caps {
    Backend backend = Backend::SdlGpu; // the backend in use (never Default)
    std::string driver;                // "metal", "vulkan", "direct3d12"
    std::string deviceName;            // as reported by the driver; may be empty
    ShaderFormat shaderFormat = ShaderFormat::Spirv;
    bool gpuTiming = false;      // Device::gpuMilliseconds (Metal only)
    bool gpuPassTiming = false;  // Device::gpuTimings: GPU time per timed range of passes (native Metal backend only)
    bool depthClamp = false;     // RasterizerState::depthClip = false works (not in the iOS Simulator)
    bool depthBiasClamp = false; // RasterizerState::depthBiasClamp works (not on SDL's Vulkan backend)
    bool indirectDraw = false;   // RenderPass::draw*Indirect (not in the iOS Simulator)
    bool bcTextures = false;     // BC1-BC7 can be sampled
    bool astcTextures = false;   // ASTC 4x4 can be sampled
    // Acceleration structures and ray queries in fragment and compute shaders (native Metal backend on GPUs with
    // ray tracing; not on SDL GPU, which has no acceleration structures).
    bool rayTracing = false;
    bool spatialUpscaling = false;  // UpscalerType::Spatial (MetalFX on the native Metal backend)
    bool temporalUpscaling = false; // UpscalerType::Temporal (MetalFX on the native Metal backend)
    // UpscalerType::TemporalDenoised (MetalFX's denoiser on the native Metal backend, from macOS 26 and iOS 26).
    bool denoisedUpscaling = false;
};

// GPU time of one range of passes (CommandBuffer::beginTiming .. endTiming). A GPU overlaps passes (on Apple GPUs the
// vertex stage of a pass runs while the previous pass still shades fragments, then waits), so the busy time counts the
// time any stage of the range's passes ran, without the waits, and the sum of several ranges can exceed the time
// they span together.
struct GpuTiming {
    std::string name;
    double milliseconds = 0.0;      // busy time
    double startMilliseconds = 0.0; // first and last timestamp of its passes, from the command buffer's first one
    double endMilliseconds = 0.0;
    std::uint32_t depth = 0; // ranges it was opened in (0: a top-level range)
};

// Device::shaderReloadStatus.
struct ShaderReloadStatus {
    bool enabled = false;
    bool compiling = false;
    std::uint32_t reloads = 0;   // shader sets compiled again and swapped in
    std::uint32_t failures = 0;  // compiles that failed (the old code stays)
    std::uint32_t pipelines = 0; // pipelines created again
    std::string lastError;       // the last failure: the command and its output
};

class Device {
public:
    // The shader formats offered to the driver are the ones the engine's shader build embeds on this platform. Create
    // and destroy the device on the main thread: the SDL GPU device holds a reference to SDL's video subsystem.
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
    // Caps::rayTracing; built in an AccelerationStructurePass.
    Result<AccelerationStructure> createAccelerationStructure(const AccelerationStructureDesc& desc);
    // The bytes a compacted copy of the bottom level needs (AccelerationStructurePass::queryCompactedSize), once the
    // command buffer that recorded its last query has completed on the GPU; empty before that or without a query.
    // Does not wait.
    std::optional<std::uint32_t> compactedSize(const AccelerationStructure& bottomLevel) const;
    // Caps::spatialUpscaling, Caps::temporalUpscaling or Caps::denoisedUpscaling, by type.
    Result<Upscaler> createUpscaler(const UpscalerDesc& desc);

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
    PresentMode presentMode(SDL_Window* window) const;
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
    // The timed ranges of the command buffer behind a signalled fence, in the order they began; empty where
    // unavailable (!Caps::gpuPassTiming), not yet signalled or without ranges. A range whose passes could not all be
    // timed (more passes than the backend samples per command buffer) is left out.
    std::vector<GpuTiming> gpuTimings(const Fence& fence) const;
    // Per-pass timing costs CPU time in every timed command buffer (about 0.1 ms per frame in the engine demo), so it
    // is off until something reads gpuTimings: command buffers acquired while it is on record their ranges
    // (Caps::gpuPassTiming).
    void setPassTiming(bool enabled);
    bool passTiming() const;
    void waitIdle();

    // Shader hot reload, for development on desktops (docs/app.md, "Shader hot reload"): pipelines follow edits of
    // their HLSL sources and the files those include. The shader sets of live pipelines that have a manifest
    // (ShaderBlob::manifest) are watched; a set whose files changed is compiled again with the build's commands, in
    // processes of their own, and once every entry compiled, its pipelines are created again behind the same handles
    // (pipelines created later get the new code too). After a compile error, or where a pipeline cannot be created
    // from the new code, the old code stays (logged). Edits that change a shader's resources need the C++ side too.
    // buildDirectory: where the manifests' paths start; empty: the nearest directory with a CMakeCache.txt at or above
    // the executable's. An error where there is none or processes cannot run (mobile platforms, the web).
    Result<void> enableShaderHotReload(std::string_view buildDirectory = {});
    // Between frames, on the thread that records them: checks the watched files every 0.5 s, advances the compiles
    // without waiting for them and creates the pipelines again.
    void pollShaderHotReload();
    ShaderReloadStatus shaderReloadStatus() const;

    // Live resources created by this device (leak checks).
    std::int64_t liveResources() const;

private:
    Device() = default;
    std::unique_ptr<detail::DeviceImpl> m_impl;
};

} // namespace quench::gpu
