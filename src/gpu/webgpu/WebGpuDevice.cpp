#include "gpu/webgpu/WebGpu.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <emscripten.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace quench::gpu::detail {

namespace wg {

WGPUTextureFormat toWgpu(TextureFormat format) {
    switch (format) {
    case TextureFormat::R8Unorm: return WGPUTextureFormat_R8Unorm;
    case TextureFormat::Rg8Unorm: return WGPUTextureFormat_RG8Unorm;
    case TextureFormat::Rgba8Unorm: return WGPUTextureFormat_RGBA8Unorm;
    case TextureFormat::Rgba8UnormSrgb: return WGPUTextureFormat_RGBA8UnormSrgb;
    case TextureFormat::Bgra8Unorm: return WGPUTextureFormat_BGRA8Unorm;
    case TextureFormat::Bgra8UnormSrgb: return WGPUTextureFormat_BGRA8UnormSrgb;
    case TextureFormat::Rgb10a2Unorm: return WGPUTextureFormat_RGB10A2Unorm;
    case TextureFormat::R16Float: return WGPUTextureFormat_R16Float;
    case TextureFormat::Rg16Float: return WGPUTextureFormat_RG16Float;
    case TextureFormat::Rgba16Float: return WGPUTextureFormat_RGBA16Float;
    case TextureFormat::R32Float: return WGPUTextureFormat_R32Float;
    case TextureFormat::Rg32Float: return WGPUTextureFormat_RG32Float;
    case TextureFormat::Rgba32Float: return WGPUTextureFormat_RGBA32Float;
    case TextureFormat::Rg11b10Ufloat: return WGPUTextureFormat_RG11B10Ufloat;
    case TextureFormat::R32Uint: return WGPUTextureFormat_R32Uint;
    case TextureFormat::Bc1RgbaUnorm: return WGPUTextureFormat_BC1RGBAUnorm;
    case TextureFormat::Bc1RgbaUnormSrgb: return WGPUTextureFormat_BC1RGBAUnormSrgb;
    case TextureFormat::Bc3RgbaUnorm: return WGPUTextureFormat_BC3RGBAUnorm;
    case TextureFormat::Bc3RgbaUnormSrgb: return WGPUTextureFormat_BC3RGBAUnormSrgb;
    case TextureFormat::Bc4RUnorm: return WGPUTextureFormat_BC4RUnorm;
    case TextureFormat::Bc5RgUnorm: return WGPUTextureFormat_BC5RGUnorm;
    case TextureFormat::Bc6hRgbUfloat: return WGPUTextureFormat_BC6HRGBUfloat;
    case TextureFormat::Bc7RgbaUnorm: return WGPUTextureFormat_BC7RGBAUnorm;
    case TextureFormat::Bc7RgbaUnormSrgb: return WGPUTextureFormat_BC7RGBAUnormSrgb;
    case TextureFormat::Astc4x4Unorm: return WGPUTextureFormat_ASTC4x4Unorm;
    case TextureFormat::Astc4x4UnormSrgb: return WGPUTextureFormat_ASTC4x4UnormSrgb;
    case TextureFormat::Etc2Rgb8Unorm: return WGPUTextureFormat_ETC2RGB8Unorm;
    case TextureFormat::Etc2Rgb8UnormSrgb: return WGPUTextureFormat_ETC2RGB8UnormSrgb;
    case TextureFormat::Etc2Rgba8Unorm: return WGPUTextureFormat_ETC2RGBA8Unorm;
    case TextureFormat::Etc2Rgba8UnormSrgb: return WGPUTextureFormat_ETC2RGBA8UnormSrgb;
    case TextureFormat::Depth16Unorm: return WGPUTextureFormat_Depth16Unorm;
    case TextureFormat::Depth24UnormStencil8: return WGPUTextureFormat_Depth24PlusStencil8;
    case TextureFormat::Depth32Float: return WGPUTextureFormat_Depth32Float;
    case TextureFormat::Depth32FloatStencil8: return WGPUTextureFormat_Depth32FloatStencil8;
    case TextureFormat::Invalid:
    case TextureFormat::Count: break;
    }
    return WGPUTextureFormat_Undefined;
}

TextureFormat fromWgpu(WGPUTextureFormat format) {
    for (std::uint8_t i = 1; i < std::uint8_t(TextureFormat::Count); ++i)
        if (toWgpu(TextureFormat(i)) == format) return TextureFormat(i);
    return TextureFormat::Invalid;
}

} // namespace wg

namespace {

using wg::sv;

std::string text(WGPUStringView view) {
    if (!view.data) return {};
    return view.length == WGPU_STRLEN ? std::string(view.data) : std::string(view.data, view.length);
}

void sleepUntil(const bool& done) {
    while (!done) emscripten_sleep(1);
}

WGPUCompareFunction toWgpu(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return WGPUCompareFunction_Never;
    case CompareOp::Less: return WGPUCompareFunction_Less;
    case CompareOp::Equal: return WGPUCompareFunction_Equal;
    case CompareOp::LessOrEqual: return WGPUCompareFunction_LessEqual;
    case CompareOp::Greater: return WGPUCompareFunction_Greater;
    case CompareOp::NotEqual: return WGPUCompareFunction_NotEqual;
    case CompareOp::GreaterOrEqual: return WGPUCompareFunction_GreaterEqual;
    case CompareOp::Always: return WGPUCompareFunction_Always;
    }
    return WGPUCompareFunction_Always;
}

WGPUAddressMode toWgpu(AddressMode mode) {
    switch (mode) {
    case AddressMode::Repeat: return WGPUAddressMode_Repeat;
    case AddressMode::MirroredRepeat: return WGPUAddressMode_MirrorRepeat;
    case AddressMode::ClampToEdge: return WGPUAddressMode_ClampToEdge;
    }
    return WGPUAddressMode_Repeat;
}

WGPUVertexFormat toWgpu(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float: return WGPUVertexFormat_Float32;
    case VertexFormat::Float2: return WGPUVertexFormat_Float32x2;
    case VertexFormat::Float3: return WGPUVertexFormat_Float32x3;
    case VertexFormat::Float4: return WGPUVertexFormat_Float32x4;
    case VertexFormat::Half2: return WGPUVertexFormat_Float16x2;
    case VertexFormat::Half4: return WGPUVertexFormat_Float16x4;
    case VertexFormat::Uint: return WGPUVertexFormat_Uint32;
    case VertexFormat::Uint2: return WGPUVertexFormat_Uint32x2;
    case VertexFormat::Uint4: return WGPUVertexFormat_Uint32x4;
    case VertexFormat::Ubyte4: return WGPUVertexFormat_Uint8x4;
    case VertexFormat::Ubyte4Norm: return WGPUVertexFormat_Unorm8x4;
    case VertexFormat::Short2Norm: return WGPUVertexFormat_Snorm16x2;
    case VertexFormat::Short4Norm: return WGPUVertexFormat_Snorm16x4;
    case VertexFormat::Ushort4: return WGPUVertexFormat_Uint16x4;
    case VertexFormat::Ushort4Norm: return WGPUVertexFormat_Unorm16x4;
    }
    return WGPUVertexFormat_Float32x3;
}

WGPUBlendFactor toWgpu(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero: return WGPUBlendFactor_Zero;
    case BlendFactor::One: return WGPUBlendFactor_One;
    case BlendFactor::SrcColor: return WGPUBlendFactor_Src;
    case BlendFactor::OneMinusSrcColor: return WGPUBlendFactor_OneMinusSrc;
    case BlendFactor::DstColor: return WGPUBlendFactor_Dst;
    case BlendFactor::OneMinusDstColor: return WGPUBlendFactor_OneMinusDst;
    case BlendFactor::SrcAlpha: return WGPUBlendFactor_SrcAlpha;
    case BlendFactor::OneMinusSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
    case BlendFactor::DstAlpha: return WGPUBlendFactor_DstAlpha;
    case BlendFactor::OneMinusDstAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
    case BlendFactor::ConstantColor: return WGPUBlendFactor_Constant;
    case BlendFactor::OneMinusConstantColor: return WGPUBlendFactor_OneMinusConstant;
    case BlendFactor::SrcAlphaSaturate: return WGPUBlendFactor_SrcAlphaSaturated;
    }
    return WGPUBlendFactor_One;
}

WGPUBlendComponent blendComponent(BlendOp op, BlendFactor source, BlendFactor destination) {
    WGPUBlendComponent c = WGPU_BLEND_COMPONENT_INIT;
    switch (op) {
    case BlendOp::Add: c.operation = WGPUBlendOperation_Add; break;
    case BlendOp::Subtract: c.operation = WGPUBlendOperation_Subtract; break;
    case BlendOp::ReverseSubtract: c.operation = WGPUBlendOperation_ReverseSubtract; break;
    case BlendOp::Min: c.operation = WGPUBlendOperation_Min; break;
    case BlendOp::Max: c.operation = WGPUBlendOperation_Max; break;
    }
    // WebGPU takes min and max with factors of one only (they are ignored elsewhere).
    const bool minMax = op == BlendOp::Min || op == BlendOp::Max;
    c.srcFactor = minMax ? WGPUBlendFactor_One : toWgpu(source);
    c.dstFactor = minMax ? WGPUBlendFactor_One : toWgpu(destination);
    return c;
}

bool compressed(TextureFormat format) { return formatInfo(format).blockWidth > 1; }

// The feature a format needs; CoreFeaturesAndLimits for none.
WGPUFeatureName formatFeature(TextureFormat format) {
    if (format >= TextureFormat::Bc1RgbaUnorm && format <= TextureFormat::Bc7RgbaUnormSrgb)
        return WGPUFeatureName_TextureCompressionBC;
    if (format == TextureFormat::Astc4x4Unorm || format == TextureFormat::Astc4x4UnormSrgb)
        return WGPUFeatureName_TextureCompressionASTC;
    if (format >= TextureFormat::Etc2Rgb8Unorm && format <= TextureFormat::Etc2Rgba8UnormSrgb)
        return WGPUFeatureName_TextureCompressionETC2;
    if (format == TextureFormat::Depth32FloatStencil8) return WGPUFeatureName_Depth32FloatStencil8;
    return WGPUFeatureName_CoreFeaturesAndLimits;
}

// ------------------------------------------------------------------ WGSL declarations

std::uint32_t numberAfter(std::string_view line, std::string_view key) {
    const std::size_t at = line.find(key);
    if (at == std::string_view::npos) return ~0u;
    std::uint32_t value = 0;
    const char* first = line.data() + at + key.size();
    const auto [end, error] = std::from_chars(first, line.data() + line.size(), value);
    return error == std::errc() && end != first ? value : ~0u;
}

// The resource variables of a WGSL module as Tint writes them, one per line:
// `@group(2u) @binding(0u) var name : texture_2d<f32>;`, `var<uniform>`, `var<storage, read>`.
Result<std::vector<WgBinding>> parseBindings(std::string_view code) {
    std::vector<WgBinding> bindings;
    for (std::size_t at = 0; at < code.size();) {
        std::size_t end = code.find('\n', at);
        if (end == std::string_view::npos) end = code.size();
        const std::string_view line = code.substr(at, end - at);
        at = end + 1;
        if (!line.starts_with("@group(")) continue;
        WgBinding b;
        b.group = numberAfter(line, "@group(");
        b.binding = numberAfter(line, "@binding(");
        const std::size_t colon = line.rfind(" : ") != std::string_view::npos  ? line.rfind(" : ") + 3
                                  : line.rfind(": ") != std::string_view::npos ? line.rfind(": ") + 2
                                                                               : std::string_view::npos;
        if (b.group == ~0u || b.binding == ~0u || colon == std::string_view::npos)
            return Error{std::format("cannot read the WGSL declaration '{}'", line)};
        const std::string_view type = line.substr(colon);
        if (line.find("var<uniform>") != std::string_view::npos) {
            b.kind = WgBindingKind::Uniform;
        } else if (line.find("var<storage, read_write>") != std::string_view::npos) {
            b.kind = WgBindingKind::StorageWrite;
        } else if (line.find("var<storage") != std::string_view::npos) {
            b.kind = WgBindingKind::StorageRead;
        } else if (type.starts_with("sampler")) {
            b.kind = WgBindingKind::Sampler;
            b.comparison = type.starts_with("sampler_comparison");
        } else if (type.starts_with("texture_") && !type.starts_with("texture_storage_")) {
            b.kind = WgBindingKind::Texture;
            std::string_view shape = type.substr(8);
            if (shape.starts_with("depth_")) {
                b.sampleType = WGPUTextureSampleType_Depth;
                shape.remove_prefix(6);
            }
            if (shape.starts_with("multisampled_")) {
                b.multisampled = true;
                shape.remove_prefix(13);
            }
            if (shape.starts_with("2d_array"))
                b.dimension = WGPUTextureViewDimension_2DArray;
            else if (shape.starts_with("2d"))
                b.dimension = WGPUTextureViewDimension_2D;
            else if (shape.starts_with("cube_array"))
                b.dimension = WGPUTextureViewDimension_CubeArray;
            else if (shape.starts_with("cube"))
                b.dimension = WGPUTextureViewDimension_Cube;
            else if (shape.starts_with("3d"))
                b.dimension = WGPUTextureViewDimension_3D;
            else
                return Error{std::format("unsupported WGSL texture type in '{}'", line)};
            if (shape.find("<i32>") != std::string_view::npos) b.sampleType = WGPUTextureSampleType_Sint;
            if (shape.find("<u32>") != std::string_view::npos) b.sampleType = WGPUTextureSampleType_Uint;
        } else {
            return Error{std::format("unsupported WGSL resource in '{}'", line)};
        }
        bindings.push_back(b);
    }
    return bindings;
}

} // namespace

// ------------------------------------------------------------------ the device

Result<std::unique_ptr<DeviceImpl>> createWebGpuDevice(const DeviceDesc& desc) {
    auto impl = std::make_unique<WebGpuDevice>();
    if (Result<void> started = impl->start(desc); !started) return Error{started.error()};
    return std::unique_ptr<DeviceImpl>(std::move(impl));
}

Result<void> WebGpuDevice::start(const DeviceDesc& desc) {
    // JavaScript inside C++ for clang-format: no === or !==, which it would split.
    if (!EM_ASM_INT({ return typeof navigator != 'undefined' && navigator.gpu ? 1 : 0; }))
        return Error{"this browser has no WebGPU (navigator.gpu)"};
    m_instance = wgpuCreateInstance(nullptr);
    if (!m_instance) return Error{"cannot create a WebGPU instance"};

    struct Request {
        bool done = false;
        void* object = nullptr;
        std::string message;
    };
    Request adapter;
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.powerPreference = desc.preferLowPower ? WGPUPowerPreference_LowPower : WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo adapterInfo = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapterInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    adapterInfo.callback = [](WGPURequestAdapterStatus, WGPUAdapter result, WGPUStringView message, void* user, void*) {
        Request& r = *static_cast<Request*>(user);
        r.object = result;
        r.message = text(message);
        r.done = true;
    };
    adapterInfo.userdata1 = &adapter;
    wgpuInstanceRequestAdapter(m_instance, &options, adapterInfo);
    sleepUntil(adapter.done);
    if (!adapter.object) return Error{"no WebGPU adapter: " + adapter.message};
    m_adapter = static_cast<WGPUAdapter>(adapter.object);

    WGPUSupportedFeatures supported = WGPU_SUPPORTED_FEATURES_INIT;
    wgpuAdapterGetFeatures(m_adapter, &supported);
    for (const WGPUFeatureName wanted :
         {WGPUFeatureName_DepthClipControl, WGPUFeatureName_Depth32FloatStencil8, WGPUFeatureName_TextureCompressionBC,
          WGPUFeatureName_TextureCompressionETC2, WGPUFeatureName_TextureCompressionASTC,
          WGPUFeatureName_IndirectFirstInstance, WGPUFeatureName_RG11B10UfloatRenderable,
          WGPUFeatureName_Float32Filterable, WGPUFeatureName_Float32Blendable})
        for (std::size_t i = 0; i < supported.featureCount; ++i)
            if (supported.features[i] == wanted) m_features.push_back(wanted);
    wgpuSupportedFeaturesFreeMembers(supported);
    WGPULimits limits = WGPU_LIMITS_INIT;
    const bool haveLimits = wgpuAdapterGetLimits(m_adapter, &limits) == WGPUStatus_Success;

    Request device;
    WGPUDeviceDescriptor deviceDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    deviceDesc.label = sv("quench");
    deviceDesc.requiredFeatureCount = m_features.size();
    deviceDesc.requiredFeatures = m_features.data();
    if (haveLimits) deviceDesc.requiredLimits = &limits; // everything the adapter has
    deviceDesc.uncapturedErrorCallbackInfo.callback = [](const WGPUDevice*, WGPUErrorType, WGPUStringView message,
                                                         void*, void*) {
        static std::uint32_t count = 0;
        if (++count <= 40)
            log::error("gpu", "WebGPU: {}", text(message));
        else if (count == 41)
            log::error("gpu", "WebGPU: more errors, not logged");
    };
    deviceDesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    deviceDesc.deviceLostCallbackInfo.callback = [](const WGPUDevice*, WGPUDeviceLostReason reason,
                                                    WGPUStringView message, void*, void*) {
        if (reason != WGPUDeviceLostReason_Destroyed) log::error("gpu", "WebGPU device lost: {}", text(message));
    };
    WGPURequestDeviceCallbackInfo deviceInfo = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    deviceInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    deviceInfo.callback = [](WGPURequestDeviceStatus, WGPUDevice result, WGPUStringView message, void* user, void*) {
        Request& r = *static_cast<Request*>(user);
        r.object = result;
        r.message = text(message);
        r.done = true;
    };
    deviceInfo.userdata1 = &device;
    wgpuAdapterRequestDevice(m_adapter, &deviceDesc, deviceInfo);
    sleepUntil(device.done);
    if (!device.object) return Error{"no WebGPU device: " + device.message};
    m_device = static_cast<WGPUDevice>(device.object);
    m_queue = wgpuDeviceGetQueue(m_device);
    m_limits = WGPU_LIMITS_INIT;
    wgpuDeviceGetLimits(m_device, &m_limits);

    caps.backend = Backend::WebGpu;
    caps.driver = "webgpu";
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(m_adapter, &info) == WGPUStatus_Success) {
        caps.deviceName = text(info.description);
        if (caps.deviceName.empty()) {
            caps.deviceName = text(info.vendor);
            for (const std::string& part : {text(info.architecture), text(info.device)})
                if (!part.empty()) caps.deviceName += (caps.deviceName.empty() ? "" : " ") + part;
        }
        wgpuAdapterInfoFreeMembers(info);
    }
    caps.shaderFormat = ShaderFormat::Wgsl;
    caps.gpuTiming = false;
    caps.gpuPassTiming = false;
    caps.depthClamp = hasFeature(WGPUFeatureName_DepthClipControl);
    caps.depthBiasClamp = true;
    caps.indirectDraw = true;
    caps.computeShaders = true;

    WGPUSamplerDescriptor sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampler.minFilter = sampler.magFilter = WGPUFilterMode_Linear;
    m_defaultSampler = wgpuDeviceCreateSampler(m_device, &sampler);
    m_blitSamplers[1] = wgpuDeviceCreateSampler(m_device, &sampler);
    sampler.minFilter = sampler.magFilter = WGPUFilterMode_Nearest;
    m_blitSamplers[0] = wgpuDeviceCreateSampler(m_device, &sampler);
    WGPUBufferDescriptor storage = WGPU_BUFFER_DESCRIPTOR_INIT;
    storage.usage = WGPUBufferUsage_Storage;
    storage.size = 65536;
    m_dummyStorage = wgpuDeviceCreateBuffer(m_device, &storage);
    return {};
}

WebGpuDevice::~WebGpuDevice() {
    for (WGPUTexture texture : m_deadTextures) wgpuTextureRelease(texture);
    for (WGPUBuffer buffer : m_deadBuffers) wgpuBufferRelease(buffer);
    for (auto& [key, entry] : m_bindGroups) wgpuBindGroupRelease(entry.group);
    for (auto& [key, layout] : m_layouts) wgpuBindGroupLayoutRelease(layout);
    for (auto& [key, pipeline] : m_blitPipelines) wgpuRenderPipelineRelease(pipeline);
    for (WGPUBindGroupLayout layout : m_blitLayouts)
        if (layout) wgpuBindGroupLayoutRelease(layout);
    if (m_blitModule) wgpuShaderModuleRelease(m_blitModule);
    for (WGPUSampler sampler : m_blitSamplers)
        if (sampler) wgpuSamplerRelease(sampler);
    for (WGPUBuffer page : m_freePages) wgpuBufferRelease(page);
    for (auto& dummy : m_dummyTextures)
        if (dummy) {
            for (WgTexture::View& v : dummy->views) wgpuTextureViewRelease(v.view);
            wgpuTextureRelease(dummy->texture);
        }
    if (m_defaultSampler) wgpuSamplerRelease(m_defaultSampler);
    if (m_dummyStorage) wgpuBufferRelease(m_dummyStorage);
    while (!m_windows.empty()) releaseWindow(m_windows.back()->window);
    if (m_queue) wgpuQueueRelease(m_queue);
    if (m_device) wgpuDeviceRelease(m_device);
    if (m_adapter) wgpuAdapterRelease(m_adapter);
    if (m_instance) wgpuInstanceRelease(m_instance);
}

bool WebGpuDevice::hasFeature(WGPUFeatureName feature) const {
    return feature == WGPUFeatureName_CoreFeaturesAndLimits ||
           std::find(m_features.begin(), m_features.end(), feature) != m_features.end();
}

bool WebGpuDevice::supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const {
    if (wg::toWgpu(format) == WGPUTextureFormat_Undefined || !hasFeature(formatFeature(format))) return false;
    const FormatInfo& info = formatInfo(format);
    // Read-only storage textures in render passes and textures written outside them: not on this backend.
    if (any(usage & (TextureUsage::GraphicsStorageRead | TextureUsage::ShaderWrite))) return false;
    if (any(usage & TextureUsage::DepthStencilTarget) && !info.depth) return false;
    if (any(usage & TextureUsage::ColorTarget)) {
        if (info.depth || compressed(format)) return false;
        if (format == TextureFormat::Rg11b10Ufloat && !hasFeature(WGPUFeatureName_RG11B10UfloatRenderable))
            return false;
    }
    if (type == TextureType::Texture3D && (info.depth || compressed(format))) return false;
    return true;
}

bool WebGpuDevice::supportsSampleCount(TextureFormat format, std::uint32_t sampleCount) const {
    if (sampleCount == 1) return true;
    if (sampleCount != 4 || compressed(format)) return false;
    // Formats WebGPU cannot multisample or resolve.
    return format != TextureFormat::R32Float && format != TextureFormat::Rg32Float &&
           format != TextureFormat::Rgba32Float && format != TextureFormat::R32Uint;
}

// WebGPU keeps the resources of submitted work alive and orders its own commands: nothing to wait for (and a wait
// would yield to the browser in the middle of a frame).
void WebGpuDevice::waitIdle() {}

// ------------------------------------------------------------------ resources

Result<void*> WebGpuDevice::createTexture(const TextureDesc& desc) {
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.label = sv(desc.name);
    d.format = wg::toWgpu(desc.format);
    d.dimension = desc.type == TextureType::Texture3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    d.size = {desc.width, desc.height, desc.type == TextureType::Texture2D ? 1u : desc.layers};
    d.mipLevelCount = desc.mipLevels;
    d.sampleCount = desc.sampleCount;
    if (any(desc.usage & TextureUsage::Sampled)) d.usage |= WGPUTextureUsage_TextureBinding;
    if (any(desc.usage & (TextureUsage::ColorTarget | TextureUsage::DepthStencilTarget)))
        d.usage |= WGPUTextureUsage_RenderAttachment;
    if (any(desc.usage & TextureUsage::GraphicsStorageRead)) d.usage |= WGPUTextureUsage_StorageBinding;
    if (desc.sampleCount == 1) d.usage |= WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    WGPUTexture native = wgpuDeviceCreateTexture(m_device, &d);
    if (!native)
        return Error{std::format("cannot create texture '{}' {}x{} {}", desc.name, desc.width, desc.height,
                                 formatInfo(desc.format).name)};
    auto* texture = new WgTexture;
    texture->texture = native;
    texture->desc = desc;
    texture->desc.name = {};
    texture->format = d.format;
    texture->id = m_nextId++;
    const bool float32 = desc.format == TextureFormat::R32Float || desc.format == TextureFormat::Rg32Float ||
                         desc.format == TextureFormat::Rgba32Float;
    texture->filterable = !formatInfo(desc.format).depth && desc.format != TextureFormat::R32Uint &&
                          (!float32 || hasFeature(WGPUFeatureName_Float32Filterable));
    return static_cast<void*>(texture);
}

Result<void*> WebGpuDevice::createBuffer(const BufferDesc& desc) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.label = sv(desc.name);
    d.size = std::max((desc.size + 3u) & ~3u, 4u); // copies take multiples of 4 bytes
    d.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    if (any(desc.usage & BufferUsage::Vertex)) d.usage |= WGPUBufferUsage_Vertex;
    if (any(desc.usage & BufferUsage::Index)) d.usage |= WGPUBufferUsage_Index;
    if (any(desc.usage & BufferUsage::Indirect)) d.usage |= WGPUBufferUsage_Indirect;
    if (any(desc.usage &
            (BufferUsage::GraphicsStorageRead | BufferUsage::ComputeStorageRead | BufferUsage::ComputeStorageWrite)))
        d.usage |= WGPUBufferUsage_Storage;
    WGPUBuffer native = wgpuDeviceCreateBuffer(m_device, &d);
    if (!native) return Error{std::format("cannot create buffer '{}' of {} bytes", desc.name, desc.size)};
    return static_cast<void*>(new WgBuffer{native, m_nextId++, std::uint32_t(d.size)});
}

Result<void*> WebGpuDevice::createSampler(const SamplerDesc& desc) {
    WGPUSamplerDescriptor d = WGPU_SAMPLER_DESCRIPTOR_INIT;
    d.label = sv(desc.name);
    d.addressModeU = toWgpu(desc.addressU);
    d.addressModeV = toWgpu(desc.addressV);
    d.addressModeW = toWgpu(desc.addressW);
    d.minFilter = desc.minFilter == Filter::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    d.magFilter = desc.magFilter == Filter::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    d.mipmapFilter = desc.mipmapMode == MipmapMode::Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    d.lodMinClamp = std::max(desc.minLod, 0.0f);
    d.lodMaxClamp = std::max(desc.maxLod, d.lodMinClamp);
    // WebGPU has no LOD bias, and anisotropy needs linear filters throughout.
    const bool linear = d.minFilter == WGPUFilterMode_Linear && d.magFilter == WGPUFilterMode_Linear &&
                        d.mipmapFilter == WGPUMipmapFilterMode_Linear;
    d.maxAnisotropy = linear ? std::uint16_t(std::clamp(desc.maxAnisotropy, 1.0f, 16.0f)) : std::uint16_t(1);
    if (desc.compare) d.compare = toWgpu(desc.compareOp);
    auto* sampler = new WgSampler;
    sampler->sampler = wgpuDeviceCreateSampler(m_device, &d);
    sampler->id = m_nextId++;
    sampler->comparison = desc.compare;
    d.minFilter = d.magFilter = WGPUFilterMode_Nearest;
    d.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    d.maxAnisotropy = 1;
    sampler->nearest = wgpuDeviceCreateSampler(m_device, &d);
    if (!sampler->sampler || !sampler->nearest) {
        release(Kind::Sampler, sampler);
        return Error{std::format("cannot create sampler '{}'", desc.name)};
    }
    return static_cast<void*>(sampler);
}

Result<void*> WebGpuDevice::createTransferBuffer(const TransferBufferDesc& desc) {
    auto* transfer = new WgTransfer;
    transfer->download = desc.direction == TransferDirection::Download;
    transfer->data.resize(desc.size);
    return static_cast<void*>(transfer);
}

void WebGpuDevice::release(Kind kind, void* native) noexcept {
    switch (kind) {
    // An open command buffer may have recorded uses of a buffer or texture: those are destroyed after it.
    case Kind::Buffer: {
        auto* buffer = static_cast<WgBuffer*>(native);
        if (m_openCommands)
            m_deadBuffers.push_back(buffer->buffer);
        else
            wgpuBufferRelease(buffer->buffer); // the port destroys it with its last reference
        delete buffer;
        break;
    }
    case Kind::Texture: {
        auto* texture = static_cast<WgTexture*>(native);
        for (WgTexture::View& v : texture->views) wgpuTextureViewRelease(v.view);
        if (m_openCommands) {
            m_deadTextures.push_back(texture->texture);
        } else {
            wgpuTextureDestroy(texture->texture);
            wgpuTextureRelease(texture->texture);
        }
        delete texture;
        break;
    }
    case Kind::Sampler: {
        auto* sampler = static_cast<WgSampler*>(native);
        if (sampler->sampler) wgpuSamplerRelease(sampler->sampler);
        if (sampler->nearest) wgpuSamplerRelease(sampler->nearest);
        delete sampler;
        break;
    }
    case Kind::TransferBuffer: {
        delete static_cast<WgTransfer*>(native); // a read still in flight belongs to its map callback
        break;
    }
    case Kind::GraphicsPipeline:
    case Kind::ComputePipeline: {
        auto* pipeline = static_cast<WgPipeline*>(native);
        for (WgStage& stage : pipeline->stages)
            if (stage.module) wgpuShaderModuleRelease(stage.module);
        for (WgPipeline::Variant& v : pipeline->variants)
            if (v.pipeline) wgpuRenderPipelineRelease(v.pipeline);
        if (pipeline->computePipeline) wgpuComputePipelineRelease(pipeline->computePipeline);
        delete pipeline; // the layouts belong to the device's cache
        break;
    }
    case Kind::Fence: delete static_cast<WgFence*>(native); break;
    case Kind::AccelerationStructure:
    case Kind::Upscaler: QUENCH_ASSERT(false, "WebGPU creates no acceleration structures or upscalers"); break;
    }
}

std::span<std::byte> WebGpuDevice::map(void* transferBuffer, std::uint32_t size, bool) {
    auto& transfer = *static_cast<WgTransfer*>(transferBuffer);
    // A download: its staging buffers were mapped at their submission. After the submission's fence they have been
    // read; before it this waits, which hands the frame back to the browser.
    for (const std::shared_ptr<WgRead>& read : transfer.reads) {
        if (!read->submitted) {
            if (!read->done) log::error("gpu", "a download was mapped before its command buffer was submitted");
            continue;
        }
        sleepUntil(read->done);
        if (read->ok && std::uint64_t(read->offset) + read->bytes.size() <= transfer.data.size())
            std::memcpy(transfer.data.data() + read->offset, read->bytes.data(), read->bytes.size());
    }
    transfer.reads.clear();
    return {transfer.data.data(), std::min<std::size_t>(size, transfer.data.size())};
}

void WebGpuDevice::unmap(void*) {}

// ------------------------------------------------------------------ texture views

WGPUTextureView WebGpuDevice::view(WgTexture& texture, WGPUTextureViewDimension dimension, std::uint32_t mip,
                                   std::uint32_t mipCount, std::uint32_t layer, std::uint32_t layerCount,
                                   bool sampled) {
    const std::uint64_t key = std::uint64_t(dimension) | std::uint64_t(mip) << 4 | std::uint64_t(mipCount) << 9 |
                              std::uint64_t(layer) << 14 | std::uint64_t(layerCount) << 30 |
                              std::uint64_t(sampled) << 46;
    for (const WgTexture::View& v : texture.views)
        if (v.key == key) return v.view;
    WGPUTextureViewDescriptor d = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    d.dimension = dimension;
    d.baseMipLevel = mip;
    d.mipLevelCount = mipCount ? mipCount : WGPU_MIP_LEVEL_COUNT_UNDEFINED;
    d.baseArrayLayer = layer;
    d.arrayLayerCount = layerCount ? layerCount : WGPU_ARRAY_LAYER_COUNT_UNDEFINED;
    // A shader reads the depth of a depth and stencil texture.
    d.aspect = sampled && formatInfo(texture.desc.format).depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
    WGPUTextureView created = wgpuTextureCreateView(texture.texture, &d);
    texture.views.push_back({key, created});
    return created;
}

WGPUTextureView WebGpuDevice::sampledView(WgTexture& texture, WGPUTextureViewDimension dimension) {
    switch (dimension) {
    case WGPUTextureViewDimension_2D: return view(texture, dimension, 0, 0, 0, 1, true);
    case WGPUTextureViewDimension_Cube: return view(texture, dimension, 0, 0, 0, 6, true);
    case WGPUTextureViewDimension_3D: return view(texture, dimension, 0, 0, 0, 1, true);
    default: return view(texture, dimension, 0, 0, 0, 0, true); // arrays: every layer
    }
}

WGPUTextureView WebGpuDevice::targetView(const TextureRef& ref, std::uint32_t mip, std::uint32_t layer,
                                         std::uint32_t* depthSlice) {
    WgTexture& texture = *wg::texture(ref);
    if (texture.desc.type == TextureType::Texture3D) {
        if (depthSlice) *depthSlice = layer;
        return view(texture, WGPUTextureViewDimension_3D, mip, 1, 0, 1, false);
    }
    return view(texture, WGPUTextureViewDimension_2D, mip, 1, layer, 1, false);
}

WgTexture* WebGpuDevice::dummyTexture(WGPUTextureViewDimension dimension) {
    const bool cube = dimension == WGPUTextureViewDimension_Cube || dimension == WGPUTextureViewDimension_CubeArray;
    const std::size_t index = dimension == WGPUTextureViewDimension_3D ? 2 : cube ? 1 : 0;
    if (!m_dummyTextures[index]) {
        TextureDesc desc;
        desc.type = index == 2 ? TextureType::Texture3D : index == 1 ? TextureType::Cube : TextureType::Texture2DArray;
        desc.layers = index == 1 ? 6 : 1;
        desc.name = "quench missing texture";
        Result<void*> created = createTexture(desc);
        if (!created) return nullptr;
        m_dummyTextures[index].reset(static_cast<WgTexture*>(*created));
    }
    return m_dummyTextures[index].get();
}

// ------------------------------------------------------------------ pipelines

Result<void> WebGpuDevice::createStage(const ShaderBlob& blob, WgStage& stage) {
    const ShaderCode code = shaderCodeFor(blob, ShaderFormat::Wgsl);
    if (!code) return Error{std::format("shader '{}' has no WGSL code in this build", blob.entry)};
    const std::string_view source(reinterpret_cast<const char*>(code.data), code.size);
    Result<std::vector<WgBinding>> bindings = parseBindings(source);
    if (!bindings) return Error{std::format("shader '{}': {}", blob.entry, bindings.error())};
    const std::uint32_t resourceGroup = blob.stage == ShaderStage::Fragment ? 2 : 0;
    const std::uint32_t uniformGroup = blob.stage == ShaderStage::Vertex     ? 1
                                       : blob.stage == ShaderStage::Fragment ? 3
                                                                             : 2;
    for (const WgBinding& b : *bindings) {
        if (b.kind == WgBindingKind::Uniform && b.group == uniformGroup && b.binding < kWgUniformSlots)
            stage.uniforms.push_back(b.binding);
        else if (b.kind == WgBindingKind::StorageWrite && blob.stage == ShaderStage::Compute && b.group == 1)
            stage.writes.push_back(b);
        else if (b.group == resourceGroup && b.kind != WgBindingKind::Uniform && b.kind != WgBindingKind::StorageWrite)
            stage.resources.push_back(b);
        else
            return Error{std::format("shader '{}': a resource at group {} binding {} is not where this backend "
                                     "expects it",
                                     blob.entry, b.group, b.binding)};
    }
    const auto byBinding = [](const WgBinding& a, const WgBinding& b) { return a.binding < b.binding; };
    std::sort(stage.resources.begin(), stage.resources.end(), byBinding);
    std::sort(stage.writes.begin(), stage.writes.end(), byBinding);
    std::sort(stage.uniforms.begin(), stage.uniforms.end());
    for (const WgBinding& b : stage.resources)
        if (b.kind == WgBindingKind::StorageRead) stage.storageBindings.push_back(b.binding);

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = sv(source);
    WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    d.nextInChain = &wgsl.chain;
    d.label = sv(blob.entry);
    stage.module = wgpuDeviceCreateShaderModule(m_device, &d);
    if (!stage.module) return Error{std::format("cannot create shader '{}'", blob.entry)};
    stage.entry = blob.entry;
    return {};
}

namespace {

// Layouts are shared by description: pipelines with the same bindings get the same object, and so can share bind
// groups.
std::string layoutKey(std::span<const WGPUBindGroupLayoutEntry> entries) {
    std::string key;
    for (const WGPUBindGroupLayoutEntry& e : entries)
        key += std::format("{}:{}:{}{}:{}:{}{}{};", e.binding, std::uint32_t(e.visibility),
                           std::uint32_t(e.buffer.type), e.buffer.hasDynamicOffset ? 'd' : '-',
                           std::uint32_t(e.sampler.type), std::uint32_t(e.texture.sampleType),
                           std::uint32_t(e.texture.viewDimension), e.texture.multisampled ? 'm' : '-');
    return key;
}

} // namespace

WGPUBindGroupLayout WebGpuDevice::resourceLayout(const WgStage& stage, WGPUShaderStage visibility,
                                                 std::uint32_t unfilterable) {
    std::vector<WGPUBindGroupLayoutEntry> entries;
    for (const WgBinding& b : stage.resources) {
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = b.binding;
        e.visibility = visibility;
        const bool nearest = (unfilterable >> (b.binding / 2)) & 1u;
        if (b.kind == WgBindingKind::Texture) {
            e.texture.sampleType = b.sampleType == WGPUTextureSampleType_Float && nearest
                                       ? WGPUTextureSampleType_UnfilterableFloat
                                       : b.sampleType;
            e.texture.viewDimension = b.dimension;
            e.texture.multisampled = b.multisampled;
        } else if (b.kind == WgBindingKind::Sampler) {
            e.sampler.type = b.comparison ? WGPUSamplerBindingType_Comparison
                             : nearest    ? WGPUSamplerBindingType_NonFiltering
                                          : WGPUSamplerBindingType_Filtering;
        } else {
            e.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        }
        entries.push_back(e);
    }
    const std::string key = layoutKey(entries);
    if (const auto it = m_layouts.find(key); it != m_layouts.end()) return it->second;
    WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    d.entryCount = entries.size();
    d.entries = entries.data();
    return m_layouts[key] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
}

WGPUBindGroupLayout WebGpuDevice::uniformLayout(const WgStage& stage, WGPUShaderStage visibility) {
    std::vector<WGPUBindGroupLayoutEntry> entries;
    for (const std::uint32_t slot : stage.uniforms) {
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = slot;
        e.visibility = visibility;
        e.buffer.type = WGPUBufferBindingType_Uniform;
        e.buffer.hasDynamicOffset = true;
        entries.push_back(e);
    }
    const std::string key = layoutKey(entries);
    if (const auto it = m_layouts.find(key); it != m_layouts.end()) return it->second;
    WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    d.entryCount = entries.size();
    d.entries = entries.data();
    return m_layouts[key] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
}

Result<void*> WebGpuDevice::createPipeline(const GraphicsPipelineDesc& desc) {
    auto pipeline = std::make_unique<WgPipeline>();
    WgPipeline& p = *pipeline;
    p.id = m_nextId++;
    p.name = desc.name.empty() ? std::format("{}/{}", desc.vertexShader->entry, desc.fragmentShader->entry)
                               : std::string(desc.name);
    const auto fail = [&](const std::string& error) -> Result<void*> {
        release(Kind::GraphicsPipeline, pipeline.release());
        return Error{error};
    };
    if (Result<void> stage = createStage(*desc.vertexShader, p.stages[0]); !stage) return fail(stage.error());
    if (Result<void> stage = createStage(*desc.fragmentShader, p.stages[1]); !stage) return fail(stage.error());
    p.uniformLayouts[0] = uniformLayout(p.stages[0], WGPUShaderStage_Vertex);
    p.uniformLayouts[1] = uniformLayout(p.stages[1], WGPUShaderStage_Fragment);

    std::uint32_t slots = 0;
    for (const VertexBufferLayout& b : desc.vertexBuffers) slots = std::max(slots, b.slot + 1);
    p.vertexBuffers.assign(slots, WGPU_VERTEX_BUFFER_LAYOUT_INIT);
    p.vertexAttributes.resize(slots);
    for (WGPUVertexBufferLayout& b : p.vertexBuffers) b.stepMode = WGPUVertexStepMode_Undefined; // a slot not used
    for (const VertexAttribute& a : desc.vertexAttributes) {
        if (a.slot >= slots) return fail(std::format("pipeline '{}': an attribute of a missing vertex buffer", p.name));
        WGPUVertexAttribute attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
        attribute.format = toWgpu(a.format);
        attribute.offset = a.offset;
        attribute.shaderLocation = a.location;
        p.vertexAttributes[a.slot].push_back(attribute);
    }
    for (const VertexBufferLayout& b : desc.vertexBuffers) {
        WGPUVertexBufferLayout& out = p.vertexBuffers[b.slot];
        out.stepMode = b.rate == VertexInputRate::PerInstance ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
        out.arrayStride = b.stride;
        out.attributeCount = p.vertexAttributes[b.slot].size();
        out.attributes = p.vertexAttributes[b.slot].data();
    }

    p.blends.resize(desc.colorTargets.size());
    for (std::size_t i = 0; i < desc.colorTargets.size(); ++i) {
        const ColorTargetDesc& in = desc.colorTargets[i];
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = wg::toWgpu(in.format);
        if (in.blend.enabled) {
            p.blends[i].color = blendComponent(in.blend.colorOp, in.blend.srcColor, in.blend.dstColor);
            p.blends[i].alpha = blendComponent(in.blend.alphaOp, in.blend.srcAlpha, in.blend.dstAlpha);
            target.blend = &p.blends[i];
        }
        target.writeMask = WGPUColorWriteMask_None;
        if (any(in.blend.writeMask & ColorWrite::R)) target.writeMask |= WGPUColorWriteMask_Red;
        if (any(in.blend.writeMask & ColorWrite::G)) target.writeMask |= WGPUColorWriteMask_Green;
        if (any(in.blend.writeMask & ColorWrite::B)) target.writeMask |= WGPUColorWriteMask_Blue;
        if (any(in.blend.writeMask & ColorWrite::A)) target.writeMask |= WGPUColorWriteMask_Alpha;
        p.colorTargets.push_back(target);
    }

    p.primitive = WGPU_PRIMITIVE_STATE_INIT;
    switch (desc.primitive) {
    case PrimitiveType::TriangleList: p.primitive.topology = WGPUPrimitiveTopology_TriangleList; break;
    case PrimitiveType::TriangleStrip: p.primitive.topology = WGPUPrimitiveTopology_TriangleStrip; break;
    case PrimitiveType::LineList: p.primitive.topology = WGPUPrimitiveTopology_LineList; break;
    case PrimitiveType::LineStrip: p.primitive.topology = WGPUPrimitiveTopology_LineStrip; break;
    case PrimitiveType::PointList: p.primitive.topology = WGPUPrimitiveTopology_PointList; break;
    }
    const bool strip = desc.primitive == PrimitiveType::TriangleStrip || desc.primitive == PrimitiveType::LineStrip;
    if (strip) p.primitive.stripIndexFormat = WGPUIndexFormat_Uint32; // indexed strips take 32-bit indices here
    p.primitive.frontFace = desc.rasterizer.frontFace == FrontFace::Clockwise ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    p.primitive.cullMode = desc.rasterizer.cull == CullMode::Front  ? WGPUCullMode_Front
                           : desc.rasterizer.cull == CullMode::Back ? WGPUCullMode_Back
                                                                    : WGPUCullMode_None;
    p.primitive.unclippedDepth = !desc.rasterizer.depthClip && caps.depthClamp;
    // FillMode::Line has no WebGPU form: such a pipeline fills.

    p.hasDepth = desc.depthFormat != TextureFormat::Invalid;
    p.depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    p.depthStencil.format = wg::toWgpu(desc.depthFormat);
    p.depthStencil.depthWriteEnabled = desc.depth.write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    p.depthStencil.depthCompare = desc.depth.test ? toWgpu(desc.depth.compare) : WGPUCompareFunction_Always;
    const bool triangles =
        desc.primitive == PrimitiveType::TriangleList || desc.primitive == PrimitiveType::TriangleStrip;
    if (desc.rasterizer.depthBias && triangles) {
        p.depthStencil.depthBias = std::int32_t(std::lround(desc.rasterizer.depthBiasConstant));
        p.depthStencil.depthBiasSlopeScale = desc.rasterizer.depthBiasSlope;
        p.depthStencil.depthBiasClamp = desc.rasterizer.depthBiasClamp;
    }
    p.multisample = WGPU_MULTISAMPLE_STATE_INIT;
    p.multisample.count = desc.sampleCount > 1 ? 4 : 1;
    p.multisample.alphaToCoverageEnabled = desc.alphaToCoverage;

    // The variant most draws use is built now, so that errors of the description show at creation.
    if (!variant(p, {0, 0})) return fail(std::format("cannot create pipeline '{}'", p.name));
    return static_cast<void*>(pipeline.release());
}

const WgPipeline::Variant* WebGpuDevice::variant(WgPipeline& p, const std::array<std::uint32_t, 2>& unfilterable) {
    for (const WgPipeline::Variant& v : p.variants)
        if (v.unfilterable == unfilterable) return &v;
    WgPipeline::Variant v;
    v.unfilterable = unfilterable;
    v.resourceLayouts[0] = resourceLayout(p.stages[0], WGPUShaderStage_Vertex, unfilterable[0]);
    v.resourceLayouts[1] = resourceLayout(p.stages[1], WGPUShaderStage_Fragment, unfilterable[1]);
    const std::array<WGPUBindGroupLayout, 4> layouts{v.resourceLayouts[0], p.uniformLayouts[0], v.resourceLayouts[1],
                                                     p.uniformLayouts[1]};
    WGPUPipelineLayoutDescriptor layoutDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layoutDesc.bindGroupLayoutCount = layouts.size();
    layoutDesc.bindGroupLayouts = layouts.data();
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(m_device, &layoutDesc);

    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.label = sv(p.name);
    d.layout = layout;
    d.vertex.module = p.stages[0].module;
    d.vertex.entryPoint = sv(p.stages[0].entry);
    d.vertex.bufferCount = p.vertexBuffers.size();
    d.vertex.buffers = p.vertexBuffers.data();
    d.primitive = p.primitive;
    d.depthStencil = p.hasDepth ? &p.depthStencil : nullptr;
    d.multisample = p.multisample;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = p.stages[1].module;
    fragment.entryPoint = sv(p.stages[1].entry);
    fragment.targetCount = p.colorTargets.size();
    fragment.targets = p.colorTargets.data();
    d.fragment = &fragment;
    v.pipeline = wgpuDeviceCreateRenderPipeline(m_device, &d);
    wgpuPipelineLayoutRelease(layout);
    if (!v.pipeline) return nullptr;
    p.variants.push_back(v);
    return &p.variants.back();
}

Result<void*> WebGpuDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    auto pipeline = std::make_unique<WgPipeline>();
    WgPipeline& p = *pipeline;
    p.compute = true;
    p.id = m_nextId++;
    p.name = desc.name.empty() ? std::string(desc.shader->entry) : std::string(desc.name);
    if (Result<void> stage = createStage(*desc.shader, p.stages[0]); !stage) {
        release(Kind::ComputePipeline, pipeline.release());
        return Error{stage.error()};
    }
    WgStage& stage = p.stages[0];
    p.readLayout = resourceLayout(stage, WGPUShaderStage_Compute, 0);
    std::vector<WGPUBindGroupLayoutEntry> writes;
    for (const WgBinding& b : stage.writes) {
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = b.binding;
        e.visibility = WGPUShaderStage_Compute;
        e.buffer.type = WGPUBufferBindingType_Storage;
        writes.push_back(e);
    }
    const std::string key = layoutKey(writes);
    if (const auto it = m_layouts.find(key); it != m_layouts.end()) {
        p.writeLayout = it->second;
    } else {
        WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        d.entryCount = writes.size();
        d.entries = writes.data();
        p.writeLayout = m_layouts[key] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
    }
    p.uniformLayouts[0] = uniformLayout(stage, WGPUShaderStage_Compute);
    const std::array<WGPUBindGroupLayout, 3> layouts{p.readLayout, p.writeLayout, p.uniformLayouts[0]};
    WGPUPipelineLayoutDescriptor layoutDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layoutDesc.bindGroupLayoutCount = layouts.size();
    layoutDesc.bindGroupLayouts = layouts.data();
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(m_device, &layoutDesc);
    WGPUComputePipelineDescriptor d = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    d.label = sv(p.name);
    d.layout = layout;
    d.compute.module = stage.module;
    d.compute.entryPoint = sv(stage.entry);
    p.computePipeline = wgpuDeviceCreateComputePipeline(m_device, &d);
    wgpuPipelineLayoutRelease(layout);
    if (!p.computePipeline) {
        release(Kind::ComputePipeline, pipeline.release());
        return Error{std::format("cannot create compute pipeline '{}'", desc.name)};
    }
    return static_cast<void*>(pipeline.release());
}

// ------------------------------------------------------------------ presentation

WgWindow* WebGpuDevice::findWindow(SDL_Window* window) {
    for (auto& w : m_windows)
        if (w->window == window) return w.get();
    return nullptr;
}

const WgWindow* WebGpuDevice::findWindow(SDL_Window* window) const {
    for (const auto& w : m_windows)
        if (w->window == window) return w.get();
    return nullptr;
}

Result<void> WebGpuDevice::claimWindow(SDL_Window* window) {
    const char* selector =
        SDL_GetStringProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING, "#canvas");
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    source.selector = sv(selector);
    WGPUSurfaceDescriptor d = WGPU_SURFACE_DESCRIPTOR_INIT;
    d.nextInChain = &source.chain;
    WGPUSurface surface = wgpuInstanceCreateSurface(m_instance, &d);
    if (!surface) return Error{std::format("cannot create a WebGPU surface for the canvas '{}'", selector)};
    auto entry = std::make_unique<WgWindow>();
    entry->window = window;
    entry->surface = surface;
    // The canvas's preferred format comes first.
    WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(surface, m_adapter, &capabilities) == WGPUStatus_Success) {
        if (capabilities.formatCount > 0 && capabilities.formats[0] == WGPUTextureFormat_RGBA8Unorm)
            entry->format = WGPUTextureFormat_RGBA8Unorm;
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    }
    m_windows.push_back(std::move(entry));
    return {};
}

void WebGpuDevice::releaseWindow(SDL_Window* window) {
    const auto it =
        std::find_if(m_windows.begin(), m_windows.end(), [&](const auto& w) { return w->window == window; });
    if (it == m_windows.end()) return;
    WgWindow& w = **it;
    for (WgTexture::View& v : w.current.views) wgpuTextureViewRelease(v.view);
    if (w.current.texture) wgpuTextureRelease(w.current.texture);
    if (w.width) wgpuSurfaceUnconfigure(w.surface);
    wgpuSurfaceRelease(w.surface);
    m_windows.erase(it);
}

// The browser presents at the display's rate, and only SDR canvases are configured here.
bool WebGpuDevice::supportsPresentMode(SDL_Window*, PresentMode mode) const { return mode == PresentMode::Vsync; }

bool WebGpuDevice::supportsComposition(SDL_Window*, SwapchainComposition composition) const {
    return composition == SwapchainComposition::Sdr;
}

bool WebGpuDevice::setSwapchainParameters(SDL_Window*, SwapchainComposition composition, PresentMode mode) {
    return composition == SwapchainComposition::Sdr && mode == PresentMode::Vsync;
}

TextureFormat WebGpuDevice::swapchainFormat(SDL_Window* window) const {
    const WgWindow* w = findWindow(window);
    return w ? wg::fromWgpu(w->format) : TextureFormat::Invalid;
}

TextureRef WebGpuDevice::acquireSwapchainTexture(void*, SDL_Window* window) {
    WgWindow* w = findWindow(window);
    if (!w) return {};
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (width <= 0 || height <= 0) return {};
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = m_device;
    config.format = w->format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = std::uint32_t(width);
    config.height = std::uint32_t(height);
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    config.presentMode = WGPUPresentMode_Fifo;
    if (config.width != w->width || config.height != w->height) {
        // Safari 27 sometimes never shows a canvas configured at the start: its frames are rendered (a copy of the
        // canvas has them), but the page stays black until the canvas's size changes. A change of one pixel and
        // back, a few frames later, makes it show them.
        if (w->width == 0) w->framesUntilRefresh = 20;
        wgpuSurfaceConfigure(w->surface, &config);
        w->width = config.width;
        w->height = config.height;
    } else if (w->framesUntilRefresh > 0 && --w->framesUntilRefresh == 0) {
        config.width = config.width > 1 ? config.width - 1 : 2;
        wgpuSurfaceConfigure(w->surface, &config);
        config.width = w->width;
        wgpuSurfaceConfigure(w->surface, &config);
    }
    for (WgTexture::View& v : w->current.views) wgpuTextureViewRelease(v.view);
    w->current.views.clear();
    if (w->current.texture) wgpuTextureRelease(w->current.texture);
    w->current.texture = nullptr;
    WGPUSurfaceTexture surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(w->surface, &surfaceTexture);
    if (!surfaceTexture.texture || (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                                    surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
        log::error("gpu", "no canvas texture (status {})", std::uint32_t(surfaceTexture.status));
        if (surfaceTexture.texture) wgpuTextureRelease(surfaceTexture.texture);
        w->width = w->height = 0; // configure again
        return {};
    }
    w->current.texture = surfaceTexture.texture;
    w->current.format = w->format;
    w->current.id = m_nextId++;
    w->current.desc = {};
    w->current.desc.format = wg::fromWgpu(w->format);
    w->current.desc.usage = TextureUsage::ColorTarget;
    w->current.desc.width = w->width;
    w->current.desc.height = w->height;
    TextureRef ref;
    ref.native = &w->current;
    ref.desc = w->current.desc;
    return ref;
}

// ------------------------------------------------------------------ fences

bool WebGpuDevice::isSignaled(void* fence) const { return *static_cast<WgFence*>(fence)->pending == 0; }

void WebGpuDevice::wait(void* fence) const {
    const std::uint32_t& pending = *static_cast<WgFence*>(fence)->pending;
    while (pending != 0) emscripten_sleep(1);
}

} // namespace quench::gpu::detail
