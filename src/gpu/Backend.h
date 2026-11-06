#pragma once

// The interface between the public GPU classes and a backend (src/gpu/sdlgpu, src/gpu/metal). The public classes
// check their arguments (QUENCH_ASSERT) and forward here with the backend's native handles: resources, command
// buffers, passes and fences are opaque pointers owned by the backend. A pass handle is valid until its end*Pass.

#include "core/Platform.h"

#include <quench/gpu/Device.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace quench::gpu::detail {

// A claimed window and its swapchain parameters.
struct WindowEntry {
    SDL_Window* window = nullptr;
    PresentMode presentMode = PresentMode::Vsync;
    SwapchainComposition composition = SwapchainComposition::Sdr;
};

struct DeviceImpl {
    DeviceImpl() = default;
    DeviceImpl(const DeviceImpl&) = delete;
    DeviceImpl& operator=(const DeviceImpl&) = delete;
    virtual ~DeviceImpl() = default;

    Caps caps;
    bool passTiming = false; // Device::setPassTiming
    std::atomic<std::int64_t> live{0};
    std::vector<WindowEntry> windows;

    WindowEntry* find(SDL_Window* window) {
        for (WindowEntry& w : windows)
            if (w.window == window) return &w;
        return nullptr;
    }

    // ---------------------------------------------------------------- device and resources
    virtual bool supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const = 0;
    virtual bool supportsSampleCount(TextureFormat format, std::uint32_t sampleCount) const = 0;
    // Each returns a native handle released with release().
    virtual Result<void*> createTexture(const TextureDesc& desc) = 0;
    virtual Result<void*> createBuffer(const BufferDesc& desc) = 0;
    virtual Result<void*> createSampler(const SamplerDesc& desc) = 0;
    virtual Result<void*> createTransferBuffer(const TransferBufferDesc& desc) = 0;
    virtual Result<void*> createPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual Result<void*> createComputePipeline(const ComputePipelineDesc& desc) = 0;
    // Backends without ray tracing or upscaling (Caps) keep these defaults; the front end does not call the
    // commands below on them.
    virtual Result<void*> createAccelerationStructure(const AccelerationStructureDesc&) {
        return Error{"acceleration structures need Caps::rayTracing"};
    }
    virtual Result<void*> createUpscaler(const UpscalerDesc&) { return Error{"upscaling is not supported"}; }
    virtual void release(Kind kind, void* native) noexcept = 0;
    // Empty on failure (logged).
    virtual std::span<std::byte> map(void* transferBuffer, std::uint32_t size, bool cycle) = 0;
    virtual void unmap(void* transferBuffer) = 0;
    virtual void waitIdle() = 0;

    // ---------------------------------------------------------------- command buffers
    virtual void* acquireCommandBuffer() = 0;
    // Null on failure (logged).
    virtual void* beginRenderPass(void* commands, std::span<const ColorTarget> colors, const DepthTarget* depth) = 0;
    virtual void* beginCopyPass(void* commands) = 0;
    virtual void* beginComputePass(void* commands, std::span<const StorageBufferWrite> writes) = 0;
    virtual void endRenderPass(void* commands, void* pass) = 0;
    virtual void endCopyPass(void* commands, void* pass) = 0;
    virtual void endComputePass(void* commands, void* pass) = 0;
    virtual void pushUniforms(void* commands, ShaderStage stage, std::uint32_t slot, const void* data,
                              std::uint32_t size) = 0;
    virtual void generateMipmaps(void* commands, const TextureRef& texture) = 0;
    virtual void blit(void* commands, const BlitDesc& blit) = 0;
    // An empty ref when there is no texture to draw to (minimized window) or on failure (logged).
    virtual TextureRef acquireSwapchainTexture(void* commands, SDL_Window* window) = 0;
    virtual void pushDebugGroup(void* commands, const char* name) = 0;
    virtual void popDebugGroup(void* commands) = 0;
    virtual void insertDebugLabel(void* commands, const char* text) = 0;
    virtual void beginTiming(void* commands, const char* name) = 0;
    virtual void endTiming(void* commands) = 0;
    virtual Result<void> submit(void* commands) = 0;
    // The fence handle is released with release(Kind::Fence, ...).
    virtual Result<void*> submitWithFence(void* commands) = 0;
    virtual void cancel(void* commands) = 0;

    // ---------------------------------------------------------------- render passes
    virtual void bindPipeline(void* pass, void* pipeline) = 0;
    virtual void setViewport(void* pass, const Viewport& viewport) = 0;
    virtual void setScissor(void* pass, const Rect& scissor) = 0;
    virtual void setBlendConstants(void* pass, const Color& color) = 0;
    virtual void bindVertexBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferBinding> buffers) = 0;
    virtual void bindIndexBuffer(void* pass, const BufferBinding& buffer, IndexFormat format) = 0;
    virtual void bindSamplers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                              std::span<const TextureSamplerBinding> bindings) = 0;
    virtual void bindStorageBuffers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                    std::span<const BufferRef> buffers) = 0;
    virtual void bindStorageTextures(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                     std::span<const TextureRef> textures) = 0;
    virtual void draw(void* pass, std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                      std::uint32_t firstInstance) = 0;
    virtual void drawIndexed(void* pass, std::uint32_t indexCount, std::uint32_t instanceCount,
                             std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance) = 0;
    virtual void drawIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) = 0;
    virtual void drawIndexedIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset,
                                     std::uint32_t drawCount) = 0;

    // ---------------------------------------------------------------- compute passes
    virtual void bindComputePipeline(void* pass, void* pipeline) = 0;
    virtual void bindComputeStorageBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferRef> buffers) = 0;
    virtual void dispatch(void* pass, std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) = 0;

    // ---------------------------------------------------------------- ray tracing and upscaling (Caps)
    virtual void* beginAccelerationStructurePass(void*) { return nullptr; }
    virtual void endAccelerationStructurePass(void*, void*) {}
    virtual void buildBottomLevel(void*, void*, const RayGeometry&, bool) {}
    // refit: when the bottom levels are those of the last build (else it builds).
    virtual void buildTopLevel(void*, void*, std::span<const RayInstance>, bool) {}
    virtual void queryCompactedSize(void*, void*) {}
    virtual void copyAndCompact(void*, void*, void*) {}
    // AccelerationStructure::bytes and Device::compactedSize.
    virtual std::uint64_t accelerationStructureBytes(void*) const { return 0; }
    virtual std::optional<std::uint32_t> compactedSize(void*) const { return std::nullopt; }
    virtual void bindAccelerationStructure(void*, ShaderStage, std::uint32_t, void*) {}
    virtual void upscale(void*, void*, const UpscaleDesc&) {}

    // ---------------------------------------------------------------- copy passes
    virtual void uploadToBuffer(void* pass, void* transferBuffer, std::uint32_t sourceOffset,
                                const BufferRef& destination, std::uint32_t destinationOffset, std::uint32_t size,
                                bool cycle) = 0;
    virtual void uploadToTexture(void* pass, void* transferBuffer, std::uint32_t sourceOffset,
                                 const TextureRegion& destination, bool cycle) = 0;
    virtual void downloadFromTexture(void* pass, const TextureRegion& source, void* transferBuffer,
                                     std::uint32_t destinationOffset) = 0;
    virtual void downloadFromBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset, std::uint32_t size,
                                    void* transferBuffer, std::uint32_t destinationOffset) = 0;
    virtual void copyBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset,
                            const BufferRef& destination, std::uint32_t destinationOffset, std::uint32_t size,
                            bool cycle) = 0;
    virtual void copyTexture(void* pass, const TextureRegion& source, const TextureRegion& destination, bool cycle) = 0;

    // ---------------------------------------------------------------- presentation
    virtual Result<void> claimWindow(SDL_Window* window) = 0;
    virtual void releaseWindow(SDL_Window* window) = 0;
    virtual bool supportsPresentMode(SDL_Window* window, PresentMode mode) const = 0;
    virtual bool supportsComposition(SDL_Window* window, SwapchainComposition composition) const = 0;
    // Both at once (SDL sets them together); false on failure (logged). Called with supported values only.
    virtual bool setSwapchainParameters(SDL_Window* window, SwapchainComposition composition, PresentMode mode) = 0;
    virtual TextureFormat swapchainFormat(SDL_Window* window) const = 0;

    // ---------------------------------------------------------------- fences
    virtual bool isSignaled(void* fence) const = 0;
    virtual void wait(void* fence) const = 0;
    virtual std::optional<double> gpuMilliseconds(void* fence) const = 0;
    virtual std::vector<GpuTiming> gpuTimings(void* fence) const = 0;
};

// The backends. Each fills caps except bcTextures and astcTextures (Device::create asks supportsFormat).
Result<std::unique_ptr<DeviceImpl>> createSdlGpuDevice(const DeviceDesc& desc);
#if QUENCH_PLATFORM_APPLE
Result<std::unique_ptr<DeviceImpl>> createMetalDevice(const DeviceDesc& desc);
#endif

inline ShaderCode shaderCodeFor(const ShaderBlob& blob, ShaderFormat format) {
    switch (format) {
    case ShaderFormat::Spirv: return blob.spirv;
    case ShaderFormat::Msl: return blob.msl;
    case ShaderFormat::Metallib: return blob.metallib;
    case ShaderFormat::Dxil: return blob.dxil;
    }
    return {nullptr, 0};
}

inline std::uint32_t mipExtent(std::uint32_t size, std::uint32_t level) { return size >> level ? size >> level : 1; }

// Width and height 0 extend the region to the edge of the level.
inline std::uint32_t regionWidth(const TextureRegion& r) {
    return r.width ? r.width : mipExtent(r.texture.desc.width, r.mipLevel) - r.x;
}
inline std::uint32_t regionHeight(const TextureRegion& r) {
    return r.height ? r.height : mipExtent(r.texture.desc.height, r.mipLevel) - r.y;
}

} // namespace quench::gpu::detail
