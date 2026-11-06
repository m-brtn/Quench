#pragma once

// SDL GPU backend internals: the device behind gpu::Device and the mapping of engine types to SDL GPU types. Native
// handles are the SDL objects themselves (SDL_GPUTexture*, SDL_GPUCommandBuffer*, SDL_GPURenderPass*, ...).

#include "gpu/Backend.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace quench::gpu::detail {

// GPU execution time of the command buffer behind a signalled SDL Metal fence (SdlGpuTimingMetal.m); < 0 when
// unknown.
double metalFenceMilliseconds(SDL_GPUFence* fence);

class SdlGpuDevice final : public DeviceImpl {
public:
    SdlGpuDevice(SDL_GPUDevice* device, SDL_GPUShaderFormat shaderFormat)
        : m_device(device), m_shaderFormat(shaderFormat) {}
    ~SdlGpuDevice() override;

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
    std::optional<double> gpuMilliseconds(void* fence) const override;
    std::vector<GpuTiming> gpuTimings(void*) const override { return {}; }

private:
    Result<SDL_GPUShader*> createShader(const ShaderBlob& blob);

    SDL_GPUDevice* m_device = nullptr;
    SDL_GPUShaderFormat m_shaderFormat = SDL_GPU_SHADERFORMAT_INVALID;
};

} // namespace quench::gpu::detail

namespace quench::gpu::sdl {

SDL_GPUTextureFormat toSdl(TextureFormat format);
TextureFormat fromSdl(SDL_GPUTextureFormat format);

inline SDL_GPUTextureType toSdl(TextureType type) {
    switch (type) {
    case TextureType::Texture2D: return SDL_GPU_TEXTURETYPE_2D;
    case TextureType::Texture2DArray: return SDL_GPU_TEXTURETYPE_2D_ARRAY;
    case TextureType::Texture3D: return SDL_GPU_TEXTURETYPE_3D;
    case TextureType::Cube: return SDL_GPU_TEXTURETYPE_CUBE;
    case TextureType::CubeArray: return SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
    }
    return SDL_GPU_TEXTURETYPE_2D;
}

inline SDL_GPUTextureUsageFlags toSdl(TextureUsage usage) {
    SDL_GPUTextureUsageFlags flags = 0;
    if (any(usage & TextureUsage::Sampled)) flags |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (any(usage & TextureUsage::ColorTarget)) flags |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if (any(usage & TextureUsage::DepthStencilTarget)) flags |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (any(usage & TextureUsage::GraphicsStorageRead)) flags |= SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    if (any(usage & TextureUsage::ShaderWrite)) flags |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    return flags;
}

inline SDL_GPUBufferUsageFlags toSdl(BufferUsage usage) {
    SDL_GPUBufferUsageFlags flags = 0;
    if (any(usage & BufferUsage::Vertex)) flags |= SDL_GPU_BUFFERUSAGE_VERTEX;
    if (any(usage & BufferUsage::Index)) flags |= SDL_GPU_BUFFERUSAGE_INDEX;
    if (any(usage & BufferUsage::Indirect)) flags |= SDL_GPU_BUFFERUSAGE_INDIRECT;
    if (any(usage & BufferUsage::GraphicsStorageRead)) flags |= SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    if (any(usage & BufferUsage::ComputeStorageRead)) flags |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    if (any(usage & BufferUsage::ComputeStorageWrite)) flags |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    return flags;
}

// 1, 2, 4 or 8; anything else is a programming error (asserted by the callers).
inline SDL_GPUSampleCount sampleCountToSdl(std::uint32_t count) {
    switch (count) {
    case 2: return SDL_GPU_SAMPLECOUNT_2;
    case 4: return SDL_GPU_SAMPLECOUNT_4;
    case 8: return SDL_GPU_SAMPLECOUNT_8;
    default: return SDL_GPU_SAMPLECOUNT_1;
    }
}

inline bool validSampleCount(std::uint32_t count) { return count == 1 || count == 2 || count == 4 || count == 8; }

inline SDL_GPUFilter toSdl(Filter filter) {
    return filter == Filter::Nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
}

inline SDL_GPUSamplerMipmapMode toSdl(MipmapMode mode) {
    return mode == MipmapMode::Nearest ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
}

inline SDL_GPUSamplerAddressMode toSdl(AddressMode mode) {
    switch (mode) {
    case AddressMode::Repeat: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case AddressMode::MirroredRepeat: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
}

inline SDL_GPUCompareOp toSdl(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return SDL_GPU_COMPAREOP_NEVER;
    case CompareOp::Less: return SDL_GPU_COMPAREOP_LESS;
    case CompareOp::Equal: return SDL_GPU_COMPAREOP_EQUAL;
    case CompareOp::LessOrEqual: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case CompareOp::Greater: return SDL_GPU_COMPAREOP_GREATER;
    case CompareOp::NotEqual: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case CompareOp::GreaterOrEqual: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    case CompareOp::Always: return SDL_GPU_COMPAREOP_ALWAYS;
    }
    return SDL_GPU_COMPAREOP_ALWAYS;
}

inline SDL_GPUPrimitiveType toSdl(PrimitiveType type) {
    switch (type) {
    case PrimitiveType::TriangleList: return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    case PrimitiveType::TriangleStrip: return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    case PrimitiveType::LineList: return SDL_GPU_PRIMITIVETYPE_LINELIST;
    case PrimitiveType::LineStrip: return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
    case PrimitiveType::PointList: return SDL_GPU_PRIMITIVETYPE_POINTLIST;
    }
    return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
}

inline SDL_GPUFillMode toSdl(FillMode mode) {
    return mode == FillMode::Line ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
}

inline SDL_GPUCullMode toSdl(CullMode mode) {
    switch (mode) {
    case CullMode::None: return SDL_GPU_CULLMODE_NONE;
    case CullMode::Front: return SDL_GPU_CULLMODE_FRONT;
    case CullMode::Back: return SDL_GPU_CULLMODE_BACK;
    }
    return SDL_GPU_CULLMODE_NONE;
}

inline SDL_GPUFrontFace toSdl(FrontFace face) {
    return face == FrontFace::Clockwise ? SDL_GPU_FRONTFACE_CLOCKWISE : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
}

inline SDL_GPUVertexElementFormat toSdl(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    case VertexFormat::Float2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    case VertexFormat::Float3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    case VertexFormat::Float4: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    case VertexFormat::Half2: return SDL_GPU_VERTEXELEMENTFORMAT_HALF2;
    case VertexFormat::Half4: return SDL_GPU_VERTEXELEMENTFORMAT_HALF4;
    case VertexFormat::Uint: return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
    case VertexFormat::Uint2: return SDL_GPU_VERTEXELEMENTFORMAT_UINT2;
    case VertexFormat::Uint4: return SDL_GPU_VERTEXELEMENTFORMAT_UINT4;
    case VertexFormat::Ubyte4: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4;
    case VertexFormat::Ubyte4Norm: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    case VertexFormat::Short2Norm: return SDL_GPU_VERTEXELEMENTFORMAT_SHORT2_NORM;
    case VertexFormat::Short4Norm: return SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM;
    case VertexFormat::Ushort4: return SDL_GPU_VERTEXELEMENTFORMAT_USHORT4;
    case VertexFormat::Ushort4Norm: return SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
}

inline SDL_GPUVertexInputRate toSdl(VertexInputRate rate) {
    return rate == VertexInputRate::PerInstance ? SDL_GPU_VERTEXINPUTRATE_INSTANCE : SDL_GPU_VERTEXINPUTRATE_VERTEX;
}

inline SDL_GPUBlendFactor toSdl(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero: return SDL_GPU_BLENDFACTOR_ZERO;
    case BlendFactor::One: return SDL_GPU_BLENDFACTOR_ONE;
    case BlendFactor::SrcColor: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DstColor: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case BlendFactor::OneMinusDstColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlpha: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::ConstantColor: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case BlendFactor::OneMinusConstantColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    case BlendFactor::SrcAlphaSaturate: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    }
    return SDL_GPU_BLENDFACTOR_ONE;
}

inline SDL_GPUBlendOp toSdl(BlendOp op) {
    switch (op) {
    case BlendOp::Add: return SDL_GPU_BLENDOP_ADD;
    case BlendOp::Subtract: return SDL_GPU_BLENDOP_SUBTRACT;
    case BlendOp::ReverseSubtract: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case BlendOp::Min: return SDL_GPU_BLENDOP_MIN;
    case BlendOp::Max: return SDL_GPU_BLENDOP_MAX;
    }
    return SDL_GPU_BLENDOP_ADD;
}

inline SDL_GPUColorComponentFlags toSdl(ColorWrite mask) {
    SDL_GPUColorComponentFlags flags = 0;
    if (any(mask & ColorWrite::R)) flags |= SDL_GPU_COLORCOMPONENT_R;
    if (any(mask & ColorWrite::G)) flags |= SDL_GPU_COLORCOMPONENT_G;
    if (any(mask & ColorWrite::B)) flags |= SDL_GPU_COLORCOMPONENT_B;
    if (any(mask & ColorWrite::A)) flags |= SDL_GPU_COLORCOMPONENT_A;
    return flags;
}

inline SDL_GPULoadOp toSdl(LoadOp op) {
    switch (op) {
    case LoadOp::Load: return SDL_GPU_LOADOP_LOAD;
    case LoadOp::Clear: return SDL_GPU_LOADOP_CLEAR;
    case LoadOp::DontCare: return SDL_GPU_LOADOP_DONT_CARE;
    }
    return SDL_GPU_LOADOP_DONT_CARE;
}

inline SDL_GPUStoreOp toSdl(StoreOp op) {
    switch (op) {
    case StoreOp::Store: return SDL_GPU_STOREOP_STORE;
    case StoreOp::DontCare: return SDL_GPU_STOREOP_DONT_CARE;
    case StoreOp::Resolve: return SDL_GPU_STOREOP_RESOLVE;
    case StoreOp::ResolveAndStore: return SDL_GPU_STOREOP_RESOLVE_AND_STORE;
    }
    return SDL_GPU_STOREOP_STORE;
}

inline SDL_GPUPresentMode toSdl(PresentMode mode) {
    switch (mode) {
    case PresentMode::Vsync: return SDL_GPU_PRESENTMODE_VSYNC;
    case PresentMode::Immediate: return SDL_GPU_PRESENTMODE_IMMEDIATE;
    case PresentMode::Mailbox: return SDL_GPU_PRESENTMODE_MAILBOX;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
}

inline SDL_GPUSwapchainComposition toSdl(SwapchainComposition composition) {
    switch (composition) {
    case SwapchainComposition::Sdr: return SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    case SwapchainComposition::SdrLinear: return SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR;
    case SwapchainComposition::HdrExtendedLinear: return SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR;
    case SwapchainComposition::Hdr10: return SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084;
    }
    return SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
}

inline SDL_GPUShaderStage toSdl(ShaderStage stage) {
    return stage == ShaderStage::Vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
}

inline SDL_FColor toSdl(const Color& color) { return {color.r, color.g, color.b, color.a}; }

inline SDL_GPUTexture* texture(const TextureRef& ref) { return static_cast<SDL_GPUTexture*>(ref.native); }

inline SDL_GPUTextureRegion toSdlRegion(const TextureRegion& in) {
    SDL_GPUTextureRegion region{};
    region.texture = texture(in.texture);
    region.mip_level = in.mipLevel;
    region.layer = in.layer;
    region.x = in.x;
    region.y = in.y;
    region.z = in.z;
    region.w = detail::regionWidth(in);
    region.h = detail::regionHeight(in);
    region.d = in.depth;
    return region;
}
inline SDL_GPUBuffer* buffer(const BufferRef& ref) { return static_cast<SDL_GPUBuffer*>(ref.native); }
inline SDL_GPUSampler* sampler(const SamplerRef& ref) { return static_cast<SDL_GPUSampler*>(ref.native); }

} // namespace quench::gpu::sdl
