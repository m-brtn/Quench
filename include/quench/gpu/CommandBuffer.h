#pragma once

// Command recording. A command buffer is acquired from gpu::Device, filled with passes and submitted once, on the
// thread that acquired it. At most one pass is open at a time; a pass ends when end() is called or when it is
// destroyed. Commands run on the GPU in submission order.

#include <quench/core/Result.h>
#include <quench/gpu/Resources.h>
#include <quench/gpu/ShaderBlob.h>
#include <quench/gpu/Types.h>

#include <cstdint>
#include <span>

struct SDL_Window;

namespace quench::gpu {

class CommandBuffer;

struct TextureRegion {
    TextureRef texture{};
    std::uint32_t mipLevel = 0;
    std::uint32_t layer = 0; // array layer or cube face
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;      // 3D textures
    std::uint32_t width = 0;  // 0: to the edge of the level
    std::uint32_t height = 0; // 0: to the edge of the level
    std::uint32_t depth = 1;
};

struct ColorTarget {
    TextureRef texture{};
    std::uint32_t mipLevel = 0;
    std::uint32_t layer = 0;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    Color clear{};
    TextureRef resolveTexture{}; // StoreOp::Resolve or ResolveAndStore: the single-sample texture to resolve into
    std::uint32_t resolveMipLevel = 0;
    std::uint32_t resolveLayer = 0;
    bool cycle = false;        // write to fresh memory if the GPU may still use the texture (not with LoadOp::Load)
    bool cycleResolve = false; // the same for resolveTexture
};

struct DepthTarget {
    TextureRef texture{};
    std::uint32_t mipLevel = 0;
    std::uint32_t layer = 0;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::DontCare;
    float clearDepth = 0.0f; // the far plane in reversed depth
    LoadOp stencilLoad = LoadOp::DontCare;
    StoreOp stencilStore = StoreOp::DontCare;
    std::uint8_t clearStencil = 0;
    bool cycle = false;
};

struct TextureSamplerBinding {
    TextureRef texture;
    SamplerRef sampler;
};

struct BufferBinding {
    BufferRef buffer;
    std::uint32_t offset = 0;
};

struct BlitDesc {
    TextureRegion source{}; // z and depth are not used: one layer or 3D slice (layer)
    TextureRegion destination{};
    LoadOp load = LoadOp::DontCare; // what the destination outside the blitted region keeps
    Color clear{};
    Filter filter = Filter::Linear;
    bool cycle = false;
};

class RenderPass {
public:
    RenderPass() = default;
    ~RenderPass() { end(); }
    RenderPass(RenderPass&& other) noexcept;
    RenderPass& operator=(RenderPass&& other) noexcept;
    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    explicit operator bool() const { return m_native != nullptr; }
    void end();

    // The pipeline must match the pass: color formats, depth format and sample count.
    void bindPipeline(const GraphicsPipeline& pipeline);
    void setViewport(const Viewport& viewport);
    void setScissor(const Rect& scissor);
    void setBlendConstants(const Color& color);

    void bindVertexBuffers(std::uint32_t firstSlot, std::span<const BufferBinding> buffers);
    void bindVertexBuffer(std::uint32_t slot, const BufferBinding& buffer) { bindVertexBuffers(slot, {&buffer, 1}); }
    void bindIndexBuffer(const BufferBinding& buffer, IndexFormat format);
    void bindSamplers(ShaderStage stage, std::uint32_t firstSlot, std::span<const TextureSamplerBinding> bindings);
    void bindSampler(ShaderStage stage, std::uint32_t slot, const TextureSamplerBinding& binding) {
        bindSamplers(stage, slot, {&binding, 1});
    }
    void bindStorageBuffers(ShaderStage stage, std::uint32_t firstSlot, std::span<const BufferRef> buffers);
    void bindStorageTextures(ShaderStage stage, std::uint32_t firstSlot, std::span<const TextureRef> textures);

    // Uniform data for the following draws (also of later passes in this command buffer).
    void pushUniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size);
    template <class T>
    void pushUniforms(ShaderStage stage, std::uint32_t slot, const T& data) {
        static_assert(sizeof(T) % 16 == 0, "uniform blocks are made of 16-byte members (vec4, mat4)");
        pushUniforms(stage, slot, &data, sizeof(T));
    }

    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1, std::uint32_t firstVertex = 0,
              std::uint32_t firstInstance = 0);
    void drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount = 1, std::uint32_t firstIndex = 0,
                     std::int32_t vertexOffset = 0, std::uint32_t firstInstance = 0);
    // The buffer holds drawCount commands of 4 (draw) or 5 (drawIndexed) uint32 values, as in Vulkan and Metal.
    void drawIndirect(const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount);
    void drawIndexedIndirect(const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount);

private:
    friend class CommandBuffer;
    CommandBuffer* m_commands = nullptr;
    void* m_native = nullptr;
    detail::PipelineInfo m_targets{};
    bool m_pipelineBound = false;
};

class CopyPass {
public:
    CopyPass() = default;
    ~CopyPass() { end(); }
    CopyPass(CopyPass&& other) noexcept;
    CopyPass& operator=(CopyPass&& other) noexcept;
    CopyPass(const CopyPass&) = delete;
    CopyPass& operator=(const CopyPass&) = delete;

    explicit operator bool() const { return m_native != nullptr; }
    void end();

    // cycle: write to fresh memory if the GPU may still use the destination (data replaced every frame).
    void upload(const TransferBuffer& source, std::uint32_t sourceOffset, const BufferRef& destination,
                std::uint32_t destinationOffset, std::uint32_t size, bool cycle = false);
    // The source rows are tightly packed (block rows for compressed formats).
    void upload(const TransferBuffer& source, std::uint32_t sourceOffset, const TextureRegion& destination,
                bool cycle = false);
    void download(const TextureRegion& source, const TransferBuffer& destination, std::uint32_t destinationOffset);
    void download(const BufferRef& source, std::uint32_t sourceOffset, std::uint32_t size,
                  const TransferBuffer& destination, std::uint32_t destinationOffset);
    void copy(const BufferRef& source, std::uint32_t sourceOffset, const BufferRef& destination,
              std::uint32_t destinationOffset, std::uint32_t size, bool cycle = false);
    // Copies the source region; the destination takes its texture, level, layer and position (not its size).
    void copy(const TextureRegion& source, const TextureRegion& destination, bool cycle = false);

private:
    friend class CommandBuffer;
    CommandBuffer* m_commands = nullptr;
    void* m_native = nullptr;
};

// A buffer a compute pass writes (BufferUsage::ComputeStorageWrite), bound for the whole pass.
struct StorageBufferWrite {
    BufferRef buffer{};
    bool cycle = false; // write to fresh memory if the GPU may still use the buffer (data replaced every frame)
};

// Compute work between render and copy passes. Commands after the pass see what it wrote.
class ComputePass {
public:
    ComputePass() = default;
    ~ComputePass() { end(); }
    ComputePass(ComputePass&& other) noexcept;
    ComputePass& operator=(ComputePass&& other) noexcept;
    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;

    explicit operator bool() const { return m_native != nullptr; }
    void end();

    void bindPipeline(const ComputePipeline& pipeline);
    // Read-only storage buffers (BufferUsage::ComputeStorageRead) in slot order after the shader's samplers.
    void bindStorageBuffers(std::uint32_t firstSlot, std::span<const BufferRef> buffers);
    void pushUniforms(std::uint32_t slot, const void* data, std::uint32_t size);
    template <class T>
    void pushUniforms(std::uint32_t slot, const T& data) {
        static_assert(sizeof(T) % 16 == 0, "uniform blocks are made of 16-byte members (vec4, mat4)");
        pushUniforms(slot, &data, sizeof(T));
    }
    // Thread groups of the pipeline's [numthreads] size.
    void dispatch(std::uint32_t groupsX, std::uint32_t groupsY = 1, std::uint32_t groupsZ = 1);

private:
    friend class CommandBuffer;
    CommandBuffer* m_commands = nullptr;
    void* m_native = nullptr;
    bool m_pipelineBound = false;
};

class CommandBuffer {
public:
    CommandBuffer() = default;
    // A command buffer that was not submitted is cancelled.
    ~CommandBuffer();
    CommandBuffer(CommandBuffer&& other) noexcept;
    CommandBuffer& operator=(CommandBuffer&& other) noexcept;
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    explicit operator bool() const { return m_native != nullptr; }

    RenderPass beginRenderPass(std::span<const ColorTarget> colors, const DepthTarget* depth = nullptr);
    RenderPass beginRenderPass(const ColorTarget& color, const DepthTarget* depth = nullptr) {
        return beginRenderPass({&color, 1}, depth);
    }
    CopyPass beginCopyPass();
    // `writes`: the read-write storage buffers in slot order (at most 8).
    ComputePass beginComputePass(std::span<const StorageBufferWrite> writes);

    // Uniform data for the following draws (or dispatches: ShaderStage::Compute), outside a pass.
    void pushUniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size);
    template <class T>
    void pushUniforms(ShaderStage stage, std::uint32_t slot, const T& data) {
        static_assert(sizeof(T) % 16 == 0, "uniform blocks are made of 16-byte members (vec4, mat4)");
        pushUniforms(stage, slot, &data, sizeof(T));
    }

    // Fills levels 1.. from level 0 (linear filter). The texture needs Sampled and ColorTarget usage. Outside passes.
    void generateMipmaps(const TextureRef& texture);
    // Scaled copy between textures of any formats the GPU can render to. Outside passes.
    void blit(const BlitDesc& blit);

    // Waits until the window's swapchain has a free texture and acquires it (a color target the size of the
    // window's pixels). Empty when the window is minimized or hidden: skip the drawing to it and submit anyway.
    // Called on the window's thread; afterwards the command buffer must be submitted, not cancelled.
    TextureRef acquireSwapchainTexture(SDL_Window* window);

    // Names shown in GPU captures (Xcode, RenderDoc).
    void pushDebugGroup(const char* name);
    void popDebugGroup();
    void insertDebugLabel(const char* text);

    // A range of passes whose GPU time Device::gpuTimings reports after submitWithFence (Caps::gpuPassTiming;
    // without it these do nothing). Outside passes; ranges nest, the name is copied.
    void beginTiming(const char* name);
    void endTiming();

    Result<void> submit();
    // Also returns a fence to wait for completion or read the GPU time (Device::gpuMilliseconds).
    Result<Fence> submitWithFence();
    void cancel();

private:
    friend class Device;
    friend class RenderPass;
    friend class CopyPass;
    friend class ComputePass;
    detail::DeviceImpl* m_device = nullptr;
    void* m_native = nullptr;
    bool m_passOpen = false;
    bool m_swapchainAcquired = false;
};

} // namespace quench::gpu
