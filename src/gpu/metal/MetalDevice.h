#pragma once

// The native Metal backend (Objective-C++ with ARC; included by src/gpu/metal/*.mm only). It mirrors what SDL GPU's
// Metal backend does for the engine's API, so both render the same frames from the same quench_shaderc MSL and
// metallib code; the binding layout is SDL's (docs/architecture.md, "The native Metal backend"). The backend is used
// from one thread, the one that records and submits (as the engine's frame thread does).
//
// Native handles are pointers to the structs below. A render, copy or compute pass handle is the MetalCommandBuffer
// that records it (one pass at a time).

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#include "gpu/Backend.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#if !QUENCH_PLATFORM_IOS_SIMULATOR // the Simulator's SDK has no MetalFX
#import <MetalFX/MetalFX.h>
#endif

#include <SDL3/SDL_metal.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quench::gpu::detail {

namespace metal {

// SDL GPU's Metal layout: vertex buffer slot n is buffer index 14 + n; 0..13 hold uniforms and storage buffers.
constexpr std::uint32_t kFirstVertexBufferSlot = 14;
constexpr std::uint32_t kMaxVertexBuffers = 16;
constexpr std::uint32_t kMaxSamplers = 16;
constexpr std::uint32_t kMaxStorage = 8;
constexpr std::uint32_t kUniformSlots = 4;
constexpr std::uint32_t kMaxAccelerationStructures = 2;
// Acceleration structure build scratch: offsets of the builds in one encoder are this far apart.
constexpr NSUInteger kScratchAlignment = 256;
// Uniform data is copied into 32 KB chunks at 256-byte offsets, as in SDL GPU.
constexpr std::uint32_t kUniformChunkSize = 32768;
constexpr std::uint32_t kUniformAlignment = 256;
// Swapchain textures in flight per window, as in SDL GPU.
constexpr std::uint32_t kFramesInFlight = 2;
// Timestamp samples per command buffer: 4 per render pass (vertex and fragment start and end), 2 per compute or
// blit pass.
constexpr std::uint32_t kTimestampSamples = 512;

MTLPixelFormat pixelFormat(TextureFormat format);
// The descriptor of a bottom level: sizes only without geometry (for the sizes query).
MTLPrimitiveAccelerationStructureDescriptor* bottomLevelDescriptor(const RayGeometry* geometry,
                                                                   std::uint32_t triangleCount, bool allowRefit);

} // namespace metal

struct MetalTexture {
    id<MTLTexture> handle;
};

struct MetalBuffer {
    id<MTLBuffer> handle;
};

struct MetalSampler {
    id<MTLSamplerState> handle;
};

// Shared memory, cycled: map(cycle = true) moves to a backing no unfinished command buffer uses (made when none is
// free), as SDL GPU does.
struct MetalTransferBuffer {
    struct Backing {
        id<MTLBuffer> handle;
        std::uint64_t lastUse = 0; // serial of the last command buffer that copied from or to it
    };
    std::vector<Backing> backings;
    std::size_t active = 0;
    std::uint32_t size = 0;
    MTLResourceOptions options = 0;

    Backing& current() { return backings[active]; }
};

// Resource counts of a shader stage (ShaderBlob::resources).
struct MetalStageCounts {
    std::uint32_t samplers = 0;
    std::uint32_t storageTextures = 0;
    std::uint32_t storageBuffers = 0;
    std::uint32_t uniformBuffers = 0;
    std::uint32_t accelerationStructures = 0; // at buffer indices after the storage buffers
};

struct MetalAccelerationStructure {
    id<MTLAccelerationStructure> handle;
    AccelerationStructureLevel level = AccelerationStructureLevel::Bottom;
    NSUInteger buildScratch = 0;
    NSUInteger refitScratch = 0;
    bool allowRefit = false;
    // Top level: the bottom levels of the last build, kept alive here and made resident where it is bound (shaders
    // reach them through the top level only), and the index into them of each instance (what a refit must keep).
    NSArray<id<MTLAccelerationStructure>>* bottomLevels = nil;
    std::vector<__unsafe_unretained id<MTLResource>> residency;
    std::vector<std::uint32_t> instanceBottomLevels;
};

struct MetalUpscaler {
#if !QUENCH_PLATFORM_IOS_SIMULATOR
    id<MTLFXSpatialScaler> spatial;
    id<MTLFXTemporalScaler> temporal;
#endif
};

struct MetalGraphicsPipeline {
    id<MTLRenderPipelineState> state;
    id<MTLDepthStencilState> depthStencil; // nil without a depth target
    MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
    MTLTriangleFillMode fill = MTLTriangleFillModeFill;
    MTLCullMode cull = MTLCullModeNone;
    MTLWinding winding = MTLWindingCounterClockwise;
    MTLDepthClipMode depthClip = MTLDepthClipModeClip;
    float depthBias = 0.0f;
    float depthBiasSlope = 0.0f;
    float depthBiasClamp = 0.0f;
    MetalStageCounts vertex;
    MetalStageCounts fragment;
};

struct MetalComputePipeline {
    id<MTLComputePipelineState> state;
    std::uint32_t storageBuffers = 0;
    std::uint32_t readWriteStorageBuffers = 0;
    std::uint32_t uniformBuffers = 0;
    std::uint32_t accelerationStructures = 0;
    MTLSize threads = MTLSizeMake(1, 1, 1);
};

class MetalDevice;

// The timed ranges of one command buffer and the counter sample buffer their passes wrote to. Shared by the
// command buffer and its fence; the sample buffer goes back to the device's pool with the last owner.
struct MetalTiming {
    struct Encoder {
        std::uint32_t firstSample = 0;
        std::uint32_t samples = 0; // 4: render pass (vertex start, end, fragment start, end); 2: compute, blit
    };
    struct Range {
        std::string name;
        std::uint32_t depth = 0;
        std::size_t firstEncoder = 0;
        std::size_t endEncoder = 0;
        bool closed = false;
        bool complete = true; // every pass in it was sampled
    };

    MetalDevice* device = nullptr;
    id<MTLCounterSampleBuffer> samples;
    std::uint32_t used = 0;
    std::vector<Encoder> encoders;
    std::vector<Range> ranges;
    std::vector<std::size_t> open; // indices into ranges
    std::optional<std::vector<GpuTiming>> results;

    MetalTiming() = default;
    MetalTiming(const MetalTiming&) = delete;
    MetalTiming& operator=(const MetalTiming&) = delete;
    ~MetalTiming();
};

struct MetalFence {
    id<MTLCommandBuffer> handle;
    std::shared_ptr<MetalTiming> timing;
};

struct MetalWindow {
    SDL_Window* window = nullptr;
    SDL_MetalView view = nullptr;
    CAMetalLayer* layer = nil;
    TextureFormat format = TextureFormat::Bgra8Unorm;
    MetalTexture texture; // the drawable acquired for the frame being recorded
    id<CAMetalDrawable> drawable;
    std::array<id<MTLCommandBuffer>, metal::kFramesInFlight> inFlight;
    std::uint32_t frame = 0;
};

struct MetalUniformBinding {
    id<MTLBuffer> buffer; // nil: never pushed (a zero block is bound)
    std::uint32_t offset = 0;
    bool dirty = true;
};

enum class MetalPass : std::uint8_t { None, Render, Compute, Copy, AccelerationStructure };

struct MetalCommandBuffer {
    id<MTLCommandBuffer> handle;
    std::uint64_t serial = 0;
    bool submitted = false;
    bool cancelled = false;

    // Uniforms: chunks from the device's pool, returned when the GPU is done; the slots persist across passes.
    std::vector<id<MTLBuffer>> uniformChunks;
    std::uint32_t uniformOffset = metal::kUniformChunkSize;
    std::array<std::array<MetalUniformBinding, metal::kUniformSlots>, 3> uniforms{}; // vertex, fragment, compute

    std::vector<MetalWindow*> presents;
    std::shared_ptr<MetalTiming> timing;
    // Top-level instance descriptors: shared buffers from the device's pool, returned when the GPU is done.
    std::vector<id<MTLBuffer>> instanceBuffers;

    // The open pass.
    MetalPass pass = MetalPass::None;
    id<MTLRenderCommandEncoder> render;
    id<MTLComputeCommandEncoder> compute;
    id<MTLBlitCommandEncoder> blit;
    id<MTLAccelerationStructureCommandEncoder> accel;
    bool accelBottomBuilt = false; // a top-level build after it starts a new encoder (builds in one may overlap)
    NSUInteger scratchOffset = 0;  // in the device's scratch buffer, for the builds of the open encoder

    // Render pass bindings, applied at the next draw.
    const MetalGraphicsPipeline* graphicsPipeline = nullptr;
    std::array<id<MTLBuffer>, metal::kMaxVertexBuffers> vertexBuffers;
    std::array<NSUInteger, metal::kMaxVertexBuffers> vertexOffsets{};
    std::uint32_t vertexBufferCount = 0;
    bool vertexBuffersDirty = false;
    id<MTLBuffer> indexBuffer;
    NSUInteger indexOffset = 0;
    MTLIndexType indexType = MTLIndexTypeUInt16;
    std::array<std::array<id<MTLTexture>, metal::kMaxSamplers>, 2> textures;
    std::array<std::array<id<MTLSamplerState>, metal::kMaxSamplers>, 2> samplers;
    std::array<std::array<id<MTLBuffer>, metal::kMaxStorage>, 2> storageBuffers;
    std::array<std::array<id<MTLTexture>, metal::kMaxStorage>, 2> storageTextures;
    std::array<bool, 2> samplersDirty{};
    std::array<bool, 2> storageBuffersDirty{};
    std::array<bool, 2> storageTexturesDirty{};
    // Vertex, fragment, compute.
    std::array<std::array<const MetalAccelerationStructure*, metal::kMaxAccelerationStructures>, 3>
        accelerationStructures{};
    std::array<bool, 3> accelerationStructuresDirty{};

    // Compute pass bindings.
    const MetalComputePipeline* computePipeline = nullptr;
    std::array<id<MTLBuffer>, metal::kMaxStorage> computeBuffers;
    std::array<id<MTLBuffer>, metal::kMaxStorage> computeWrites;
    bool computeBuffersDirty = false;

    void resetPassState();
};

class MetalDevice final : public DeviceImpl {
public:
    static Result<std::unique_ptr<DeviceImpl>> create(const DeviceDesc& desc);
    MetalDevice() = default;
    ~MetalDevice() override;

    bool supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const override;
    bool supportsSampleCount(TextureFormat format, std::uint32_t sampleCount) const override;
    Result<void*> createTexture(const TextureDesc& desc) override;
    Result<void*> createBuffer(const BufferDesc& desc) override;
    Result<void*> createSampler(const SamplerDesc& desc) override;
    Result<void*> createTransferBuffer(const TransferBufferDesc& desc) override;
    Result<void*> createPipeline(const GraphicsPipelineDesc& desc) override;
    Result<void*> createComputePipeline(const ComputePipelineDesc& desc) override;
    Result<void*> createAccelerationStructure(const AccelerationStructureDesc& desc) override;
    Result<void*> createUpscaler(const UpscalerDesc& desc) override;
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
    void beginTiming(void* commands, const char* name) override;
    void endTiming(void* commands) override;
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

    void* beginAccelerationStructurePass(void* commands) override;
    void endAccelerationStructurePass(void* commands, void* pass) override;
    void buildBottomLevel(void* pass, void* structure, const RayGeometry& geometry, bool refit) override;
    void buildTopLevel(void* pass, void* structure, std::span<const RayInstance> instances, bool refit) override;
    void bindAccelerationStructure(void* pass, ShaderStage stage, std::uint32_t slot, void* structure) override;
    void upscale(void* commands, void* upscaler, const UpscaleDesc& desc) override;

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
    std::optional<double> gpuMilliseconds(void* fence) const override;
    std::vector<GpuTiming> gpuTimings(void* fence) const override;

    // MetalTiming's destructor.
    void returnSampleBuffer(id<MTLCounterSampleBuffer> samples);

private:
    friend struct MetalTiming;

    MetalWindow* findWindow(SDL_Window* window) const;
    Result<id<MTLFunction>> function(const ShaderBlob& blob) const;
    // Command buffers whose GPU work finished go back to the free list with their uniform chunks.
    void reclaim();
    // The serial below which every command buffer has finished on the GPU (or was cancelled).
    std::uint64_t finishedBelow() const;
    void useTransfer(MetalCommandBuffer& commands, MetalTransferBuffer& buffer);
    id<MTLBuffer> uniformChunk();
    void flushGraphics(MetalCommandBuffer& commands);
    void flushCompute(MetalCommandBuffer& commands);
    // Sample buffer attachment for a pass being created while a timed range is open; -1 when not sampled.
    std::int64_t timingSamples(MetalCommandBuffer& commands, std::uint32_t count);
    Result<void*> commit(MetalCommandBuffer& commands, bool fence);
    void resolve(MetalTiming& timing) const;
    Result<id<MTLRenderPipelineState>> blitPipeline(TextureFormat destination, bool array);
    // Acceleration structure passes: an encoder (with timestamps when a range is open), scratch memory for one build
    // of the open encoder, and shared memory for instance descriptors.
    bool beginAccelerationEncoder(MetalCommandBuffer& commands);
    NSUInteger scratch(MetalCommandBuffer& commands, NSUInteger size);
    id<MTLBuffer> instanceBuffer(MetalCommandBuffer& commands, NSUInteger size);

    id<MTLDevice> m_device;
    id<MTLCommandQueue> m_queue;
    bool m_debug = false;
    id<MTLBuffer> m_zeroUniforms;
    std::vector<id<MTLBuffer>> m_uniformPool;
    // Build scratch, private; grows when a build needs more (command buffers keep the old one alive). Encoders use
    // it one after another: Metal tracks the hazard between them.
    id<MTLBuffer> m_scratch;
    std::vector<id<MTLBuffer>> m_instancePool;
    // Two texels copied by the timestamp encoders around MetalFX's passes (upscale), private.
    id<MTLBuffer> m_markerBuffer;

    // Every acquired command buffer until the GPU finished it (or it was cancelled), then the free list.
    std::vector<std::unique_ptr<MetalCommandBuffer>> m_live;
    std::vector<std::unique_ptr<MetalCommandBuffer>> m_free;
    std::uint64_t m_nextSerial = 1;

    std::vector<std::unique_ptr<MetalWindow>> m_windows;

    // Blit: pipelines by destination format and source type (2D, 2D array).
    struct BlitPipeline {
        TextureFormat format;
        bool array;
        id<MTLRenderPipelineState> state;
    };
    std::vector<BlitPipeline> m_blitPipelines;
    id<MTLFunction> m_blitVertex;
    id<MTLFunction> m_blit2d;
    id<MTLFunction> m_blit2dArray;
    id<MTLSamplerState> m_blitNearest;
    id<MTLSamplerState> m_blitLinear;

    // Timestamps (Caps::gpuPassTiming).
    id<MTLCounterSet> m_timestampSet;
    std::vector<id<MTLCounterSampleBuffer>> m_samplePool;
    mutable double m_timestampPeriodNs = 1.0;
    mutable bool m_timestampCalibrated = false;
    MTLTimestamp m_calibrationGpu = 0;
    std::chrono::steady_clock::time_point m_calibrationCpu;
};

} // namespace quench::gpu::detail
