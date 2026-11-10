#pragma once

// Native WebGPU backend internals (web builds: Emscripten with its emdawnwebgpu port, <webgpu/webgpu.h>). The pinned
// SDL has no WebGPU driver in SDL GPU, so the browser's WebGPU is used directly; SDL still owns the window (the
// canvas) and input.
//
// What differs from the other backends:
// - Shaders are WGSL (translated from the SPIR-V at build time, cmake/QuenchShaders.cmake), and WebGPU wants the
//   type of every binding in a pipeline's layout. The types are read from the WGSL's own declarations when a pipeline
//   is created (parseBindings); whether a float texture can be filtered depends on the texture bound (depth and,
//   without float32-filterable, 32-bit float formats cannot), so a pipeline is built per set of unfilterable slots it
//   is drawn with (WgPipeline::Variant), and such a slot's sampler is replaced by a nearest one.
// - Binding numbers follow tools/shaderc's WGSL bindings: texture slot n at 2n and its sampler at 2n + 1, then the
//   read-only storage buffers in slot order; vertex stage groups 0 (resources) and 1 (uniforms), fragment 2 and 3;
//   compute 0 (read-only), 1 (read-write) and 2 (uniforms).
// - Uniforms pushed to a command buffer are copied into pages of a uniform buffer and bound with dynamic offsets.
// - Waiting (fences, downloads, the device request) yields to the browser through Asyncify; waitIdle() does not
//   wait (WebGPU keeps what submitted work uses alive itself).
// - The browser presents the canvas when the frame's callback returns: there is no present call.
// Everything runs on the main thread.

#include "gpu/Backend.h"

#include <webgpu/webgpu.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quench::gpu::detail {

constexpr std::uint32_t kWgTextureSlots = 16;
constexpr std::uint32_t kWgStorageSlots = 8;
constexpr std::uint32_t kWgUniformSlots = 4;
constexpr std::uint32_t kWgUniformPageSize = 256 * 1024; // offsets are handed out below this
constexpr std::uint32_t kWgUniformWindow = 64 * 1024;    // the size of a uniform binding; the largest block

struct WgTexture {
    WGPUTexture texture = nullptr;
    TextureDesc desc{};
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    std::uint64_t id = 0;
    bool filterable = true; // a float texture a filtering sampler may read
    struct View {
        std::uint64_t key = 0;
        WGPUTextureView view = nullptr;
    };
    std::vector<View> views;
};

struct WgBuffer {
    WGPUBuffer buffer = nullptr;
    std::uint64_t id = 0;
    std::uint32_t size = 0;
};

struct WgSampler {
    WGPUSampler sampler = nullptr;
    WGPUSampler nearest = nullptr; // for textures that cannot be filtered
    std::uint64_t id = 0;
    bool comparison = false;
};

// One download: a staging buffer, its rows padded to 256 bytes. The buffer is mapped as soon as its command buffer
// is submitted and its rows are copied into bytes when the browser resolves the map, so that a map() after the
// submission's fence does not wait (a wait hands the frame back to the browser, which then expires the swapchain's
// texture).
struct WgRead {
    WGPUBuffer buffer = nullptr;
    std::uint64_t bufferSize = 0;
    std::uint32_t offset = 0; // in the transfer buffer's data
    std::uint32_t rowBytes = 0;
    std::uint32_t pitch = 0;
    std::uint32_t rows = 0;
    std::vector<std::byte> bytes; // the rows, packed
    bool submitted = false;
    bool done = false; // read (ok) or failed
    bool ok = false;
    std::shared_ptr<std::uint32_t> pending; // the submission's fence count, if it has one

    WgRead() = default;
    WgRead(const WgRead&) = delete;
    WgRead& operator=(const WgRead&) = delete;
    ~WgRead() {
        if (buffer) wgpuBufferRelease(buffer);
    }
};

struct WgTransfer {
    bool download = false;
    std::vector<std::byte> data;
    std::vector<std::shared_ptr<WgRead>> reads; // downloads recorded since the last map()
};

// Signaled when the queue has done the submission's work and every download of it has been read.
struct WgFence {
    std::shared_ptr<std::uint32_t> pending;
};

enum class WgBindingKind : std::uint8_t { Texture, Sampler, StorageRead, StorageWrite, Uniform };

struct WgBinding {
    std::uint32_t group = 0;
    std::uint32_t binding = 0;
    WgBindingKind kind = WgBindingKind::Texture;
    WGPUTextureViewDimension dimension = WGPUTextureViewDimension_2D;
    WGPUTextureSampleType sampleType = WGPUTextureSampleType_Float; // Float, Depth, Sint or Uint
    bool multisampled = false;
    bool comparison = false; // sampler_comparison
};

// One shader stage of a pipeline.
struct WgStage {
    WGPUShaderModule module = nullptr;
    std::string entry;
    std::vector<WgBinding> resources;           // textures, samplers and read-only storage buffers, by binding
    std::vector<std::uint32_t> storageBindings; // read-only storage buffers: binding by slot
    std::vector<WgBinding> writes;              // compute: read-write storage buffers, by binding (the slot)
    std::vector<std::uint32_t> uniforms;        // slots, ascending
};

struct WgPipeline {
    bool compute = false;
    std::uint64_t id = 0;
    std::string name;
    std::array<WgStage, 2> stages; // vertex and fragment; compute in [0]
    // Graphics: what a variant is created from.
    std::vector<WGPUVertexBufferLayout> vertexBuffers;
    std::vector<std::vector<WGPUVertexAttribute>> vertexAttributes; // by buffer slot
    std::vector<WGPUColorTargetState> colorTargets;
    std::vector<WGPUBlendState> blends;
    WGPUPrimitiveState primitive{};
    WGPUDepthStencilState depthStencil{};
    bool hasDepth = false;
    WGPUMultisampleState multisample{};
    struct Variant {
        std::array<std::uint32_t, 2> unfilterable{}; // texture slots, per stage
        WGPURenderPipeline pipeline = nullptr;
        std::array<WGPUBindGroupLayout, 2> resourceLayouts{};
    };
    std::vector<Variant> variants;
    std::array<WGPUBindGroupLayout, 2> uniformLayouts{};
    // Compute.
    WGPUComputePipeline computePipeline = nullptr;
    WGPUBindGroupLayout readLayout = nullptr;
    WGPUBindGroupLayout writeLayout = nullptr;
};

struct WgWindow {
    SDL_Window* window = nullptr;
    WGPUSurface surface = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
    std::uint32_t width = 0; // as configured
    std::uint32_t height = 0;
    WgTexture current; // the frame's texture
};

// What a stage has bound in a pass, and the uniforms pushed to the command buffer for it.
struct WgStageState {
    std::array<WgTexture*, kWgTextureSlots> textures{};
    std::array<WgSampler*, kWgTextureSlots> samplers{};
    std::array<WgBuffer*, kWgStorageSlots> storage{};
    bool resourcesDirty = true;
};

struct WgUniformState {
    std::array<std::vector<std::byte>, kWgUniformSlots> data;
    std::array<std::uint32_t, kWgUniformSlots> offset{};
    // Index into WgCommands::pages; ~0u: not copied since it was pushed.
    std::array<std::uint32_t, kWgUniformSlots> page{~0u, ~0u, ~0u, ~0u};
    bool dirty = true;
};

struct WgCommands {
    WGPUCommandEncoder encoder = nullptr;
    // The open pass; the pass handle is the WgCommands itself.
    WGPURenderPassEncoder render = nullptr;
    WGPUComputePassEncoder computePass = nullptr;
    std::uint32_t targetWidth = 0;
    std::uint32_t targetHeight = 0;
    WgPipeline* pipeline = nullptr;
    bool pipelineDirty = true;
    // What the open pass's encoder has set.
    WGPURenderPipeline boundRender = nullptr;
    WGPUComputePipeline boundCompute = nullptr;
    std::array<WGPUBindGroup, 4> boundGroups{};
    std::array<std::array<std::uint32_t, kWgUniformSlots>, 4> boundOffsets{};
    std::array<WgStageState, 2> stages;          // vertex and fragment; compute's read-only buffers in [0]
    std::array<std::uint32_t, 2> unfilterable{}; // the stages' texture slots that cannot be filtered
    std::array<WgBuffer*, kWgStorageSlots> writes{};
    bool writesDirty = true;
    std::array<WgUniformState, 3> uniforms; // by ShaderStage
    std::vector<WGPUBuffer> pages;          // uniform pages, the last one being filled
    std::vector<std::byte> pageData;
    std::uint32_t pageUsed = 0;
    // Buffers the recorded copies read: they live until the submission (the port destroys a buffer with its last
    // reference).
    std::vector<WGPUBuffer> staging;
    std::vector<std::shared_ptr<WgRead>> reads; // the downloads recorded here, mapped at the submission
    std::vector<bool> debugGroups; // whether each open group reached the encoder (not while a pass is open)
    std::uint32_t pendingPops = 0; // groups closed while a pass was open
};

class WebGpuDevice final : public DeviceImpl {
public:
    WebGpuDevice() = default;
    ~WebGpuDevice() override;
    Result<void> start(const DeviceDesc& desc);

    bool supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const override;
    bool supportsSampleCount(TextureFormat format, std::uint32_t sampleCount) const override;
    Result<void*> createTexture(const TextureDesc& desc) override;
    Result<void*> createBuffer(const BufferDesc& desc) override;
    Result<void*> createSampler(const SamplerDesc& desc) override;
    Result<void*> createTransferBuffer(const TransferBufferDesc& desc) override;
    Result<void*> createPipeline(const GraphicsPipelineDesc& desc) override;
    Result<void*> createComputePipeline(const ComputePipelineDesc& desc) override;
    void release(Kind kind, void* native) noexcept override;
    std::span<std::byte> map(void* transferBuffer, std::uint32_t size, bool cycle) override;
    void unmap(void* transferBuffer) override;
    void waitIdle() override;

    void* acquireCommandBuffer() override;
    void* beginRenderPass(void* commands, std::span<const ColorTarget> colors, const DepthTarget* depth) override;
    void* beginCopyPass(void* commands) override;
    void* beginComputePass(void* commands, std::span<const StorageBufferWrite> writes) override;
    void endRenderPass(void* commands, void* pass) override;
    void endCopyPass(void* commands, void* pass) override;
    void endComputePass(void* commands, void* pass) override;
    void pushUniforms(void* commands, ShaderStage stage, std::uint32_t slot, const void* data,
                      std::uint32_t size) override;
    void generateMipmaps(void* commands, const TextureRef& texture) override;
    void blit(void* commands, const BlitDesc& blit) override;
    TextureRef acquireSwapchainTexture(void* commands, SDL_Window* window) override;
    void pushDebugGroup(void* commands, const char* name) override;
    void popDebugGroup(void* commands) override;
    void insertDebugLabel(void* commands, const char* text) override;
    void beginTiming(void*, const char*) override {}
    void endTiming(void*) override {}
    Result<void> submit(void* commands) override;
    Result<void*> submitWithFence(void* commands) override;
    void cancel(void* commands) override;

    void bindPipeline(void* pass, void* pipeline) override;
    void setViewport(void* pass, const Viewport& viewport) override;
    void setScissor(void* pass, const Rect& scissor) override;
    void setBlendConstants(void* pass, const Color& color) override;
    void bindVertexBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferBinding> buffers) override;
    void bindIndexBuffer(void* pass, const BufferBinding& buffer, IndexFormat format) override;
    void bindSamplers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                      std::span<const TextureSamplerBinding> bindings) override;
    void bindStorageBuffers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                            std::span<const BufferRef> buffers) override;
    void bindStorageTextures(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                             std::span<const TextureRef> textures) override;
    void draw(void* pass, std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
              std::uint32_t firstInstance) override;
    void drawIndexed(void* pass, std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                     std::int32_t vertexOffset, std::uint32_t firstInstance) override;
    void drawIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) override;
    void drawIndexedIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset,
                             std::uint32_t drawCount) override;

    void bindComputePipeline(void* pass, void* pipeline) override;
    void bindComputeStorageBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferRef> buffers) override;
    void dispatch(void* pass, std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override;

    void uploadToBuffer(void* pass, void* transferBuffer, std::uint32_t sourceOffset, const BufferRef& destination,
                        std::uint32_t destinationOffset, std::uint32_t size, bool cycle) override;
    void uploadToTexture(void* pass, void* transferBuffer, std::uint32_t sourceOffset, const TextureRegion& destination,
                         bool cycle) override;
    void downloadFromTexture(void* pass, const TextureRegion& source, void* transferBuffer,
                             std::uint32_t destinationOffset) override;
    void downloadFromBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset, std::uint32_t size,
                            void* transferBuffer, std::uint32_t destinationOffset) override;
    void copyBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset, const BufferRef& destination,
                    std::uint32_t destinationOffset, std::uint32_t size, bool cycle) override;
    void copyTexture(void* pass, const TextureRegion& source, const TextureRegion& destination, bool cycle) override;

    Result<void> claimWindow(SDL_Window* window) override;
    void releaseWindow(SDL_Window* window) override;
    bool supportsPresentMode(SDL_Window* window, PresentMode mode) const override;
    bool supportsComposition(SDL_Window* window, SwapchainComposition composition) const override;
    bool setSwapchainParameters(SDL_Window* window, SwapchainComposition composition, PresentMode mode) override;
    TextureFormat swapchainFormat(SDL_Window* window) const override;

    bool isSignaled(void* fence) const override;
    void wait(void* fence) const override;
    std::optional<double> gpuMilliseconds(void*) const override { return std::nullopt; }
    std::vector<GpuTiming> gpuTimings(void*) const override { return {}; }

private:
    struct BindGroupEntry {
        WGPUBindGroup group = nullptr;
        std::uint64_t lastUsed = 0;
    };

    bool hasFeature(WGPUFeatureName feature) const;
    WGPUTextureView view(WgTexture& texture, WGPUTextureViewDimension dimension, std::uint32_t mip,
                         std::uint32_t mipCount, std::uint32_t layer, std::uint32_t layerCount, bool sampled);
    WGPUTextureView sampledView(WgTexture& texture, WGPUTextureViewDimension dimension);
    WGPUTextureView targetView(const TextureRef& texture, std::uint32_t mip, std::uint32_t layer,
                               std::uint32_t* depthSlice);
    Result<void> createStage(const ShaderBlob& blob, WgStage& stage);
    WGPUBindGroupLayout uniformLayout(const WgStage& stage, WGPUShaderStage visibility);
    const WgPipeline::Variant* variant(WgPipeline& pipeline, const std::array<std::uint32_t, 2>& unfilterable);
    WGPUBindGroupLayout resourceLayout(const WgStage& stage, WGPUShaderStage visibility, std::uint32_t unfilterable);
    WGPUBindGroup resourceGroup(const WgStage& stage, WGPUBindGroupLayout layout, WgStageState& state,
                                std::uint32_t unfilterable);
    WGPUBindGroup cachedGroup(std::vector<std::uint64_t>& key, WGPUBindGroupLayout layout,
                              std::span<const WGPUBindGroupEntry> entries);
    WGPUBindGroup uniformGroup(WGPUBindGroupLayout layout, const WgStage& stage, WGPUBuffer page);
    // Copies the stage's pushed uniforms into a page of the command buffer where needed (all of a stage's blocks in
    // one page); the dynamic offsets in slot order. Returns the page.
    WGPUBuffer placeUniforms(WgCommands& c, const WgStage& stage, ShaderStage which,
                             std::array<std::uint32_t, kWgUniformSlots>& offsets);
    void newPage(WgCommands& c);
    void flushPage(WgCommands& c);
    std::uint32_t unfilterableSlots(const WgStage& stage, const WgStageState& state);
    void setGroup(WgCommands& c, std::uint32_t index, WGPUBindGroup group, std::span<const std::uint32_t> offsets);
    void bindUniforms(WgCommands& c, std::uint32_t index, const WgStage& stage, ShaderStage which,
                      WGPUBindGroupLayout layout, bool pipelineChanged);
    bool prepareDraw(WgCommands& c);
    bool prepareDispatch(WgCommands& c);
    void resetPassState(WgCommands& c);
    void endDebugGroups(WgCommands& c);
    WgTexture* dummyTexture(WGPUTextureViewDimension dimension);
    // A mapped buffer of `size` bytes (a multiple of 4) with `data` at its start, unmapped, kept until the
    // submission.
    WGPUBuffer stagingBuffer(WgCommands& c, const std::byte* data, std::uint64_t size);
    WGPURenderPipeline blitPipeline(WGPUTextureFormat format, bool unfilterable);
    // sourceRegion: offset and size in the source level, 0..1.
    void blitPass(WgCommands& c, WgTexture& source, std::uint32_t sourceMip, std::uint32_t sourceLayer,
                  const std::array<float, 4>& sourceRegion, const TextureRef& destination, std::uint32_t mip,
                  std::uint32_t layer, const Rect& rect, LoadOp load, const Color& clear, Filter filter);
    Result<void> finish(WgCommands* c, std::shared_ptr<std::uint32_t>* pending);
    void closeCommands(WgCommands* c);
    void trimBindGroups();
    WgWindow* findWindow(SDL_Window* window);
    const WgWindow* findWindow(SDL_Window* window) const;

    WGPUInstance m_instance = nullptr;
    WGPUAdapter m_adapter = nullptr;
    WGPUDevice m_device = nullptr;
    WGPUQueue m_queue = nullptr;
    WGPULimits m_limits{};
    std::vector<WGPUFeatureName> m_features;
    std::uint64_t m_nextId = 1;
    std::uint64_t m_submissions = 0;
    // Released textures and buffers are destroyed once no open command buffer can still use them.
    std::uint32_t m_openCommands = 0;
    std::vector<WGPUTexture> m_deadTextures;
    std::vector<WGPUBuffer> m_deadBuffers;
    std::vector<std::byte> m_scratch;
    std::vector<std::unique_ptr<WgWindow>> m_windows;
    std::vector<WGPUBuffer> m_freePages;
    std::unordered_map<std::string, WGPUBindGroupLayout> m_layouts; // by description
    std::unordered_map<std::string, BindGroupEntry> m_bindGroups;   // by layout and resource ids
    std::array<std::unique_ptr<WgTexture>, 3> m_dummyTextures;      // 2D array (also as 2D), cube, 3D
    WGPUSampler m_defaultSampler = nullptr;
    WGPUBuffer m_dummyStorage = nullptr;
    // The blit: a pipeline per destination format, for filterable and unfilterable sources.
    WGPUShaderModule m_blitModule = nullptr;
    std::array<WGPUBindGroupLayout, 2> m_blitLayouts{}; // filterable, unfilterable
    std::unordered_map<std::uint32_t, WGPURenderPipeline> m_blitPipelines;
    std::array<WGPUSampler, 2> m_blitSamplers{}; // nearest, linear
};

namespace wg {

inline WGPUStringView sv(std::string_view text) { return {text.data(), text.size()}; }

WGPUTextureFormat toWgpu(TextureFormat format);
TextureFormat fromWgpu(WGPUTextureFormat format);

inline WgTexture* texture(const TextureRef& ref) { return static_cast<WgTexture*>(ref.native); }
inline WgBuffer* buffer(const BufferRef& ref) { return static_cast<WgBuffer*>(ref.native); }
inline WgSampler* sampler(const SamplerRef& ref) { return static_cast<WgSampler*>(ref.native); }

} // namespace wg

} // namespace quench::gpu::detail
