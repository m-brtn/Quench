#include "gpu/sdlgpu/SdlGpu.h"

#include "core/Platform.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>

#include <algorithm>
#include <array>
#include <format>
#include <string>

#if QUENCH_PLATFORM_APPLE
extern "C" int quench_sdlgpu_metal_fence_times(const void* fence, double* start, double* end);
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
    case TextureFormat::Etc2Rgb8Unorm:
    case TextureFormat::Etc2Rgb8UnormSrgb:
    case TextureFormat::Etc2Rgba8Unorm:
    case TextureFormat::Etc2Rgba8UnormSrgb: return SDL_GPU_TEXTUREFORMAT_INVALID; // not in SDL GPU 3.4
    case TextureFormat::Depth16Unorm: return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    case TextureFormat::Depth24UnormStencil8: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case TextureFormat::Depth32Float: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case TextureFormat::Depth32FloatStencil8: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    case TextureFormat::Count: break;
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

TextureFormat sdl::fromSdl(SDL_GPUTextureFormat format) {
    if (format == SDL_GPU_TEXTUREFORMAT_INVALID) return TextureFormat::Invalid;
    for (std::size_t i = 1; i < std::size_t(TextureFormat::Count); ++i)
        if (toSdl(TextureFormat(i)) == format) return TextureFormat(i);
    return TextureFormat::Invalid;
}

namespace detail {

std::optional<double> metalFenceMilliseconds(SDL_GPUFence* fence) {
#if QUENCH_PLATFORM_APPLE
    double start = 0.0, end = 0.0;
    if (!quench_sdlgpu_metal_fence_times(fence, &start, &end)) return std::nullopt;
    return commandBufferMilliseconds(start, end);
#else
    (void)fence;
    return std::nullopt;
#endif
}

namespace {

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

// ------------------------------------------------------------------ device

Result<std::unique_ptr<DeviceImpl>> createSdlGpuDevice(const DeviceDesc& desc) {
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

    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(sdlDevice) & offered;
    SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_INVALID;
    ShaderFormat format = ShaderFormat::Spirv;
    if (supported & SDL_GPU_SHADERFORMAT_METALLIB) {
        shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
        format = ShaderFormat::Metallib;
    } else if (supported & SDL_GPU_SHADERFORMAT_MSL) {
        shaderFormat = SDL_GPU_SHADERFORMAT_MSL;
        format = ShaderFormat::Msl;
    } else if (supported & SDL_GPU_SHADERFORMAT_DXIL) {
        shaderFormat = SDL_GPU_SHADERFORMAT_DXIL;
        format = ShaderFormat::Dxil;
    } else if (supported & SDL_GPU_SHADERFORMAT_SPIRV) {
        shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
        format = ShaderFormat::Spirv;
    }
    // From here the device owns sdlDevice and the video reference.
    auto impl = std::make_unique<SdlGpuDevice>(sdlDevice, shaderFormat);
    Caps& caps = impl->caps;
    caps.backend = Backend::SdlGpu;
    caps.driver = SDL_GetGPUDeviceDriver(sdlDevice);
    caps.deviceName = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(sdlDevice), SDL_PROP_GPU_DEVICE_NAME_STRING, "");
    if (shaderFormat == SDL_GPU_SHADERFORMAT_INVALID)
        return Error{std::format("GPU driver '{}' accepts none of the shader formats built in", caps.driver)};
    caps.shaderFormat = format;

    const bool metal = caps.driver == "metal";
    const bool vulkan = caps.driver == "vulkan";
    caps.gpuTiming = QUENCH_PLATFORM_APPLE && metal;
    caps.gpuPassTiming = false;
    // The SDL patch for the iOS Simulator skips setDepthClipMode (cmake/patches/SdlIosSimulator.cmake).
    caps.depthClamp = !(metal && QUENCH_PLATFORM_IOS_SIMULATOR);
    // SDL's Vulkan backend does not enable the depthBiasClamp device feature.
    caps.depthBiasClamp = !vulkan;
    // The Simulator's Metal device is GPU family Apple2, without indirect draws; SDL requires Apple3 on devices.
    caps.indirectDraw = !(metal && QUENCH_PLATFORM_IOS_SIMULATOR);
    return std::unique_ptr<DeviceImpl>(std::move(impl));
}

SdlGpuDevice::~SdlGpuDevice() {
    SDL_DestroyGPUDevice(m_device);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool SdlGpuDevice::supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const {
    const SDL_GPUTextureFormat sdlFormat = sdl::toSdl(format);
    if (sdlFormat == SDL_GPU_TEXTUREFORMAT_INVALID) return false;
    return SDL_GPUTextureSupportsFormat(m_device, sdlFormat, sdl::toSdl(type), sdl::toSdl(usage));
}

bool SdlGpuDevice::supportsSampleCount(TextureFormat format, std::uint32_t sampleCount) const {
    return SDL_GPUTextureSupportsSampleCount(m_device, sdl::toSdl(format), sdl::sampleCountToSdl(sampleCount));
}

void SdlGpuDevice::waitIdle() { SDL_WaitForGPUIdle(m_device); }

// ------------------------------------------------------------------ resources

void SdlGpuDevice::release(Kind kind, void* native) noexcept {
    SDL_GPUDevice* d = m_device;
    switch (kind) {
    case Kind::Buffer: SDL_ReleaseGPUBuffer(d, static_cast<SDL_GPUBuffer*>(native)); break;
    case Kind::Texture: SDL_ReleaseGPUTexture(d, static_cast<SDL_GPUTexture*>(native)); break;
    case Kind::Sampler: SDL_ReleaseGPUSampler(d, static_cast<SDL_GPUSampler*>(native)); break;
    case Kind::TransferBuffer: SDL_ReleaseGPUTransferBuffer(d, static_cast<SDL_GPUTransferBuffer*>(native)); break;
    case Kind::GraphicsPipeline:
        SDL_ReleaseGPUGraphicsPipeline(d, static_cast<SDL_GPUGraphicsPipeline*>(native));
        break;
    case Kind::ComputePipeline: SDL_ReleaseGPUComputePipeline(d, static_cast<SDL_GPUComputePipeline*>(native)); break;
    case Kind::Fence: {
        // SDL's Vulkan driver shares one reference between the fence and its command buffer: a fence released
        // before a cleanup pass has seen it signalled goes back to the pool while the command buffer still points at
        // it, is reset by its next submission, and the command buffer (with the textures and buffers it holds) can
        // stay in flight for good. Waiting on a signalled fence does not block and runs that cleanup pass first.
        SDL_GPUFence* fence = static_cast<SDL_GPUFence*>(native);
        if (SDL_QueryGPUFence(d, fence)) SDL_WaitForGPUFences(d, true, &fence, 1);
        SDL_ReleaseGPUFence(d, fence);
        break;
    }
    case Kind::AccelerationStructure:
    case Kind::Upscaler: QUENCH_ASSERT(false, "SDL GPU creates no acceleration structures or upscalers"); break;
    }
}

std::span<std::byte> SdlGpuDevice::map(void* transferBuffer, std::uint32_t size, bool cycle) {
    void* data = SDL_MapGPUTransferBuffer(m_device, static_cast<SDL_GPUTransferBuffer*>(transferBuffer), cycle);
    if (!data) {
        log::error("gpu", "SDL_MapGPUTransferBuffer: {}", SDL_GetError());
        return {};
    }
    return {static_cast<std::byte*>(data), size};
}

void SdlGpuDevice::unmap(void* transferBuffer) {
    SDL_UnmapGPUTransferBuffer(m_device, static_cast<SDL_GPUTransferBuffer*>(transferBuffer));
}

Result<SDL_GPUShader*> SdlGpuDevice::createShader(const ShaderBlob& blob) {
    const ShaderCode code = shaderCodeFor(blob, caps.shaderFormat);
    if (!code)
        return Error{
            std::format("shader '{}' has no {} code in this build", blob.entry, shaderFormatName(caps.shaderFormat))};
    SDL_GPUShaderCreateInfo info{};
    info.code = code.data;
    info.code_size = code.size;
    info.entrypoint = blob.entry;
    info.format = m_shaderFormat;
    info.stage = sdl::toSdl(blob.stage);
    info.num_samplers = blob.resources.samplers;
    info.num_storage_textures = blob.resources.storageTextures;
    info.num_storage_buffers = blob.resources.storageBuffers;
    info.num_uniform_buffers = blob.resources.uniformBuffers;
    const NameProps props(blob.entry, SDL_PROP_GPU_SHADER_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUShader* shader = SDL_CreateGPUShader(m_device, &info);
    if (!shader) return sdlError(std::format("cannot create shader '{}'", blob.entry));
    return shader;
}

Result<void*> SdlGpuDevice::createTexture(const TextureDesc& desc) {
    SDL_GPUTextureCreateInfo info{};
    info.type = sdl::toSdl(desc.type);
    info.format = sdl::toSdl(desc.format);
    info.usage = sdl::toSdl(desc.usage);
    info.width = desc.width;
    info.height = desc.height;
    info.layer_count_or_depth = desc.layers;
    info.num_levels = desc.mipLevels;
    info.sample_count = sdl::sampleCountToSdl(desc.sampleCount);
    const NameProps props(desc.name, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUTexture* native = SDL_CreateGPUTexture(m_device, &info);
    if (!native)
        return sdlError(std::format("cannot create {} {}x{} {}", describe("texture", desc.name), desc.width,
                                    desc.height, formatInfo(desc.format).name));
    return static_cast<void*>(native);
}

Result<void*> SdlGpuDevice::createBuffer(const BufferDesc& desc) {
    SDL_GPUBufferCreateInfo info{};
    info.usage = sdl::toSdl(desc.usage);
    info.size = desc.size;
    const NameProps props(desc.name, SDL_PROP_GPU_BUFFER_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUBuffer* native = SDL_CreateGPUBuffer(m_device, &info);
    if (!native) return sdlError(std::format("cannot create {} of {} bytes", describe("buffer", desc.name), desc.size));
    return static_cast<void*>(native);
}

Result<void*> SdlGpuDevice::createSampler(const SamplerDesc& desc) {
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
    SDL_GPUSampler* native = SDL_CreateGPUSampler(m_device, &info);
    if (!native) return sdlError("cannot create " + describe("sampler", desc.name));
    return static_cast<void*>(native);
}

Result<void*> SdlGpuDevice::createTransferBuffer(const TransferBufferDesc& desc) {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = desc.direction == TransferDirection::Upload ? SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD
                                                             : SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = desc.size;
    const NameProps props(desc.name, SDL_PROP_GPU_TRANSFERBUFFER_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUTransferBuffer* native = SDL_CreateGPUTransferBuffer(m_device, &info);
    if (!native)
        return sdlError(std::format("cannot create {} of {} bytes", describe("transfer buffer", desc.name), desc.size));
    return static_cast<void*>(native);
}

Result<void*> SdlGpuDevice::createPipeline(const GraphicsPipelineDesc& desc) {
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
    for (std::size_t i = 0; i < desc.colorTargets.size(); ++i) {
        const ColorTargetDesc& in = desc.colorTargets[i];
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
    }

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

    Result<SDL_GPUShader*> vs = createShader(*desc.vertexShader);
    if (!vs) return Error{vs.error()};
    Result<SDL_GPUShader*> fs = createShader(*desc.fragmentShader);
    if (!fs) {
        SDL_ReleaseGPUShader(m_device, *vs);
        return Error{fs.error()};
    }
    info.vertex_shader = *vs;
    info.fragment_shader = *fs;
    const std::string name = desc.name.empty()
                                 ? std::format("{}/{}", desc.vertexShader->entry, desc.fragmentShader->entry)
                                 : std::string(desc.name);
    const NameProps props(name, SDL_PROP_GPU_GRAPHICSPIPELINE_CREATE_NAME_STRING);
    info.props = props.id();
    SDL_GPUGraphicsPipeline* native = SDL_CreateGPUGraphicsPipeline(m_device, &info);
    SDL_ReleaseGPUShader(m_device, *vs);
    SDL_ReleaseGPUShader(m_device, *fs);
    if (!native) return sdlError(std::format("cannot create pipeline '{}'", name));
    return static_cast<void*>(native);
}

Result<void*> SdlGpuDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    const ShaderBlob& blob = *desc.shader;
    const ShaderCode code = shaderCodeFor(blob, caps.shaderFormat);
    if (!code)
        return Error{
            std::format("shader '{}' has no {} code in this build", blob.entry, shaderFormatName(caps.shaderFormat))};
    const ShaderResources& r = blob.resources;
    SDL_GPUComputePipelineCreateInfo info{};
    info.code = code.data;
    info.code_size = code.size;
    info.entrypoint = blob.entry;
    info.format = m_shaderFormat;
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
    SDL_GPUComputePipeline* native = SDL_CreateGPUComputePipeline(m_device, &info);
    if (!native) return sdlError(std::format("cannot create compute pipeline '{}'", name));
    return static_cast<void*>(native);
}

// ------------------------------------------------------------------ presentation

Result<void> SdlGpuDevice::claimWindow(SDL_Window* window) {
    if (!SDL_ClaimWindowForGPUDevice(m_device, window)) return sdlError("cannot claim the window for the GPU");
    return {};
}

void SdlGpuDevice::releaseWindow(SDL_Window* window) { SDL_ReleaseWindowFromGPUDevice(m_device, window); }

bool SdlGpuDevice::supportsPresentMode(SDL_Window* window, PresentMode mode) const {
    return SDL_WindowSupportsGPUPresentMode(m_device, window, sdl::toSdl(mode));
}

bool SdlGpuDevice::supportsComposition(SDL_Window* window, SwapchainComposition composition) const {
    return SDL_WindowSupportsGPUSwapchainComposition(m_device, window, sdl::toSdl(composition));
}

bool SdlGpuDevice::setSwapchainParameters(SDL_Window* window, SwapchainComposition composition, PresentMode mode) {
    if (!SDL_SetGPUSwapchainParameters(m_device, window, sdl::toSdl(composition), sdl::toSdl(mode))) {
        log::error("gpu", "SDL_SetGPUSwapchainParameters (composition {}): {}", swapchainCompositionName(composition),
                   SDL_GetError());
        return false;
    }
    return true;
}

TextureFormat SdlGpuDevice::swapchainFormat(SDL_Window* window) const {
    return sdl::fromSdl(SDL_GetGPUSwapchainTextureFormat(m_device, window));
}

// ------------------------------------------------------------------ fences

bool SdlGpuDevice::isSignaled(void* fence) const {
    return SDL_QueryGPUFence(m_device, static_cast<SDL_GPUFence*>(fence));
}

void SdlGpuDevice::wait(void* fence) const {
    SDL_GPUFence* native = static_cast<SDL_GPUFence*>(fence);
    if (!SDL_WaitForGPUFences(m_device, true, &native, 1))
        log::error("gpu", "SDL_WaitForGPUFences: {}", SDL_GetError());
}

std::optional<double> SdlGpuDevice::gpuMilliseconds(void* fence) const {
    return metalFenceMilliseconds(static_cast<SDL_GPUFence*>(fence));
}

} // namespace detail
} // namespace quench::gpu
