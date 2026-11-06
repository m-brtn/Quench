// The native Metal backend: the device, resources, presentation and fences (MetalCommands.mm records commands).

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#include "gpu/metal/MetalDevice.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <gpu_blit_shaders.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

namespace quench::gpu::detail {

namespace {

std::string describe(std::string_view kind, std::string_view name) {
    return name.empty() ? std::string(kind) : std::format("{} '{}'", kind, name);
}

std::string errorText(NSError* error) {
    return error ? std::string(error.localizedDescription.UTF8String) : std::string("unknown error");
}

NSString* label(std::string_view name) {
    return [[NSString alloc] initWithBytes:name.data() length:name.size() encoding:NSUTF8StringEncoding];
}

MTLTextureType textureType(TextureType type, bool multisample) {
    switch (type) {
    case TextureType::Texture2D: return multisample ? MTLTextureType2DMultisample : MTLTextureType2D;
    case TextureType::Texture2DArray: return MTLTextureType2DArray;
    case TextureType::Texture3D: return MTLTextureType3D;
    case TextureType::Cube: return MTLTextureTypeCube;
    case TextureType::CubeArray: return MTLTextureTypeCubeArray;
    }
    return MTLTextureType2D;
}

MTLVertexFormat vertexFormat(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float: return MTLVertexFormatFloat;
    case VertexFormat::Float2: return MTLVertexFormatFloat2;
    case VertexFormat::Float3: return MTLVertexFormatFloat3;
    case VertexFormat::Float4: return MTLVertexFormatFloat4;
    case VertexFormat::Half2: return MTLVertexFormatHalf2;
    case VertexFormat::Half4: return MTLVertexFormatHalf4;
    case VertexFormat::Uint: return MTLVertexFormatUInt;
    case VertexFormat::Uint2: return MTLVertexFormatUInt2;
    case VertexFormat::Uint4: return MTLVertexFormatUInt4;
    case VertexFormat::Ubyte4: return MTLVertexFormatUChar4;
    case VertexFormat::Ubyte4Norm: return MTLVertexFormatUChar4Normalized;
    case VertexFormat::Short2Norm: return MTLVertexFormatShort2Normalized;
    case VertexFormat::Short4Norm: return MTLVertexFormatShort4Normalized;
    case VertexFormat::Ushort4: return MTLVertexFormatUShort4;
    case VertexFormat::Ushort4Norm: return MTLVertexFormatUShort4Normalized;
    }
    return MTLVertexFormatInvalid;
}

MTLBlendFactor blendFactor(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero: return MTLBlendFactorZero;
    case BlendFactor::One: return MTLBlendFactorOne;
    case BlendFactor::SrcColor: return MTLBlendFactorSourceColor;
    case BlendFactor::OneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::DstColor: return MTLBlendFactorDestinationColor;
    case BlendFactor::OneMinusDstColor: return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
    case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
    case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::ConstantColor: return MTLBlendFactorBlendColor;
    case BlendFactor::OneMinusConstantColor: return MTLBlendFactorOneMinusBlendColor;
    case BlendFactor::SrcAlphaSaturate: return MTLBlendFactorSourceAlphaSaturated;
    }
    return MTLBlendFactorZero;
}

MTLBlendOperation blendOp(BlendOp op) {
    switch (op) {
    case BlendOp::Add: return MTLBlendOperationAdd;
    case BlendOp::Subtract: return MTLBlendOperationSubtract;
    case BlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
    case BlendOp::Min: return MTLBlendOperationMin;
    case BlendOp::Max: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

MTLCompareFunction compareFunction(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return MTLCompareFunctionNever;
    case CompareOp::Less: return MTLCompareFunctionLess;
    case CompareOp::Equal: return MTLCompareFunctionEqual;
    case CompareOp::LessOrEqual: return MTLCompareFunctionLessEqual;
    case CompareOp::Greater: return MTLCompareFunctionGreater;
    case CompareOp::NotEqual: return MTLCompareFunctionNotEqual;
    case CompareOp::GreaterOrEqual: return MTLCompareFunctionGreaterEqual;
    case CompareOp::Always: return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

MTLSamplerAddressMode addressMode(AddressMode mode) {
    switch (mode) {
    case AddressMode::Repeat: return MTLSamplerAddressModeRepeat;
    case AddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
    case AddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
    }
    return MTLSamplerAddressModeRepeat;
}

MTLPrimitiveType primitiveType(PrimitiveType type) {
    switch (type) {
    case PrimitiveType::TriangleList: return MTLPrimitiveTypeTriangle;
    case PrimitiveType::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    case PrimitiveType::LineList: return MTLPrimitiveTypeLine;
    case PrimitiveType::LineStrip: return MTLPrimitiveTypeLineStrip;
    case PrimitiveType::PointList: return MTLPrimitiveTypePoint;
    }
    return MTLPrimitiveTypeTriangle;
}

MTLColorWriteMask writeMask(ColorWrite mask) {
    MTLColorWriteMask result = MTLColorWriteMaskNone;
    if (any(mask & ColorWrite::R)) result |= MTLColorWriteMaskRed;
    if (any(mask & ColorWrite::G)) result |= MTLColorWriteMaskGreen;
    if (any(mask & ColorWrite::B)) result |= MTLColorWriteMaskBlue;
    if (any(mask & ColorWrite::A)) result |= MTLColorWriteMaskAlpha;
    return result;
}

MetalStageCounts stageCounts(const ShaderBlob& blob) {
    return {blob.resources.samplers, blob.resources.storageTextures, blob.resources.storageBuffers,
            blob.resources.uniformBuffers, blob.resources.accelerationStructures};
}

bool isBc(TextureFormat format) {
    return format >= TextureFormat::Bc1RgbaUnorm && format <= TextureFormat::Bc7RgbaUnormSrgb;
}

bool isAstc(TextureFormat format) {
    return format == TextureFormat::Astc4x4Unorm || format == TextureFormat::Astc4x4UnormSrgb;
}

// The swapchain format and color space of each composition, as SDL GPU sets them.
TextureFormat compositionFormat(SwapchainComposition composition) {
    switch (composition) {
    case SwapchainComposition::Sdr: return TextureFormat::Bgra8Unorm;
    case SwapchainComposition::SdrLinear: return TextureFormat::Bgra8UnormSrgb;
    case SwapchainComposition::HdrExtendedLinear: return TextureFormat::Rgba16Float;
    case SwapchainComposition::Hdr10: return TextureFormat::Rgb10a2Unorm;
    }
    return TextureFormat::Bgra8Unorm;
}

CFStringRef compositionColorSpace(SwapchainComposition composition) {
    switch (composition) {
    case SwapchainComposition::Sdr:
    case SwapchainComposition::SdrLinear: return kCGColorSpaceSRGB;
    case SwapchainComposition::HdrExtendedLinear: return kCGColorSpaceExtendedLinearSRGB;
    case SwapchainComposition::Hdr10: return kCGColorSpaceITUR_2100_PQ;
    }
    return kCGColorSpaceSRGB;
}

} // namespace

MTLPrimitiveAccelerationStructureDescriptor*
metal::bottomLevelDescriptor(const RayGeometry* geometry, std::uint32_t triangleCount, bool allowRefit) {
    MTLAccelerationStructureTriangleGeometryDescriptor* g =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    g.vertexFormat = MTLAttributeFormatFloat3;
    g.indexType = MTLIndexTypeUInt32;
    g.triangleCount = triangleCount;
    g.opaque = YES; // instances decide (RayInstanceFlags)
    if (geometry) {
        g.vertexBuffer = static_cast<MetalBuffer*>(geometry->vertices.native)->handle;
        g.vertexBufferOffset = geometry->vertexOffset;
        g.vertexStride = geometry->vertexStride;
        g.indexBuffer = static_cast<MetalBuffer*>(geometry->indices.native)->handle;
        g.indexBufferOffset = geometry->indexOffset;
    }
    MTLPrimitiveAccelerationStructureDescriptor* d = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    d.geometryDescriptors = @[ g ];
    d.usage = allowRefit ? MTLAccelerationStructureUsageRefit : MTLAccelerationStructureUsageNone;
    return d;
}

MTLPixelFormat metal::pixelFormat(TextureFormat format) {
    switch (format) {
    case TextureFormat::Invalid: return MTLPixelFormatInvalid;
    case TextureFormat::R8Unorm: return MTLPixelFormatR8Unorm;
    case TextureFormat::Rg8Unorm: return MTLPixelFormatRG8Unorm;
    case TextureFormat::Rgba8Unorm: return MTLPixelFormatRGBA8Unorm;
    case TextureFormat::Rgba8UnormSrgb: return MTLPixelFormatRGBA8Unorm_sRGB;
    case TextureFormat::Bgra8Unorm: return MTLPixelFormatBGRA8Unorm;
    case TextureFormat::Bgra8UnormSrgb: return MTLPixelFormatBGRA8Unorm_sRGB;
    case TextureFormat::Rgb10a2Unorm: return MTLPixelFormatRGB10A2Unorm;
    case TextureFormat::R16Float: return MTLPixelFormatR16Float;
    case TextureFormat::Rg16Float: return MTLPixelFormatRG16Float;
    case TextureFormat::Rgba16Float: return MTLPixelFormatRGBA16Float;
    case TextureFormat::R32Float: return MTLPixelFormatR32Float;
    case TextureFormat::Rg32Float: return MTLPixelFormatRG32Float;
    case TextureFormat::Rgba32Float: return MTLPixelFormatRGBA32Float;
    case TextureFormat::Rg11b10Ufloat: return MTLPixelFormatRG11B10Float;
    case TextureFormat::R32Uint: return MTLPixelFormatR32Uint;
    case TextureFormat::Bc1RgbaUnorm: return MTLPixelFormatBC1_RGBA;
    case TextureFormat::Bc1RgbaUnormSrgb: return MTLPixelFormatBC1_RGBA_sRGB;
    case TextureFormat::Bc3RgbaUnorm: return MTLPixelFormatBC3_RGBA;
    case TextureFormat::Bc3RgbaUnormSrgb: return MTLPixelFormatBC3_RGBA_sRGB;
    case TextureFormat::Bc4RUnorm: return MTLPixelFormatBC4_RUnorm;
    case TextureFormat::Bc5RgUnorm: return MTLPixelFormatBC5_RGUnorm;
    case TextureFormat::Bc6hRgbUfloat: return MTLPixelFormatBC6H_RGBUfloat;
    case TextureFormat::Bc7RgbaUnorm: return MTLPixelFormatBC7_RGBAUnorm;
    case TextureFormat::Bc7RgbaUnormSrgb: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    case TextureFormat::Astc4x4Unorm: return MTLPixelFormatASTC_4x4_LDR;
    case TextureFormat::Astc4x4UnormSrgb: return MTLPixelFormatASTC_4x4_sRGB;
    case TextureFormat::Depth16Unorm: return MTLPixelFormatDepth16Unorm;
    case TextureFormat::Depth24UnormStencil8:
#if QUENCH_PLATFORM_MACOS
        return MTLPixelFormatDepth24Unorm_Stencil8;
#else
        return MTLPixelFormatInvalid;
#endif
    case TextureFormat::Depth32Float: return MTLPixelFormatDepth32Float;
    case TextureFormat::Depth32FloatStencil8: return MTLPixelFormatDepth32Float_Stencil8;
    case TextureFormat::Count: break;
    }
    return MTLPixelFormatInvalid;
}

// ------------------------------------------------------------------ device

Result<std::unique_ptr<DeviceImpl>> createMetalDevice(const DeviceDesc& desc) { return MetalDevice::create(desc); }

Result<std::unique_ptr<DeviceImpl>> MetalDevice::create(const DeviceDesc& desc) {
    @autoreleasepool {
        if (!desc.driver.empty() && desc.driver != "metal")
            return Error{std::format("the native Metal backend has no driver '{}'", desc.driver)};
        // As SDL GPU does: Metal reads it when the first device is created.
        if (desc.debug) setenv("MTL_DEBUG_LAYER", "1", 0);

        id<MTLDevice> device = nil;
#if QUENCH_PLATFORM_MACOS
        if (desc.preferLowPower) {
            for (id<MTLDevice> candidate in MTLCopyAllDevices())
                if (candidate.isLowPower) {
                    device = candidate;
                    break;
                }
        }
#endif
        if (!device) device = MTLCreateSystemDefaultDevice();
        if (!device) return Error{"cannot create a Metal device"};
#if QUENCH_PLATFORM_MACOS
        const bool supported = [device supportsFamily:MTLGPUFamilyMac2];
#elif QUENCH_PLATFORM_IOS_SIMULATOR
        const bool supported = true; // the Simulator's device reports Apple2 only (as SDL's patch accepts)
#else
        const bool supported = [device supportsFamily:MTLGPUFamilyApple3];
#endif
        if (!supported)
            return Error{std::format("Metal device '{}' is below the required GPU family", device.name.UTF8String)};

        auto impl = std::make_unique<MetalDevice>();
        impl->m_device = device;
        impl->m_queue = [device newCommandQueue];
        impl->m_debug = desc.debug;
        if (!impl->m_queue) return Error{"cannot create a Metal command queue"};
        impl->m_zeroUniforms = [device newBufferWithLength:metal::kUniformChunkSize
                                                   options:MTLResourceStorageModePrivate];

        Caps& caps = impl->caps;
        caps.backend = Backend::Metal;
        caps.driver = "metal";
        caps.deviceName = device.name.UTF8String;
#if QUENCH_SHADERS_METALLIB
        caps.shaderFormat = ShaderFormat::Metallib;
#else
        caps.shaderFormat = ShaderFormat::Msl;
#endif
        caps.gpuTiming = true;
        caps.depthClamp = !QUENCH_PLATFORM_IOS_SIMULATOR;
        caps.depthBiasClamp = true;
        caps.indirectDraw = !QUENCH_PLATFORM_IOS_SIMULATOR;

        // Timestamps at encoder (stage) boundaries: Apple GPUs sample there, not between draws.
        if ([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            for (id<MTLCounterSet> set in device.counterSets)
                if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) impl->m_timestampSet = set;
        }
        caps.gpuPassTiming = impl->m_timestampSet != nil;
        if (caps.gpuPassTiming) {
            MTLTimestamp cpu = 0;
            [device sampleTimestamps:&cpu gpuTimestamp:&impl->m_calibrationGpu];
            impl->m_calibrationCpu = std::chrono::steady_clock::now();
        }
        // Ray queries in fragment shaders need ray tracing from render pipelines too.
        caps.rayTracing = device.supportsRaytracing && device.supportsRaytracingFromRender;
#if !QUENCH_PLATFORM_IOS_SIMULATOR
        caps.spatialUpscaling = [MTLFXSpatialScalerDescriptor supportsDevice:device];
        caps.temporalUpscaling = [MTLFXTemporalScalerDescriptor supportsDevice:device];
#endif

        // Blit: the engine's shader (src/gpu/shaders/blit.hlsl) and SDL's two samplers.
        const ShaderBlob* blobs[] = {shaders::gpu_blit.find("fullscreen_vs"), shaders::gpu_blit.find("blit_2d_fs"),
                                     shaders::gpu_blit.find("blit_2d_array_fs")};
        QUENCH_ASSERT(blobs[0] && blobs[1] && blobs[2], "blit shaders missing from the build");
        Result<id<MTLFunction>> vertex = impl->function(*blobs[0]);
        Result<id<MTLFunction>> blit2d = impl->function(*blobs[1]);
        Result<id<MTLFunction>> blitArray = impl->function(*blobs[2]);
        if (!vertex || !blit2d || !blitArray)
            return Error{std::format("blit shaders: {}", !vertex   ? vertex.error()
                                                         : !blit2d ? blit2d.error()
                                                                   : blitArray.error())};
        impl->m_blitVertex = *vertex;
        impl->m_blit2d = *blit2d;
        impl->m_blit2dArray = *blitArray;
        MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
        sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler.rAddressMode = MTLSamplerAddressModeClampToEdge;
        sampler.minFilter = MTLSamplerMinMagFilterNearest;
        sampler.magFilter = MTLSamplerMinMagFilterNearest;
        sampler.mipFilter = MTLSamplerMipFilterNearest;
        impl->m_blitNearest = [device newSamplerStateWithDescriptor:sampler];
        sampler.minFilter = MTLSamplerMinMagFilterLinear;
        sampler.magFilter = MTLSamplerMinMagFilterLinear;
        sampler.mipFilter = MTLSamplerMipFilterLinear;
        impl->m_blitLinear = [device newSamplerStateWithDescriptor:sampler];
        return std::unique_ptr<DeviceImpl>(std::move(impl));
    }
}

MetalDevice::~MetalDevice() {
    @autoreleasepool {
        for (const std::unique_ptr<MetalCommandBuffer>& commands : m_live)
            if (commands->submitted) [commands->handle waitUntilCompleted];
        // Their timing records return sample buffers to m_samplePool, which goes after them.
        m_live.clear();
        m_free.clear();
        m_windows.clear();
    }
}

bool MetalDevice::supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const {
    @autoreleasepool {
        if (any(usage & TextureUsage::DepthStencilTarget) && !formatInfo(format).depth) return false;
#if QUENCH_PLATFORM_IOS
        if (type == TextureType::CubeArray &&
            !([m_device supportsFamily:MTLGPUFamilyCommon2] || [m_device supportsFamily:MTLGPUFamilyApple4]))
            return false;
#else
        (void)type;
#endif
        if (isBc(format)) return !any(usage & TextureUsage::ColorTarget) && m_device.supportsBCTextureCompression;
        if (isAstc(format)) {
#if QUENCH_PLATFORM_MACOS
            return [m_device supportsFamily:MTLGPUFamilyApple7];
#else
            return true;
#endif
        }
        if (format == TextureFormat::Depth24UnormStencil8) {
#if QUENCH_PLATFORM_MACOS
            return m_device.depth24Stencil8PixelFormatSupported;
#else
            return false;
#endif
        }
        return metal::pixelFormat(format) != MTLPixelFormatInvalid;
    }
}

bool MetalDevice::supportsSampleCount(TextureFormat, std::uint32_t sampleCount) const {
    return [m_device supportsTextureSampleCount:sampleCount];
}

void MetalDevice::waitIdle() {
    @autoreleasepool {
        for (const std::unique_ptr<MetalCommandBuffer>& commands : m_live)
            if (commands->submitted) [commands->handle waitUntilCompleted];
        reclaim();
    }
}

// ------------------------------------------------------------------ resources

Result<id<MTLFunction>> MetalDevice::function(const ShaderBlob& blob) const {
    const ShaderCode code = shaderCodeFor(blob, caps.shaderFormat);
    if (!code)
        return Error{
            std::format("shader '{}' has no {} code in this build", blob.entry, shaderFormatName(caps.shaderFormat))};
    NSError* error = nil;
    id<MTLLibrary> library = nil;
    if (caps.shaderFormat == ShaderFormat::Metallib) {
        if (code.size < 4 || std::memcmp(code.data, "MTLB", 4) != 0)
            return Error{std::format("shader '{}': not a Metal library", blob.entry)};
        dispatch_data_t data = dispatch_data_create(code.data, code.size, dispatch_get_global_queue(0, 0),
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        library = [m_device newLibraryWithData:data error:&error];
    } else {
        NSString* source = [[NSString alloc] initWithBytes:code.data length:code.size encoding:NSUTF8StringEncoding];
        library = [m_device newLibraryWithSource:source options:nil error:&error];
    }
    if (!library) return Error{std::format("shader '{}': {}", blob.entry, errorText(error))};
    id<MTLFunction> function = [library newFunctionWithName:@(blob.entry)];
    if (!function) return Error{std::format("shader '{}': no function of that name in its library", blob.entry)};
    return function;
}

Result<void*> MetalDevice::createTexture(const TextureDesc& desc) {
    @autoreleasepool {
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = textureType(desc.type, desc.sampleCount > 1);
        d.pixelFormat = metal::pixelFormat(desc.format);
        d.width = desc.width;
        d.height = desc.height;
        d.depth = desc.type == TextureType::Texture3D ? desc.layers : 1;
        d.mipmapLevelCount = desc.mipLevels;
        d.sampleCount = desc.sampleCount;
        // Cube arrays count cubes. SDL GPU passes the layer count here, which allocates six times the cubes.
        d.arrayLength = desc.type == TextureType::Texture2DArray ? desc.layers
                        : desc.type == TextureType::CubeArray    ? std::max(1u, desc.layers / 6)
                                                                 : 1;
        d.storageMode = MTLStorageModePrivate;
        MTLTextureUsage usage = 0;
        if (any(desc.usage & (TextureUsage::ColorTarget | TextureUsage::DepthStencilTarget)))
            usage |= MTLTextureUsageRenderTarget;
        if (any(desc.usage & (TextureUsage::Sampled | TextureUsage::GraphicsStorageRead)))
            usage |= MTLTextureUsageShaderRead;
        // MetalFX writes its output from compute or render passes of its own (its outputTextureUsage).
        if (any(desc.usage & TextureUsage::ShaderWrite))
            usage |= MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        d.usage = usage;
        id<MTLTexture> texture = [m_device newTextureWithDescriptor:d];
        if (!texture)
            return Error{std::format("cannot create {} {}x{} {}", describe("texture", desc.name), desc.width,
                                     desc.height, formatInfo(desc.format).name)};
        if (m_debug && !desc.name.empty()) texture.label = label(desc.name);
        return static_cast<void*>(new MetalTexture{texture});
    }
}

Result<void*> MetalDevice::createBuffer(const BufferDesc& desc) {
    @autoreleasepool {
        const NSUInteger size = (NSUInteger(desc.size) + 3) & ~NSUInteger(3);
        id<MTLBuffer> buffer = [m_device newBufferWithLength:size options:MTLResourceStorageModePrivate];
        if (!buffer)
            return Error{std::format("cannot create {} of {} bytes", describe("buffer", desc.name), desc.size)};
        if (m_debug && !desc.name.empty()) buffer.label = label(desc.name);
        return static_cast<void*>(new MetalBuffer{buffer});
    }
}

Result<void*> MetalDevice::createSampler(const SamplerDesc& desc) {
    @autoreleasepool {
        MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
        d.sAddressMode = addressMode(desc.addressU);
        d.tAddressMode = addressMode(desc.addressV);
        d.rAddressMode = addressMode(desc.addressW);
        d.minFilter = desc.minFilter == Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.magFilter = desc.magFilter == Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = desc.mipmapMode == MipmapMode::Linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        d.lodMinClamp = desc.minLod;
        d.lodMaxClamp = desc.maxLod;
        // Metal samplers have no LOD bias (SDL GPU's Metal backend ignores it too).
        d.maxAnisotropy = desc.maxAnisotropy > 1.0f ? NSUInteger(std::min(desc.maxAnisotropy, 16.0f)) : 1;
        d.compareFunction = desc.compare ? compareFunction(desc.compareOp) : MTLCompareFunctionAlways;
        if (m_debug && !desc.name.empty()) d.label = label(desc.name);
        id<MTLSamplerState> sampler = [m_device newSamplerStateWithDescriptor:d];
        if (!sampler) return Error{"cannot create " + describe("sampler", desc.name)};
        return static_cast<void*>(new MetalSampler{sampler});
    }
}

Result<void*> MetalDevice::createTransferBuffer(const TransferBufferDesc& desc) {
    @autoreleasepool {
        auto buffer = std::make_unique<MetalTransferBuffer>();
        buffer->size = desc.size;
        buffer->options = MTLResourceStorageModeShared |
                          (desc.direction == TransferDirection::Upload ? MTLResourceCPUCacheModeWriteCombined
                                                                       : MTLResourceCPUCacheModeDefaultCache);
        id<MTLBuffer> backing = [m_device newBufferWithLength:desc.size options:buffer->options];
        if (!backing)
            return Error{
                std::format("cannot create {} of {} bytes", describe("transfer buffer", desc.name), desc.size)};
        buffer->backings.push_back({backing, 0});
        return static_cast<void*>(buffer.release());
    }
}

Result<void*> MetalDevice::createPipeline(const GraphicsPipelineDesc& desc) {
    @autoreleasepool {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        for (std::size_t i = 0; i < desc.colorTargets.size(); ++i) {
            const ColorTargetDesc& in = desc.colorTargets[i];
            MTLRenderPipelineColorAttachmentDescriptor* color = d.colorAttachments[i];
            color.pixelFormat = metal::pixelFormat(in.format);
            color.writeMask = writeMask(in.blend.writeMask);
            color.blendingEnabled = in.blend.enabled;
            color.rgbBlendOperation = blendOp(in.blend.colorOp);
            color.alphaBlendOperation = blendOp(in.blend.alphaOp);
            color.sourceRGBBlendFactor = blendFactor(in.blend.srcColor);
            color.sourceAlphaBlendFactor = blendFactor(in.blend.srcAlpha);
            color.destinationRGBBlendFactor = blendFactor(in.blend.dstColor);
            color.destinationAlphaBlendFactor = blendFactor(in.blend.dstAlpha);
        }
        d.rasterSampleCount = desc.sampleCount;
        d.alphaToCoverageEnabled = desc.alphaToCoverage;

        id<MTLDepthStencilState> depthStencil = nil;
        if (desc.depthFormat != TextureFormat::Invalid) {
            d.depthAttachmentPixelFormat = metal::pixelFormat(desc.depthFormat);
            if (formatInfo(desc.depthFormat).stencil)
                d.stencilAttachmentPixelFormat = metal::pixelFormat(desc.depthFormat);
            MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
            ds.depthCompareFunction = desc.depth.test ? compareFunction(desc.depth.compare) : MTLCompareFunctionAlways;
            // No writes without the test, as in the other APIs.
            ds.depthWriteEnabled = desc.depth.write && desc.depth.test;
            depthStencil = [m_device newDepthStencilStateWithDescriptor:ds];
        }

        Result<id<MTLFunction>> vs = function(*desc.vertexShader);
        if (!vs) return Error{vs.error()};
        Result<id<MTLFunction>> fs = function(*desc.fragmentShader);
        if (!fs) return Error{fs.error()};
        d.vertexFunction = *vs;
        d.fragmentFunction = *fs;

        if (!desc.vertexBuffers.empty()) {
            MTLVertexDescriptor* vertex = [MTLVertexDescriptor vertexDescriptor];
            for (const VertexAttribute& in : desc.vertexAttributes) {
                MTLVertexAttributeDescriptor* attribute = vertex.attributes[in.location];
                attribute.format = vertexFormat(in.format);
                attribute.offset = in.offset;
                attribute.bufferIndex = metal::kFirstVertexBufferSlot + in.slot;
            }
            for (const VertexBufferLayout& in : desc.vertexBuffers) {
                MTLVertexBufferLayoutDescriptor* layout = vertex.layouts[metal::kFirstVertexBufferSlot + in.slot];
                layout.stepFunction = in.rate == VertexInputRate::PerInstance ? MTLVertexStepFunctionPerInstance
                                                                              : MTLVertexStepFunctionPerVertex;
                layout.stepRate = 1;
                layout.stride = in.stride;
            }
            d.vertexDescriptor = vertex;
        }

        const std::string name = desc.name.empty()
                                     ? std::format("{}/{}", desc.vertexShader->entry, desc.fragmentShader->entry)
                                     : std::string(desc.name);
        if (m_debug) d.label = label(name);
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [m_device newRenderPipelineStateWithDescriptor:d error:&error];
        if (!state) return Error{std::format("cannot create pipeline '{}': {}", name, errorText(error))};

        auto pipeline = std::make_unique<MetalGraphicsPipeline>();
        pipeline->state = state;
        pipeline->depthStencil = depthStencil;
        pipeline->primitive = primitiveType(desc.primitive);
        pipeline->fill = desc.rasterizer.fill == FillMode::Line ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
        pipeline->cull = desc.rasterizer.cull == CullMode::Front  ? MTLCullModeFront
                         : desc.rasterizer.cull == CullMode::Back ? MTLCullModeBack
                                                                  : MTLCullModeNone;
        pipeline->winding =
            desc.rasterizer.frontFace == FrontFace::Clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
        const bool clip = desc.rasterizer.depthClip || !caps.depthClamp;
        pipeline->depthClip = clip ? MTLDepthClipModeClip : MTLDepthClipModeClamp;
        if (desc.rasterizer.depthBias) {
            pipeline->depthBias = desc.rasterizer.depthBiasConstant;
            pipeline->depthBiasSlope = desc.rasterizer.depthBiasSlope;
            pipeline->depthBiasClamp = desc.rasterizer.depthBiasClamp;
        }
        pipeline->vertex = stageCounts(*desc.vertexShader);
        pipeline->fragment = stageCounts(*desc.fragmentShader);
        QUENCH_ASSERT(pipeline->vertex.uniformBuffers <= metal::kUniformSlots &&
                          pipeline->fragment.uniformBuffers <= metal::kUniformSlots,
                      "more uniform blocks than the backend binds");
        return static_cast<void*>(pipeline.release());
    }
}

Result<void*> MetalDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    @autoreleasepool {
        const ShaderBlob& blob = *desc.shader;
        Result<id<MTLFunction>> fn = function(blob);
        if (!fn) return Error{fn.error()};
        const std::string name = desc.name.empty() ? std::string(blob.entry) : std::string(desc.name);
        MTLComputePipelineDescriptor* d = [MTLComputePipelineDescriptor new];
        d.computeFunction = *fn;
        if (m_debug) d.label = label(name);
        NSError* error = nil;
        id<MTLComputePipelineState> state = [m_device newComputePipelineStateWithDescriptor:d
                                                                                    options:MTLPipelineOptionNone
                                                                                 reflection:nil
                                                                                      error:&error];
        if (!state) return Error{std::format("cannot create compute pipeline '{}': {}", name, errorText(error))};
        auto pipeline = std::make_unique<MetalComputePipeline>();
        pipeline->state = state;
        pipeline->storageBuffers = blob.resources.storageBuffers;
        pipeline->readWriteStorageBuffers = blob.resources.readWriteStorageBuffers;
        pipeline->uniformBuffers = blob.resources.uniformBuffers;
        pipeline->accelerationStructures = blob.resources.accelerationStructures;
        pipeline->threads =
            MTLSizeMake(blob.resources.threadCount[0], blob.resources.threadCount[1], blob.resources.threadCount[2]);
        QUENCH_ASSERT(pipeline->uniformBuffers <= metal::kUniformSlots, "more uniform blocks than the backend binds");
        return static_cast<void*>(pipeline.release());
    }
}

Result<void*> MetalDevice::createAccelerationStructure(const AccelerationStructureDesc& desc) {
    @autoreleasepool {
        MTLAccelerationStructureDescriptor* d = nil;
        if (desc.level == AccelerationStructureLevel::Bottom) {
            d = metal::bottomLevelDescriptor(nullptr, desc.triangleCount, desc.allowRefit);
        } else {
            MTLInstanceAccelerationStructureDescriptor* top = [MTLInstanceAccelerationStructureDescriptor descriptor];
            top.instanceCount = desc.instanceCapacity;
            top.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
            d = top;
        }
        const MTLAccelerationStructureSizes sizes = [m_device accelerationStructureSizesWithDescriptor:d];
        id<MTLAccelerationStructure> handle =
            [m_device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        if (!handle)
            return Error{std::format("cannot create {} of {} bytes", describe("acceleration structure", desc.name),
                                     sizes.accelerationStructureSize)};
        if (m_debug && !desc.name.empty()) handle.label = label(desc.name);
        auto structure = std::make_unique<MetalAccelerationStructure>();
        structure->handle = handle;
        structure->level = desc.level;
        structure->buildScratch = sizes.buildScratchBufferSize;
        structure->refitScratch = sizes.refitScratchBufferSize;
        structure->allowRefit = desc.allowRefit;
        return static_cast<void*>(structure.release());
    }
}

Result<void*> MetalDevice::createUpscaler(const UpscalerDesc& desc) {
#if QUENCH_PLATFORM_IOS_SIMULATOR
    (void)desc;
    return Error{"no MetalFX in the iOS Simulator"};
#else
    @autoreleasepool {
        auto upscaler = std::make_unique<MetalUpscaler>();
        if (desc.type == UpscalerType::Spatial) {
            MTLFXSpatialScalerDescriptor* d = [MTLFXSpatialScalerDescriptor new];
            d.colorTextureFormat = metal::pixelFormat(desc.colorFormat);
            d.outputTextureFormat = metal::pixelFormat(desc.outputFormat);
            d.inputWidth = desc.inputWidth;
            d.inputHeight = desc.inputHeight;
            d.outputWidth = desc.outputWidth;
            d.outputHeight = desc.outputHeight;
            // Linear HDR scene values: MetalFX's perceptual mode expects display values.
            d.colorProcessingMode = desc.hdrInput ? MTLFXSpatialScalerColorProcessingModeHDR
                                                  : MTLFXSpatialScalerColorProcessingModePerceptual;
            upscaler->spatial = [d newSpatialScalerWithDevice:m_device];
            if (!upscaler->spatial) return Error{"cannot create a MetalFX spatial scaler"};
        } else {
            MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
            d.colorTextureFormat = metal::pixelFormat(desc.colorFormat);
            d.depthTextureFormat = metal::pixelFormat(desc.depthFormat);
            d.motionTextureFormat = metal::pixelFormat(desc.motionFormat);
            d.outputTextureFormat = metal::pixelFormat(desc.outputFormat);
            d.inputWidth = desc.inputWidth;
            d.inputHeight = desc.inputHeight;
            d.outputWidth = desc.outputWidth;
            d.outputHeight = desc.outputHeight;
            // Created ready to run at full speed: the first frames are not slower than the rest.
            d.requiresSynchronousInitialization = YES;
            upscaler->temporal = [d newTemporalScalerWithDevice:m_device];
            if (!upscaler->temporal) return Error{"cannot create a MetalFX temporal scaler"};
            upscaler->temporal.depthReversed = desc.reversedDepth;
        }
        // TextureUsage::ShaderWrite gives an output these usages; Sampled gives the inputs ShaderRead.
        const MTLTextureUsage outputUsage =
            upscaler->spatial ? upscaler->spatial.outputTextureUsage : upscaler->temporal.outputTextureUsage;
        constexpr MTLTextureUsage kOutputUsage =
            MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        if ((outputUsage & ~kOutputUsage) != 0)
            log::warning("gpu", "MetalFX wants output texture usage {:#x}; TextureUsage::ShaderWrite gives {:#x}",
                         std::uint64_t(outputUsage), std::uint64_t(kOutputUsage));
        return static_cast<void*>(upscaler.release());
    }
#endif
}

void MetalDevice::release(Kind kind, void* native) noexcept {
    // Command buffers retain what they use, so the objects live on until the GPU is done with them.
    @autoreleasepool {
        switch (kind) {
        case Kind::Buffer: delete static_cast<MetalBuffer*>(native); break;
        case Kind::Texture: delete static_cast<MetalTexture*>(native); break;
        case Kind::Sampler: delete static_cast<MetalSampler*>(native); break;
        case Kind::TransferBuffer: delete static_cast<MetalTransferBuffer*>(native); break;
        case Kind::GraphicsPipeline: delete static_cast<MetalGraphicsPipeline*>(native); break;
        case Kind::ComputePipeline: delete static_cast<MetalComputePipeline*>(native); break;
        case Kind::Fence: delete static_cast<MetalFence*>(native); break;
        case Kind::AccelerationStructure: delete static_cast<MetalAccelerationStructure*>(native); break;
        case Kind::Upscaler: delete static_cast<MetalUpscaler*>(native); break;
        }
    }
}

std::span<std::byte> MetalDevice::map(void* transferBuffer, std::uint32_t size, bool cycle) {
    @autoreleasepool {
        MetalTransferBuffer& buffer = *static_cast<MetalTransferBuffer*>(transferBuffer);
        if (cycle) {
            reclaim();
            const std::uint64_t finished = finishedBelow();
            if (buffer.current().lastUse >= finished) {
                auto it =
                    std::find_if(buffer.backings.begin(), buffer.backings.end(),
                                 [finished](const MetalTransferBuffer::Backing& b) { return b.lastUse < finished; });
                if (it == buffer.backings.end()) {
                    id<MTLBuffer> backing = [m_device newBufferWithLength:buffer.size options:buffer.options];
                    if (!backing) {
                        log::error("gpu", "cannot cycle a transfer buffer of {} bytes", buffer.size);
                        return {};
                    }
                    buffer.backings.push_back({backing, 0});
                    it = buffer.backings.end() - 1;
                }
                buffer.active = std::size_t(it - buffer.backings.begin());
            }
        }
        return {static_cast<std::byte*>(buffer.current().handle.contents), size};
    }
}

void MetalDevice::unmap(void*) {}

// ------------------------------------------------------------------ presentation

MetalWindow* MetalDevice::findWindow(SDL_Window* window) const {
    for (const std::unique_ptr<MetalWindow>& w : m_windows)
        if (w->window == window) return w.get();
    return nullptr;
}

Result<void> MetalDevice::claimWindow(SDL_Window* window) {
    @autoreleasepool {
        auto w = std::make_unique<MetalWindow>();
        w->window = window;
        w->view = SDL_Metal_CreateView(window);
        if (!w->view) return Error{std::format("SDL_Metal_CreateView: {}", SDL_GetError())};
        w->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(w->view);
        w->layer.device = m_device;
        m_windows.push_back(std::move(w));
        if (!setSwapchainParameters(window, SwapchainComposition::Sdr, PresentMode::Vsync)) {
            SDL_Metal_DestroyView(m_windows.back()->view);
            m_windows.pop_back();
            return Error{"cannot set up the window's Metal layer"};
        }
        return {};
    }
}

void MetalDevice::releaseWindow(SDL_Window* window) {
    @autoreleasepool {
        auto it = std::find_if(m_windows.begin(), m_windows.end(),
                               [window](const std::unique_ptr<MetalWindow>& w) { return w->window == window; });
        if (it == m_windows.end()) return;
        waitIdle();
        (*it)->drawable = nil;
        (*it)->texture.handle = nil;
        (*it)->layer = nil;
        SDL_Metal_DestroyView((*it)->view);
        m_windows.erase(it);
    }
}

bool MetalDevice::supportsPresentMode(SDL_Window*, PresentMode mode) const {
    // displaySyncEnabled exists on macOS only; there is no mailbox mode on a CAMetalLayer.
    return mode == PresentMode::Vsync || (mode == PresentMode::Immediate && QUENCH_PLATFORM_MACOS);
}

bool MetalDevice::supportsComposition(SDL_Window*, SwapchainComposition composition) const {
    return composition != SwapchainComposition::Hdr10 || QUENCH_PLATFORM_MACOS;
}

bool MetalDevice::setSwapchainParameters(SDL_Window* window, SwapchainComposition composition, PresentMode mode) {
    @autoreleasepool {
        MetalWindow* w = findWindow(window);
        if (!w) {
            log::error("gpu", "swapchain parameters for a window the Metal device has not claimed");
            return false;
        }
        waitIdle();
        w->format = compositionFormat(composition);
        w->layer.pixelFormat = metal::pixelFormat(w->format);
#if QUENCH_PLATFORM_MACOS
        w->layer.displaySyncEnabled = mode != PresentMode::Immediate;
#else
        (void)mode;
#endif
        w->layer.wantsExtendedDynamicRangeContent = composition != SwapchainComposition::Sdr;
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(compositionColorSpace(composition));
        w->layer.colorspace = colorSpace;
        CGColorSpaceRelease(colorSpace);
        return true;
    }
}

TextureFormat MetalDevice::swapchainFormat(SDL_Window* window) const {
    const MetalWindow* w = findWindow(window);
    return w ? w->format : TextureFormat::Invalid;
}

// ------------------------------------------------------------------ fences and timings

bool MetalDevice::isSignaled(void* fence) const {
    const MTLCommandBufferStatus status = static_cast<MetalFence*>(fence)->handle.status;
    return status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError;
}

void MetalDevice::wait(void* fence) const {
    @autoreleasepool {
        [static_cast<MetalFence*>(fence)->handle waitUntilCompleted];
    }
}

std::optional<double> MetalDevice::gpuMilliseconds(void* fence) const {
    id<MTLCommandBuffer> handle = static_cast<MetalFence*>(fence)->handle;
    if (handle.status != MTLCommandBufferStatusCompleted) return std::nullopt;
    const CFTimeInterval start = handle.GPUStartTime;
    const CFTimeInterval end = handle.GPUEndTime;
    if (end <= start) return std::nullopt;
    return (end - start) * 1000.0;
}

std::vector<GpuTiming> MetalDevice::gpuTimings(void* fence) const {
    @autoreleasepool {
        const MetalFence& f = *static_cast<MetalFence*>(fence);
        if (!f.timing || f.handle.status != MTLCommandBufferStatusCompleted) return {};
        if (!f.timing->results) resolve(*f.timing);
        return *f.timing->results;
    }
}

void MetalDevice::resolve(MetalTiming& timing) const {
    timing.results.emplace();
    if (!timing.samples || timing.used == 0) return;
    NSData* data = [timing.samples resolveCounterRange:NSMakeRange(0, timing.used)];
    if (!data || data.length < timing.used * sizeof(MTLCounterResultTimestamp)) {
        log::warning("gpu", "timestamps of a command buffer could not be resolved");
        return;
    }
    const auto* samples = static_cast<const MTLCounterResultTimestamp*>(data.bytes);

    // The GPU timestamp's unit: the CPU and GPU clocks sampled about a second apart (nanoseconds on Apple GPUs).
    if (!m_timestampCalibrated && std::chrono::steady_clock::now() - m_calibrationCpu > std::chrono::seconds(1)) {
        MTLTimestamp cpu = 0, gpu = 0;
        [m_device sampleTimestamps:&cpu gpuTimestamp:&gpu];
        const double cpuNs =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - m_calibrationCpu).count();
        if (gpu > m_calibrationGpu) {
            m_timestampPeriodNs = cpuNs / double(gpu - m_calibrationGpu);
            m_timestampCalibrated = true;
            log::info("gpu", "GPU timestamp period {:.4f} ns", m_timestampPeriodNs);
        }
    }

    // Sample pairs: (start, end) of a compute or blit encoder, or of the vertex and the fragment stage of a render
    // encoder; per encoder its stages' intervals.
    const auto valid = [](MTLTimestamp t) { return t != 0 && t != MTLCounterErrorValue; };
    using Interval = std::pair<MTLTimestamp, MTLTimestamp>;
    std::vector<std::size_t> firstInterval(timing.encoders.size() + 1, 0);
    std::vector<Interval> intervals;
    MTLTimestamp base = ~MTLTimestamp(0);
    for (std::size_t e = 0; e < timing.encoders.size(); ++e) {
        firstInterval[e] = intervals.size();
        const MetalTiming::Encoder& encoder = timing.encoders[e];
        for (std::uint32_t s = 0; s + 1 < encoder.samples; s += 2) {
            const MTLTimestamp start = samples[encoder.firstSample + s].timestamp;
            const MTLTimestamp end = samples[encoder.firstSample + s + 1].timestamp;
            if (!valid(start) || !valid(end) || end < start) continue;
            intervals.emplace_back(start, end);
            base = std::min(base, start);
        }
    }
    firstInterval[timing.encoders.size()] = intervals.size();

    // In the order the ranges began; a range without a sampled pass is left out.
    const double msPerTick = m_timestampPeriodNs * 1e-6;
    std::vector<Interval> merged;
    for (const MetalTiming::Range& range : timing.ranges) {
        if (!range.closed || !range.complete) continue;
        merged.assign(intervals.begin() + std::ptrdiff_t(firstInterval[range.firstEncoder]),
                      intervals.begin() + std::ptrdiff_t(firstInterval[range.endEncoder]));
        if (merged.empty()) continue;
        std::sort(merged.begin(), merged.end());
        MTLTimestamp busy = 0, end = merged.front().first;
        for (const auto& [s, e] : merged) {
            if (e > end) busy += e - std::max(s, end);
            end = std::max(end, e);
        }
        timing.results->push_back({.name = range.name,
                                   .milliseconds = double(busy) * msPerTick,
                                   .startMilliseconds = double(merged.front().first - base) * msPerTick,
                                   .endMilliseconds = double(end - base) * msPerTick,
                                   .depth = range.depth});
    }
}

void MetalDevice::returnSampleBuffer(id<MTLCounterSampleBuffer> samples) {
    if (samples) m_samplePool.push_back(samples);
}

MetalTiming::~MetalTiming() {
    if (device) device->returnSampleBuffer(samples);
}

} // namespace quench::gpu::detail
