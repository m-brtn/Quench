#include "gpu/sdlgpu/SdlGpu.h"

#include "core/Platform.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>

#if QUENCH_PLATFORM_APPLE
extern "C" double quench_sdlgpu_metal_fence_ms(const void* fence);
#endif

namespace quench::gpu {

// ------------------------------------------------------------------ format mapping

SDL_GPUTextureFormat sdl::toSdl(TextureFormat format) {
    switch (format) {
    case TextureFormat::Invalid: return SDL_GPU_TEXTUREFORMAT_INVALID;
    case TextureFormat::R8Unorm: return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    case TextureFormat::Rg8Unorm: return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    case TextureFormat::Rgba8Unorm: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case TextureFormat::Rgba8UnormSrgb: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case TextureFormat::Bgra8Unorm: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case TextureFormat::Bgra8UnormSrgb: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    case TextureFormat::Rgb10a2Unorm: return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
    case TextureFormat::R16Float: return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
    case TextureFormat::Rg16Float: return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    case TextureFormat::Rgba16Float: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::R32Float: return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    case TextureFormat::Rg32Float: return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
    case TextureFormat::Rgba32Float: return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    case TextureFormat::Rg11b10Ufloat: return SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
    case TextureFormat::R32Uint: return SDL_GPU_TEXTUREFORMAT_R32_UINT;
    case TextureFormat::Bc1RgbaUnorm: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case TextureFormat::Bc1RgbaUnormSrgb: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB;
    case TextureFormat::Bc3RgbaUnorm: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case TextureFormat::Bc3RgbaUnormSrgb: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB;
    case TextureFormat::Bc4RUnorm: return SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM;
    case TextureFormat::Bc5RgUnorm: return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
    case TextureFormat::Bc6hRgbUfloat: return SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT;
    case TextureFormat::Bc7RgbaUnorm: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    case TextureFormat::Bc7RgbaUnormSrgb: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB;
    case TextureFormat::Astc4x4Unorm: return SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM;
    case TextureFormat::Astc4x4UnormSrgb: return SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM_SRGB;
    case TextureFormat::Depth16Unorm: return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    case TextureFormat::Depth24UnormStencil8: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::Depth32Float: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case TextureFormat::Depth32FloatStencil8: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    case TextureFormat::Count: break;
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

TextureFormat sdl::fromSdl(SDL_GPUTextureFormat format) {
    for (std::size_t i = 1; i < std::size_t(TextureFormat::Count); ++i)
        if (toSdl(TextureFormat(i)) == format) return TextureFormat(i);
    return TextureFormat::Invalid;
}

// ------------------------------------------------------------------ resources

namespace detail {

double metalFenceMilliseconds(SDL_GPUFence* fence) {
#if QUENCH_PLATFORM_APPLE
    return quench_sdlgpu_metal_fence_ms(fence);
#else
    (void)fence;
    return -1.0;
#endif
}

void release(DeviceImpl* device, Kind kind, void* native) noexcept {
    SDL_GPUDevice* d = device->device;
    switch (kind) {
    case Kind::Buffer: SDL_ReleaseGPUBuffer(d, static_cast<SDL_GPUBuffer*>(native)); break;
    case Kind::Texture: SDL_ReleaseGPUTexture(d, static_cast<SDL_GPUTexture*>(native)); break;
    case Kind::Sampler: SDL_ReleaseGPUSampler(d, static_cast<SDL_GPUSampler*>(native)); break;
    case Kind::TransferBuffer: SDL_ReleaseGPUTransferBuffer(d, static_cast<SDL_GPUTransferBuffer*>(native)); break;
    case Kind::GraphicsPipeline:
        SDL_ReleaseGPUGraphicsPipeline(d, static_cast<SDL_GPUGraphicsPipeline*>(native));
        break;
    case Kind::ComputePipeline: SDL_ReleaseGPUComputePipeline(d, static_cast<SDL_GPUComputePipeline*>(native)); break;
    case Kind::Fence: SDL_ReleaseGPUFence(d, static_cast<SDL_GPUFence*>(native)); break;
    }
    device->live.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace detail

namespace {

using detail::Access;
using detail::DeviceImpl;

template <class R>
R adopt(DeviceImpl* device, void* native) {
    R resource;
    Access::adopt(resource, device, native);
    device->live.fetch_add(1, std::memory_order_relaxed);
    return resource;
}

// SDL properties holding a debug name; empty (0) without a name.
class NameProps {
public:
    NameProps(std::string_view name, const char* key) {
        if (name.empty()) return;
        m_id = SDL_CreateProperties();
        const std::string text(name);
        SDL_SetStringProperty(m_id, key, text.c_str());
    }
    ~NameProps() {
        if (m_id) SDL_DestroyProperties(m_id);
    }
    NameProps(const NameProps&) = delete;
    NameProps& operator=(const NameProps&) = delete;
    SDL_PropertiesID id() const { return m_id; }

private:
    SDL_PropertiesID m_id = 0;
};

std::string describe(std::string_view kind, std::string_view name) {
    return name.empty() ? std::string(kind) : std::format("{} '{}'", kind, name);
}

Error sdlError(std::string what) { return Error{std::format("{}: {}", what, SDL_GetError())}; }

ShaderCode shaderCode(const ShaderBlob& blob, ShaderFormat format) {
    switch (format) {
    case ShaderFormat::Spirv: return blob.spirv;
    case ShaderFormat::Msl: return blob.msl;
    case ShaderFormat::Metallib: return blob.metallib;
    case ShaderFormat::Dxil: return blob.dxil;
    }
    return {nullptr, 0};
}

Result<SDL_GPUShader*> createShader(DeviceImpl& device, const ShaderBlob& blob) {
    const ShaderCode code = shaderCode(blob, device.caps.shaderFormat);
    if (!code)
        return Error{std::format("shader '{}' has no {} code in this build", blob.entry,
                                 shaderFormatName(device.caps.shaderFormat))};
    SDL_GPUShaderCreateInfo info{};
    info.code = code.data;
    info.code_size = code.size;
    info.entrypoint = blob.entry;
    info.format = device.shaderFormat;
    info.stage = sdl::toSdl(blob.stage);
    info.num_samplers = blob.resources.samplers;
    info.num_storage_textures = blob.resources.storageTextures;
    info.num_storage_buffers = blob.resources.storageBuffers;
    info.num_uniform_buffers = blob.resources.uniformBuffers;
    const NameProps props(blob.entry, SDL_PROP_GPU_SHADER_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUShader* shader = SDL_CreateGPUShader(device.device, &info);
    if (!shader) return sdlError(std::format("cannot create shader '{}'", blob.entry));
    return shader;
}

// The formats quench_add_shaders() embeds on this platform (cmake/QuenchShaders.cmake).
SDL_GPUShaderFormat builtShaderFormats() {
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#if QUENCH_SHADERS_METALLIB
    formats |= SDL_GPU_SHADERFORMAT_METALLIB;
#endif
#if QUENCH_SHADERS_MSL
    formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
#if QUENCH_SHADERS_DXIL
    formats |= SDL_GPU_SHADERFORMAT_DXIL;
#endif
    return formats;
}

} // namespace

// ------------------------------------------------------------------ TransferBuffer

std::span<std::byte> TransferBuffer::map(bool cycle) {
    QUENCH_ASSERT(m_native, "map() on an empty TransferBuffer");
    void* data = SDL_MapGPUTransferBuffer(m_device->device, static_cast<SDL_GPUTransferBuffer*>(m_native), cycle);
    if (!data) {
        log::error("gpu", "SDL_MapGPUTransferBuffer: {}", SDL_GetError());
        return {};
    }
    return {static_cast<std::byte*>(data), m_info.size};
}

void TransferBuffer::unmap() {
    QUENCH_ASSERT(m_native, "unmap() on an empty TransferBuffer");
    SDL_UnmapGPUTransferBuffer(m_device->device, static_cast<SDL_GPUTransferBuffer*>(m_native));
}

// ------------------------------------------------------------------ Device

Result<Device> Device::create(const DeviceDesc& desc) {
    const SDL_GPUShaderFormat offered = builtShaderFormats();
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, desc.debug);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, desc.preferLowPower);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
                           (offered & SDL_GPU_SHADERFORMAT_SPIRV) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN,
                           (offered & SDL_GPU_SHADERFORMAT_MSL) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_METALLIB_BOOLEAN,
                           (offered & SDL_GPU_SHADERFORMAT_METALLIB) != 0);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN,
                           (offered & SDL_GPU_SHADERFORMAT_DXIL) != 0);
    const std::string driver(desc.driver);
    if (!driver.empty()) SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, driver.c_str());
    // SDL GPU needs the video subsystem for every driver. The device holds a reference (SDL counts them), released by
    // the destructor.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_DestroyProperties(props);
        return sdlError("cannot initialize SDL video for the GPU device");
    }
    SDL_GPUDevice* sdlDevice = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    if (!sdlDevice) {
        Error error =
            sdlError(driver.empty() ? "cannot create a GPU device" : "cannot create a GPU device (" + driver + ")");
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return error;
    }

    Device device;
    device.m_impl = std::make_unique<DeviceImpl>();
    DeviceImpl& impl = *device.m_impl;
    impl.device = sdlDevice;
    Caps& caps = impl.caps;
    caps.driver = SDL_GetGPUDeviceDriver(sdlDevice);
    caps.deviceName = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(sdlDevice), SDL_PROP_GPU_DEVICE_NAME_STRING, "");

    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(sdlDevice) & offered;
    if (supported & SDL_GPU_SHADERFORMAT_METALLIB) {
        impl.shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
        caps.shaderFormat = ShaderFormat::Metallib;
    } else if (supported & SDL_GPU_SHADERFORMAT_MSL) {
        impl.shaderFormat = SDL_GPU_SHADERFORMAT_MSL;
        caps.shaderFormat = ShaderFormat::Msl;
    } else if (supported & SDL_GPU_SHADERFORMAT_DXIL) {
        impl.shaderFormat = SDL_GPU_SHADERFORMAT_DXIL;
        caps.shaderFormat = ShaderFormat::Dxil;
    } else if (supported & SDL_GPU_SHADERFORMAT_SPIRV) {
        impl.shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
        caps.shaderFormat = ShaderFormat::Spirv;
    } else {
        return Error{std::format("GPU driver '{}' accepts none of the shader formats built in", caps.driver)};
    }

    const bool metal = caps.driver == "metal";
    const bool vulkan = caps.driver == "vulkan";
    caps.gpuTiming = QUENCH_PLATFORM_APPLE && metal;
    // The SDL patch for the iOS Simulator skips setDepthClipMode (cmake/patches/SdlIosSimulator.cmake).
    caps.depthClamp = !(metal && QUENCH_PLATFORM_IOS_SIMULATOR);
    // SDL's Vulkan backend does not enable the depthBiasClamp device feature.
    caps.depthBiasClamp = !vulkan;
    // The Simulator's Metal device is GPU family Apple2, without indirect draws; SDL requires Apple3 on devices.
    caps.indirectDraw = !(metal && QUENCH_PLATFORM_IOS_SIMULATOR);
    caps.bcTextures =
        device.supportsFormat(TextureFormat::Bc1RgbaUnorm, TextureType::Texture2D, TextureUsage::Sampled) &&
        device.supportsFormat(TextureFormat::Bc7RgbaUnorm, TextureType::Texture2D, TextureUsage::Sampled);
    caps.astcTextures =
        device.supportsFormat(TextureFormat::Astc4x4Unorm, TextureType::Texture2D, TextureUsage::Sampled);

    log::info("gpu", "driver {}, device '{}', shaders {}", caps.driver, caps.deviceName,
              shaderFormatName(caps.shaderFormat));
    return device;
}

Device::Device(Device&& other) noexcept = default;
Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        const Device previous(std::move(*this));
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Device::~Device() {
    if (!m_impl) return;
    DeviceImpl& impl = *m_impl;
    SDL_WaitForGPUIdle(impl.device);
    for (const detail::WindowEntry& entry : impl.windows) SDL_ReleaseWindowFromGPUDevice(impl.device, entry.window);
    const std::int64_t live = impl.live.load();
    if (live != 0) log::error("gpu", "{} GPU resources outlive their device", live);
    QUENCH_ASSERT(live == 0, "GPU resources outlive their device (release them before the device)");
    SDL_DestroyGPUDevice(impl.device);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

const Caps& Device::caps() const { return m_impl->caps; }

bool Device::supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const {
    return SDL_GPUTextureSupportsFormat(m_impl->device, sdl::toSdl(format), sdl::toSdl(type), sdl::toSdl(usage));
}

bool Device::supportsSampleCount(TextureFormat format, std::uint32_t sampleCount) const {
    if (!sdl::validSampleCount(sampleCount)) return false;
    return sampleCount == 1 ||
           SDL_GPUTextureSupportsSampleCount(m_impl->device, sdl::toSdl(format), sdl::sampleCountToSdl(sampleCount));
}

std::int64_t Device::liveResources() const { return m_impl->live.load(); }

Result<Texture> Device::createTexture(const TextureDesc& desc) {
    QUENCH_ASSERT(desc.format != TextureFormat::Invalid && desc.format < TextureFormat::Count, "invalid format");
    QUENCH_ASSERT(sdl::validSampleCount(desc.sampleCount), "sample count must be 1, 2, 4 or 8");
    QUENCH_ASSERT(desc.width > 0 && desc.height > 0 && desc.layers > 0 && desc.mipLevels > 0, "empty texture");
    SDL_GPUTextureCreateInfo info{};
    info.type = sdl::toSdl(desc.type);
    info.format = sdl::toSdl(desc.format);
    info.usage = sdl::toSdl(desc.usage);
    info.width = desc.width;
    info.height = desc.height;
    info.layer_count_or_depth = desc.layers;
    info.num_levels = desc.mipLevels;
    info.sample_count = sdl::sampleCountToSdl(desc.sampleCount);
    // SDL checks these only in its debug mode.
    if (!SDL_GPUTextureSupportsFormat(m_impl->device, info.format, info.type, info.usage))
        return Error{std::format("cannot create {}: {} does not support this type and usage on {}",
                                 describe("texture", desc.name), formatInfo(desc.format).name, m_impl->caps.driver)};
    if (!supportsSampleCount(desc.format, desc.sampleCount))
        return Error{std::format("cannot create {}: {} does not support {} samples on {}",
                                 describe("texture", desc.name), formatInfo(desc.format).name, desc.sampleCount,
                                 m_impl->caps.driver)};
    const NameProps props(desc.name, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUTexture* native = SDL_CreateGPUTexture(m_impl->device, &info);
    if (!native)
        return sdlError(std::format("cannot create {} {}x{} {}", describe("texture", desc.name), desc.width,
                                    desc.height, formatInfo(desc.format).name));
    Texture texture = adopt<Texture>(m_impl.get(), native);
    Access::info(texture) = desc;
    Access::info(texture).name = {};
    return texture;
}

Result<Buffer> Device::createBuffer(const BufferDesc& desc) {
    QUENCH_ASSERT(desc.size > 0, "empty buffer");
    SDL_GPUBufferCreateInfo info{};
    info.usage = sdl::toSdl(desc.usage);
    info.size = desc.size;
    const NameProps props(desc.name, SDL_PROP_GPU_BUFFER_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUBuffer* native = SDL_CreateGPUBuffer(m_impl->device, &info);
    if (!native) return sdlError(std::format("cannot create {} of {} bytes", describe("buffer", desc.name), desc.size));
    Buffer buffer = adopt<Buffer>(m_impl.get(), native);
    Access::info(buffer) = desc;
    Access::info(buffer).name = {};
    return buffer;
}

Result<Sampler> Device::createSampler(const SamplerDesc& desc) {
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = sdl::toSdl(desc.minFilter);
    info.mag_filter = sdl::toSdl(desc.magFilter);
    info.mipmap_mode = sdl::toSdl(desc.mipmapMode);
    info.address_mode_u = sdl::toSdl(desc.addressU);
    info.address_mode_v = sdl::toSdl(desc.addressV);
    info.address_mode_w = sdl::toSdl(desc.addressW);
    info.mip_lod_bias = desc.mipLodBias;
    info.enable_anisotropy = desc.maxAnisotropy > 1.0f;
    info.max_anisotropy = std::max(desc.maxAnisotropy, 1.0f);
    info.enable_compare = desc.compare;
    info.compare_op = sdl::toSdl(desc.compareOp);
    info.min_lod = desc.minLod;
    info.max_lod = desc.maxLod;
    const NameProps props(desc.name, SDL_PROP_GPU_SAMPLER_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUSampler* native = SDL_CreateGPUSampler(m_impl->device, &info);
    if (!native) return sdlError("cannot create " + describe("sampler", desc.name));
    return adopt<Sampler>(m_impl.get(), native);
}

Result<TransferBuffer> Device::createTransferBuffer(const TransferBufferDesc& desc) {
    QUENCH_ASSERT(desc.size > 0, "empty transfer buffer");
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = desc.direction == TransferDirection::Upload ? SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD
                                                             : SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = desc.size;
    const NameProps props(desc.name, SDL_PROP_GPU_TRANSFERBUFFER_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUTransferBuffer* native = SDL_CreateGPUTransferBuffer(m_impl->device, &info);
    if (!native)
        return sdlError(std::format("cannot create {} of {} bytes", describe("transfer buffer", desc.name), desc.size));
    TransferBuffer buffer = adopt<TransferBuffer>(m_impl.get(), native);
    Access::info(buffer) = desc;
    Access::info(buffer).name = {};
    return buffer;
}

Result<GraphicsPipeline> Device::createPipeline(const GraphicsPipelineDesc& desc) {
    QUENCH_ASSERT(desc.vertexShader && desc.vertexShader->stage == ShaderStage::Vertex, "vertex shader missing");
    QUENCH_ASSERT(desc.fragmentShader && desc.fragmentShader->stage == ShaderStage::Fragment,
                  "fragment shader missing");
    QUENCH_ASSERT(desc.colorTargets.size() <= 4, "at most 4 color targets");
    QUENCH_ASSERT(sdl::validSampleCount(desc.sampleCount), "sample count must be 1, 2, 4 or 8");
    QUENCH_ASSERT(desc.vertexBuffers.size() <= 16 && desc.vertexAttributes.size() <= 16, "too many vertex inputs");
    const Caps& caps = m_impl->caps;

    std::array<SDL_GPUVertexBufferDescription, 16> buffers{};
    for (std::size_t i = 0; i < desc.vertexBuffers.size(); ++i) {
        const VertexBufferLayout& in = desc.vertexBuffers[i];
        buffers[i].slot = in.slot;
        buffers[i].pitch = in.stride;
        buffers[i].input_rate = sdl::toSdl(in.rate);
    }
    std::array<SDL_GPUVertexAttribute, 16> attributes{};
    for (std::size_t i = 0; i < desc.vertexAttributes.size(); ++i) {
        const VertexAttribute& in = desc.vertexAttributes[i];
        attributes[i] = {in.location, in.slot, sdl::toSdl(in.format), in.offset};
    }
    std::array<SDL_GPUColorTargetDescription, 4> colors{};
    detail::PipelineInfo pipelineInfo;
    for (std::size_t i = 0; i < desc.colorTargets.size(); ++i) {
        const ColorTargetDesc& in = desc.colorTargets[i];
        QUENCH_ASSERT(in.format != TextureFormat::Invalid, "color target without a format");
        colors[i].format = sdl::toSdl(in.format);
        SDL_GPUColorTargetBlendState& blend = colors[i].blend_state;
        blend.enable_blend = in.blend.enabled;
        blend.src_color_blendfactor = sdl::toSdl(in.blend.srcColor);
        blend.dst_color_blendfactor = sdl::toSdl(in.blend.dstColor);
        blend.color_blend_op = sdl::toSdl(in.blend.colorOp);
        blend.src_alpha_blendfactor = sdl::toSdl(in.blend.srcAlpha);
        blend.dst_alpha_blendfactor = sdl::toSdl(in.blend.dstAlpha);
        blend.alpha_blend_op = sdl::toSdl(in.blend.alphaOp);
        blend.enable_color_write_mask = in.blend.writeMask != ColorWrite::All;
        blend.color_write_mask = sdl::toSdl(in.blend.writeMask);
        pipelineInfo.colorFormats[i] = in.format;
    }
    pipelineInfo.colorTargets = std::uint32_t(desc.colorTargets.size());
    pipelineInfo.depthFormat = desc.depthFormat;
    pipelineInfo.sampleCount = desc.sampleCount;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_input_state.vertex_buffer_descriptions = buffers.data();
    info.vertex_input_state.num_vertex_buffers = std::uint32_t(desc.vertexBuffers.size());
    info.vertex_input_state.vertex_attributes = attributes.data();
    info.vertex_input_state.num_vertex_attributes = std::uint32_t(desc.vertexAttributes.size());
    info.primitive_type = sdl::toSdl(desc.primitive);
    SDL_GPURasterizerState& raster = info.rasterizer_state;
    raster.fill_mode = sdl::toSdl(desc.rasterizer.fill);
    raster.cull_mode = sdl::toSdl(desc.rasterizer.cull);
    raster.front_face = sdl::toSdl(desc.rasterizer.frontFace);
    raster.enable_depth_bias = desc.rasterizer.depthBias;
    raster.depth_bias_constant_factor = desc.rasterizer.depthBiasConstant;
    raster.depth_bias_slope_factor = desc.rasterizer.depthBiasSlope;
    raster.depth_bias_clamp = caps.depthBiasClamp ? desc.rasterizer.depthBiasClamp : 0.0f;
    raster.enable_depth_clip = desc.rasterizer.depthClip || !caps.depthClamp;
    info.multisample_state.sample_count = sdl::sampleCountToSdl(desc.sampleCount);
    info.multisample_state.enable_alpha_to_coverage = desc.alphaToCoverage;
    info.depth_stencil_state.enable_depth_test = desc.depth.test;
    info.depth_stencil_state.enable_depth_write = desc.depth.write;
    info.depth_stencil_state.compare_op = sdl::toSdl(desc.depth.compare);
    info.target_info.color_target_descriptions = colors.data();
    info.target_info.num_color_targets = std::uint32_t(desc.colorTargets.size());
    info.target_info.has_depth_stencil_target = desc.depthFormat != TextureFormat::Invalid;
    info.target_info.depth_stencil_format = sdl::toSdl(desc.depthFormat);
    QUENCH_ASSERT(info.target_info.has_depth_stencil_target || (!desc.depth.test && !desc.depth.write),
                  "depth test or write without a depth format");

    Result<SDL_GPUShader*> vs = createShader(*m_impl, *desc.vertexShader);
    if (!vs) return Error{vs.error()};
    Result<SDL_GPUShader*> fs = createShader(*m_impl, *desc.fragmentShader);
    if (!fs) {
        SDL_ReleaseGPUShader(m_impl->device, *vs);
        return Error{fs.error()};
    }
    info.vertex_shader = *vs;
    info.fragment_shader = *fs;
    const std::string name = desc.name.empty()
                                 ? std::format("{}/{}", desc.vertexShader->entry, desc.fragmentShader->entry)
                                 : std::string(desc.name);
    const NameProps props(name, SDL_PROP_GPU_GRAPHICSPIPELINE_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUGraphicsPipeline* native = SDL_CreateGPUGraphicsPipeline(m_impl->device, &info);
    SDL_ReleaseGPUShader(m_impl->device, *vs);
    SDL_ReleaseGPUShader(m_impl->device, *fs);
    if (!native) return sdlError(std::format("cannot create pipeline '{}'", name));
    GraphicsPipeline pipeline = adopt<GraphicsPipeline>(m_impl.get(), native);
    Access::info(pipeline) = pipelineInfo;
    return pipeline;
}

Result<ComputePipeline> Device::createComputePipeline(const ComputePipelineDesc& desc) {
    QUENCH_ASSERT(desc.shader && desc.shader->stage == ShaderStage::Compute, "compute shader missing");
    const ShaderBlob& blob = *desc.shader;
    const ShaderCode code = shaderCode(blob, m_impl->caps.shaderFormat);
    if (!code)
        return Error{std::format("shader '{}' has no {} code in this build", blob.entry,
                                 shaderFormatName(m_impl->caps.shaderFormat))};
    const ShaderResources& r = blob.resources;
    SDL_GPUComputePipelineCreateInfo info{};
    info.code = code.data;
    info.code_size = code.size;
    info.entrypoint = blob.entry;
    info.format = m_impl->shaderFormat;
    info.num_samplers = r.samplers;
    info.num_readonly_storage_textures = r.storageTextures;
    info.num_readonly_storage_buffers = r.storageBuffers;
    info.num_readwrite_storage_textures = r.readWriteStorageTextures;
    info.num_readwrite_storage_buffers = r.readWriteStorageBuffers;
    info.num_uniform_buffers = r.uniformBuffers;
    info.threadcount_x = r.threadCount[0];
    info.threadcount_y = r.threadCount[1];
    info.threadcount_z = r.threadCount[2];
    const std::string name = desc.name.empty() ? std::string(blob.entry) : std::string(desc.name);
    const NameProps props(name, SDL_PROP_GPU_COMPUTEPIPELINE_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUComputePipeline* native = SDL_CreateGPUComputePipeline(m_impl->device, &info);
    if (!native) return sdlError(std::format("cannot create compute pipeline '{}'", name));
    return adopt<ComputePipeline>(m_impl.get(), native);
}

// ------------------------------------------------------------------ convenience transfers

Result<void> Device::upload(const BufferRef& destination, std::uint32_t offset, std::span<const std::byte> data) {
    QUENCH_ASSERT(destination, "upload to an empty buffer");
    QUENCH_ASSERT(offset + data.size() <= destination.desc.size, "upload past the end of the buffer");
    if (data.empty()) return {};
    Result<TransferBuffer> staging =
        createTransferBuffer({TransferDirection::Upload, std::uint32_t(data.size()), "upload"});
    if (!staging) return Error{staging.error()};
    std::span<std::byte> mapped = staging->map();
    if (mapped.empty()) return Error{"cannot map the upload buffer"};
    std::memcpy(mapped.data(), data.data(), data.size());
    staging->unmap();
    CommandBuffer commands = acquireCommandBuffer();
    if (!commands) return Error{"no command buffer for the upload"};
    {
        CopyPass copy = commands.beginCopyPass();
        copy.upload(*staging, 0, destination, offset, std::uint32_t(data.size()));
    }
    return commands.submit();
}

Result<void> Device::upload(const TextureRegion& destination, std::span<const std::byte> data) {
    QUENCH_ASSERT(destination.texture, "upload to an empty texture");
    const SDL_GPUTextureRegion region = sdl::toSdlRegion(destination);
    const std::uint64_t size = levelByteSize(destination.texture.desc.format, region.w, region.h, region.d);
    QUENCH_ASSERT(data.size() == size, "texture upload size does not match the region");
    if (data.size() != size)
        return Error{std::format("texture upload of {} bytes, the region needs {}", data.size(), size)};
    Result<TransferBuffer> staging =
        createTransferBuffer({TransferDirection::Upload, std::uint32_t(data.size()), "upload"});
    if (!staging) return Error{staging.error()};
    std::span<std::byte> mapped = staging->map();
    if (mapped.empty()) return Error{"cannot map the upload buffer"};
    std::memcpy(mapped.data(), data.data(), data.size());
    staging->unmap();
    CommandBuffer commands = acquireCommandBuffer();
    if (!commands) return Error{"no command buffer for the upload"};
    {
        CopyPass copy = commands.beginCopyPass();
        copy.upload(*staging, 0, destination);
    }
    return commands.submit();
}

Result<std::vector<std::byte>> Device::download(const TextureRegion& source) {
    QUENCH_ASSERT(source.texture, "download from an empty texture");
    const SDL_GPUTextureRegion region = sdl::toSdlRegion(source);
    const std::uint64_t size = levelByteSize(source.texture.desc.format, region.w, region.h, region.d);
    Result<TransferBuffer> staging =
        createTransferBuffer({TransferDirection::Download, std::uint32_t(size), "download"});
    if (!staging) return Error{staging.error()};
    CommandBuffer commands = acquireCommandBuffer();
    if (!commands) return Error{"no command buffer for the download"};
    {
        CopyPass copy = commands.beginCopyPass();
        copy.download(source, *staging, 0);
    }
    Result<Fence> fence = commands.submitWithFence();
    if (!fence) return Error{fence.error()};
    wait(*fence);
    std::span<std::byte> mapped = staging->map();
    if (mapped.size() < size) return Error{"cannot map the download buffer"};
    std::vector<std::byte> bytes(mapped.begin(), mapped.begin() + std::ptrdiff_t(size));
    staging->unmap();
    return bytes;
}

Result<std::vector<std::byte>> Device::download(const BufferRef& source, std::uint32_t offset, std::uint32_t size) {
    QUENCH_ASSERT(source, "download from an empty buffer");
    QUENCH_ASSERT(std::uint64_t(offset) + size <= source.desc.size, "download past the end of the buffer");
    Result<TransferBuffer> staging = createTransferBuffer({TransferDirection::Download, size, "download"});
    if (!staging) return Error{staging.error()};
    CommandBuffer commands = acquireCommandBuffer();
    if (!commands) return Error{"no command buffer for the download"};
    {
        CopyPass copy = commands.beginCopyPass();
        copy.download(source, offset, size, *staging, 0);
    }
    Result<Fence> fence = commands.submitWithFence();
    if (!fence) return Error{fence.error()};
    wait(*fence);
    std::span<std::byte> mapped = staging->map();
    if (mapped.size() < size) return Error{"cannot map the download buffer"};
    std::vector<std::byte> bytes(mapped.begin(), mapped.begin() + size);
    staging->unmap();
    return bytes;
}

// ------------------------------------------------------------------ presentation

Result<void> Device::claimWindow(SDL_Window* window, PresentMode mode) {
    QUENCH_ASSERT(window, "claimWindow(nullptr)");
    QUENCH_ASSERT(!m_impl->find(window), "window claimed twice");
    if (!SDL_ClaimWindowForGPUDevice(m_impl->device, window)) return sdlError("cannot claim the window for the GPU");
    m_impl->windows.push_back({window});
    setPresentMode(window, mode);
    return {};
}

void Device::releaseWindow(SDL_Window* window) {
    auto it = std::find_if(m_impl->windows.begin(), m_impl->windows.end(),
                           [window](const detail::WindowEntry& w) { return w.window == window; });
    QUENCH_ASSERT(it != m_impl->windows.end(), "releaseWindow on a window that was not claimed");
    if (it == m_impl->windows.end()) return;
    SDL_WaitForGPUIdle(m_impl->device);
    SDL_ReleaseWindowFromGPUDevice(m_impl->device, window);
    m_impl->windows.erase(it);
}

bool Device::supportsPresentMode(SDL_Window* window, PresentMode mode) const {
    return SDL_WindowSupportsGPUPresentMode(m_impl->device, window, sdl::toSdl(mode));
}

PresentMode Device::setPresentMode(SDL_Window* window, PresentMode mode) {
    detail::WindowEntry* entry = m_impl->find(window);
    QUENCH_ASSERT(entry, "setPresentMode on a window that was not claimed");
    if (!entry) return PresentMode::Vsync;
    if (!supportsPresentMode(window, mode)) mode = PresentMode::Vsync;
    if (!SDL_SetGPUSwapchainParameters(m_impl->device, window, sdl::toSdl(entry->composition), sdl::toSdl(mode))) {
        log::error("gpu", "SDL_SetGPUSwapchainParameters: {}", SDL_GetError());
        return entry->presentMode;
    }
    entry->presentMode = mode;
    return mode;
}

bool Device::supportsComposition(SDL_Window* window, SwapchainComposition composition) const {
    return SDL_WindowSupportsGPUSwapchainComposition(m_impl->device, window, sdl::toSdl(composition));
}

SwapchainComposition Device::setComposition(SDL_Window* window, SwapchainComposition composition) {
    detail::WindowEntry* entry = m_impl->find(window);
    QUENCH_ASSERT(entry, "setComposition on a window that was not claimed");
    if (!entry) return SwapchainComposition::Sdr;
    if (composition == entry->composition) return composition;
    if (!supportsComposition(window, composition)) {
        log::warning("gpu", "swapchain composition {} not supported by {} for this window: SDR",
                     swapchainCompositionName(composition), m_impl->caps.driver);
        composition = SwapchainComposition::Sdr;
        if (composition == entry->composition) return composition;
    }
    SDL_WaitForGPUIdle(m_impl->device);
    if (!SDL_SetGPUSwapchainParameters(m_impl->device, window, sdl::toSdl(composition),
                                       sdl::toSdl(entry->presentMode))) {
        log::error("gpu", "swapchain composition {}: {}", swapchainCompositionName(composition), SDL_GetError());
        return entry->composition;
    }
    entry->composition = composition;
    return composition;
}

SwapchainComposition Device::composition(SDL_Window* window) const {
    const detail::WindowEntry* entry = m_impl->find(window);
    return entry ? entry->composition : SwapchainComposition::Sdr;
}

TextureFormat Device::swapchainFormat(SDL_Window* window) const {
    return sdl::fromSdl(SDL_GetGPUSwapchainTextureFormat(m_impl->device, window));
}

// ------------------------------------------------------------------ fences

bool Device::isSignaled(const Fence& fence) const {
    QUENCH_ASSERT(fence, "isSignaled on an empty fence");
    return SDL_QueryGPUFence(m_impl->device, static_cast<SDL_GPUFence*>(Access::native(fence)));
}

void Device::wait(const Fence& fence) const {
    QUENCH_ASSERT(fence, "wait on an empty fence");
    SDL_GPUFence* native = static_cast<SDL_GPUFence*>(Access::native(fence));
    if (!SDL_WaitForGPUFences(m_impl->device, true, &native, 1))
        log::error("gpu", "SDL_WaitForGPUFences: {}", SDL_GetError());
}

std::optional<double> Device::gpuMilliseconds(const Fence& fence) const {
    if (!m_impl->caps.gpuTiming || !isSignaled(fence)) return std::nullopt;
    const double ms = detail::metalFenceMilliseconds(static_cast<SDL_GPUFence*>(Access::native(fence)));
    if (ms < 0.0) return std::nullopt;
    return ms;
}

void Device::waitIdle() { SDL_WaitForGPUIdle(m_impl->device); }

} // namespace quench::gpu
