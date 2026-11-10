// gpu::Device and the resources: argument checks and the parts every backend shares; the work is the backend's
// (src/gpu/Backend.h).

#include "gpu/Backend.h"
#include "gpu/ShaderReload.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>
#include <quench/core/Profile.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

namespace quench::gpu {

const char* backendName(Backend backend) {
    switch (backend) {
    case Backend::Default: return "default";
    case Backend::SdlGpu: return "sdl";
    case Backend::Metal: return "metal";
    case Backend::WebGpu: return "webgpu";
    }
    return "unknown";
}

bool backendAvailable(Backend backend) {
    switch (backend) {
    case Backend::Default: return true;
    case Backend::SdlGpu: return !QUENCH_PLATFORM_WEB;
    case Backend::Metal: return QUENCH_PLATFORM_APPLE;
    case Backend::WebGpu: return QUENCH_PLATFORM_WEB;
    }
    return false;
}

Backend defaultBackend() {
#if QUENCH_PLATFORM_WEB
    return Backend::WebGpu;
#else
    return QUENCH_PLATFORM_APPLE ? Backend::Metal : Backend::SdlGpu;
#endif
}

namespace detail {

namespace {
std::atomic<bool> g_nativeDeviceFails{false};
} // namespace

void setNativeDeviceFailsForTesting(bool fails) { g_nativeDeviceFails = fails; }

DeviceImpl::DeviceImpl() = default;
DeviceImpl::~DeviceImpl() = default;

void releaseSharedFence(DeviceImpl* device, SharedFence* fence) noexcept {
    if (fence->handles.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    if (void* native = fence->native.load(std::memory_order_acquire)) device->release(Kind::Fence, native);
    delete fence;
}

void release(DeviceImpl* device, Kind kind, void* native) noexcept {
    if (kind == Kind::GraphicsPipeline || kind == Kind::ComputePipeline)
        removePipeline(*device, kind, native);
    else if (kind == Kind::Fence)
        releaseSharedFence(device, static_cast<SharedFence*>(native));
    else
        device->release(kind, native);
    device->live.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace detail

namespace {

using detail::Access;
using detail::DeviceImpl;

bool validSampleCount(std::uint32_t count) { return count == 1 || count == 2 || count == 4 || count == 8; }

template <class R>
R adopt(DeviceImpl* device, void* native) {
    R resource;
    Access::adopt(resource, device, native);
    device->live.fetch_add(1, std::memory_order_relaxed);
    return resource;
}

Backend chooseBackend(const DeviceDesc& desc) {
    Backend requested = desc.backend;
#if QUENCH_PLATFORM_WEB
    // A web build has the one backend.
    if (requested != Backend::Default && requested != Backend::WebGpu)
        log::warning("gpu", "a web build has only the WebGPU backend, not {}", backendName(requested));
    return Backend::WebGpu;
#else
    if (requested == Backend::WebGpu) {
        log::warning("gpu", "the WebGPU backend exists only in web builds: {}", backendName(defaultBackend()));
        requested = defaultBackend();
    }
    if (requested == Backend::Default) {
        std::string_view driver = desc.driver;
        if (const char* env = std::getenv("SDL_GPU_DRIVER"); driver.empty() && env) driver = env;
        requested = !driver.empty() && driver != "metal" ? Backend::SdlGpu : defaultBackend();
        if (const char* env = std::getenv("QUENCH_GPU_BACKEND"); env && *env) {
            const std::string_view value(env);
            if (value == "metal")
                requested = Backend::Metal;
            else if (value == "sdl")
                requested = Backend::SdlGpu;
            else
                log::warning("gpu", "QUENCH_GPU_BACKEND={} is not sdl or metal: {}", value, backendName(requested));
        }
    }
    if (requested == Backend::Metal && !QUENCH_PLATFORM_APPLE) {
        log::warning("gpu", "the native Metal backend exists only on Apple platforms: SDL GPU");
        requested = Backend::SdlGpu;
    }
    return requested;
#endif
}

} // namespace

// ------------------------------------------------------------------ TransferBuffer

std::span<std::byte> TransferBuffer::map(bool cycle) {
    QUENCH_ASSERT(m_native, "map() on an empty TransferBuffer");
    return m_device->map(m_native, m_info.size, cycle);
}

void TransferBuffer::unmap() {
    QUENCH_ASSERT(m_native, "unmap() on an empty TransferBuffer");
    m_device->unmap(m_native);
}

// ------------------------------------------------------------------ Device

Result<Device> Device::create(const DeviceDesc& desc) {
    Backend backend = chooseBackend(desc);
    Result<std::unique_ptr<DeviceImpl>> impl = Error{"no backend"};
    std::string nativeError;
#if QUENCH_PLATFORM_APPLE
    if (backend == Backend::Metal) {
        if (detail::g_nativeDeviceFails)
            impl = Error{"failing for a test"};
        else
            impl = detail::createMetalDevice(desc);
        if (!impl) {
            nativeError = impl.error();
            log::warning("gpu", "the native Metal backend failed to start ({}): SDL GPU instead", nativeError);
            backend = Backend::SdlGpu;
        }
    }
#endif
#if QUENCH_PLATFORM_WEB
    impl = detail::createWebGpuDevice(desc);
#else
    if (backend == Backend::SdlGpu) impl = detail::createSdlGpuDevice(desc);
#endif
    if (!impl)
        return Error{nativeError.empty() ? impl.error()
                                         : std::format("native Metal: {}; SDL GPU: {}", nativeError, impl.error())};

    Device device;
    device.m_impl = std::move(*impl);
    Caps& caps = device.m_impl->caps;
    caps.backend = backend;
    caps.bcTextures =
        device.supportsFormat(TextureFormat::Bc1RgbaUnorm, TextureType::Texture2D, TextureUsage::Sampled) &&
        device.supportsFormat(TextureFormat::Bc7RgbaUnorm, TextureType::Texture2D, TextureUsage::Sampled);
    caps.astcTextures =
        device.supportsFormat(TextureFormat::Astc4x4Unorm, TextureType::Texture2D, TextureUsage::Sampled);
    caps.etc2Textures =
        device.supportsFormat(TextureFormat::Etc2Rgb8Unorm, TextureType::Texture2D, TextureUsage::Sampled) &&
        device.supportsFormat(TextureFormat::Etc2Rgba8Unorm, TextureType::Texture2D, TextureUsage::Sampled);
    log::info("gpu", "backend {}, driver {}, device '{}', shaders {}", backendName(caps.backend), caps.driver,
              caps.deviceName, shaderFormatName(caps.shaderFormat));
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
    impl.waitIdle();
    for (const detail::WindowEntry& entry : impl.windows) impl.releaseWindow(entry.window);
    impl.windows.clear();
    const std::int64_t live = impl.live.load();
    if (live != 0) log::error("gpu", "{} GPU resources outlive their device", live);
    QUENCH_ASSERT(live == 0, "GPU resources outlive their device (release them before the device)");
    m_impl.reset();
}

const Caps& Device::caps() const { return m_impl->caps; }

bool Device::supportsFormat(TextureFormat format, TextureType type, TextureUsage usage) const {
    return m_impl->supportsFormat(format, type, usage);
}

bool Device::supportsSampleCount(TextureFormat format, std::uint32_t sampleCount) const {
    if (!validSampleCount(sampleCount)) return false;
    return sampleCount == 1 || m_impl->supportsSampleCount(format, sampleCount);
}

std::int64_t Device::liveResources() const { return m_impl->live.load(); }

Result<Texture> Device::createTexture(const TextureDesc& desc) {
    QUENCH_ASSERT(desc.format != TextureFormat::Invalid && desc.format < TextureFormat::Count, "invalid format");
    QUENCH_ASSERT(validSampleCount(desc.sampleCount), "sample count must be 1, 2, 4 or 8");
    QUENCH_ASSERT(desc.width > 0 && desc.height > 0 && desc.layers > 0 && desc.mipLevels > 0, "empty texture");
    const std::string what = desc.name.empty() ? std::string("texture") : std::format("texture '{}'", desc.name);
    if (!supportsFormat(desc.format, desc.type, desc.usage))
        return Error{std::format("cannot create {}: {} does not support this type and usage on {}", what,
                                 formatInfo(desc.format).name, m_impl->caps.driver)};
    if (!supportsSampleCount(desc.format, desc.sampleCount))
        return Error{std::format("cannot create {}: {} does not support {} samples on {}", what,
                                 formatInfo(desc.format).name, desc.sampleCount, m_impl->caps.driver)};
    Result<void*> native = m_impl->createTexture(desc);
    if (!native) return Error{native.error()};
    Texture texture = adopt<Texture>(m_impl.get(), *native);
    Access::info(texture) = desc;
    Access::info(texture).name = {};
    return texture;
}

Result<Buffer> Device::createBuffer(const BufferDesc& desc) {
    QUENCH_ASSERT(desc.size > 0, "empty buffer");
    Result<void*> native = m_impl->createBuffer(desc);
    if (!native) return Error{native.error()};
    Buffer buffer = adopt<Buffer>(m_impl.get(), *native);
    Access::info(buffer) = desc;
    Access::info(buffer).name = {};
    return buffer;
}

Result<Sampler> Device::createSampler(const SamplerDesc& desc) {
    Result<void*> native = m_impl->createSampler(desc);
    if (!native) return Error{native.error()};
    return adopt<Sampler>(m_impl.get(), *native);
}

Result<TransferBuffer> Device::createTransferBuffer(const TransferBufferDesc& desc) {
    QUENCH_ASSERT(desc.size > 0, "empty transfer buffer");
    Result<void*> native = m_impl->createTransferBuffer(desc);
    if (!native) return Error{native.error()};
    TransferBuffer buffer = adopt<TransferBuffer>(m_impl.get(), *native);
    Access::info(buffer) = desc;
    Access::info(buffer).name = {};
    return buffer;
}

Result<GraphicsPipeline> Device::createPipeline(const GraphicsPipelineDesc& desc) {
    QUENCH_ASSERT(desc.vertexShader && desc.vertexShader->stage == ShaderStage::Vertex, "vertex shader missing");
    QUENCH_ASSERT(desc.fragmentShader && desc.fragmentShader->stage == ShaderStage::Fragment,
                  "fragment shader missing");
    QUENCH_ASSERT(desc.colorTargets.size() <= 4, "at most 4 color targets");
    QUENCH_ASSERT(validSampleCount(desc.sampleCount), "sample count must be 1, 2, 4 or 8");
    QUENCH_ASSERT(desc.vertexBuffers.size() <= 16 && desc.vertexAttributes.size() <= 16, "too many vertex inputs");
    QUENCH_ASSERT(desc.depthFormat != TextureFormat::Invalid || (!desc.depth.test && !desc.depth.write),
                  "depth test or write without a depth format");
    detail::PipelineInfo info;
    for (std::size_t i = 0; i < desc.colorTargets.size(); ++i) {
        QUENCH_ASSERT(desc.colorTargets[i].format != TextureFormat::Invalid, "color target without a format");
        info.colorFormats[i] = desc.colorTargets[i].format;
    }
    info.colorTargets = std::uint32_t(desc.colorTargets.size());
    info.depthFormat = desc.depthFormat;
    info.sampleCount = desc.sampleCount;
    auto slot = std::make_unique<detail::PipelineSlot>();
    slot->vertexBuffers.assign(desc.vertexBuffers.begin(), desc.vertexBuffers.end());
    slot->vertexAttributes.assign(desc.vertexAttributes.begin(), desc.vertexAttributes.end());
    slot->colorTargets.assign(desc.colorTargets.begin(), desc.colorTargets.end());
    slot->name = desc.name;
    slot->graphics = desc;
    slot->graphics.vertexBuffers = slot->vertexBuffers;
    slot->graphics.vertexAttributes = slot->vertexAttributes;
    slot->graphics.colorTargets = slot->colorTargets;
    slot->graphics.name = slot->name;
    Result<void*> native = detail::addPipeline(*m_impl, std::move(slot));
    if (!native) return Error{native.error()};
    GraphicsPipeline pipeline = adopt<GraphicsPipeline>(m_impl.get(), *native);
    Access::info(pipeline) = info;
    return pipeline;
}

Result<ComputePipeline> Device::createComputePipeline(const ComputePipelineDesc& desc) {
    QUENCH_ASSERT(desc.shader && desc.shader->stage == ShaderStage::Compute, "compute shader missing");
    auto slot = std::make_unique<detail::PipelineSlot>();
    slot->compute = true;
    slot->name = desc.name;
    slot->computeDesc = desc;
    slot->computeDesc.name = slot->name;
    Result<void*> native = detail::addPipeline(*m_impl, std::move(slot));
    if (!native) return Error{native.error()};
    return adopt<ComputePipeline>(m_impl.get(), *native);
}

Result<AccelerationStructure> Device::createAccelerationStructure(const AccelerationStructureDesc& desc) {
    if (!m_impl->caps.rayTracing) return Error{std::format("no ray tracing on {}", m_impl->caps.deviceName)};
    QUENCH_ASSERT(desc.compactedSize == 0 || (desc.level == AccelerationStructureLevel::Bottom && !desc.allowRefit),
                  "compactedSize is for a bottom level that receives a compacted copy, without refits");
    QUENCH_ASSERT(desc.level == AccelerationStructureLevel::Top
                      ? desc.instanceCapacity > 0
                      : desc.compactedSize > 0 || (desc.vertexCount > 0 && desc.triangleCount > 0),
                  "empty acceleration structure");
    Result<void*> native = m_impl->createAccelerationStructure(desc);
    if (!native) return Error{native.error()};
    AccelerationStructure structure = adopt<AccelerationStructure>(m_impl.get(), *native);
    Access::info(structure).desc = desc;
    Access::info(structure).desc.name = {};
    Access::info(structure).bytes = m_impl->accelerationStructureBytes(*native);
    return structure;
}

std::optional<std::uint32_t> Device::compactedSize(const AccelerationStructure& bottomLevel) const {
    QUENCH_ASSERT(bottomLevel && bottomLevel.desc().level == AccelerationStructureLevel::Bottom,
                  "compactedSize needs a bottom level");
    return m_impl->compactedSize(Access::native(bottomLevel));
}

Result<Upscaler> Device::createUpscaler(const UpscalerDesc& desc) {
    const Caps& caps = m_impl->caps;
    const bool spatial = desc.type == UpscalerType::Spatial, temporal = desc.type == UpscalerType::Temporal;
    if (spatial ? !caps.spatialUpscaling : temporal ? !caps.temporalUpscaling : !caps.denoisedUpscaling)
        return Error{std::format("no {} upscaling on {}",
                                 spatial    ? "spatial"
                                 : temporal ? "temporal"
                                            : "temporal denoised",
                                 caps.deviceName)};
    QUENCH_ASSERT(desc.inputWidth > 0 && desc.inputHeight > 0 && desc.outputWidth >= desc.inputWidth &&
                      desc.outputHeight >= desc.inputHeight,
                  "an upscaler's output is at least its input");
    Result<void*> native = m_impl->createUpscaler(desc);
    if (!native) return Error{native.error()};
    Upscaler upscaler = adopt<Upscaler>(m_impl.get(), *native);
    Access::info(upscaler) = desc;
    Access::info(upscaler).name = {};
    return upscaler;
}

// ------------------------------------------------------------------ convenience transfers

Result<void> Device::upload(const BufferRef& destination, std::uint32_t offset, std::span<const std::byte> data) {
    QUENCH_PROFILE_ZONE("gpu.upload_buffer");
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
    QUENCH_PROFILE_ZONE("gpu.upload_texture");
    QUENCH_ASSERT(destination.texture, "upload to an empty texture");
    const std::uint64_t size = levelByteSize(destination.texture.desc.format, detail::regionWidth(destination),
                                             detail::regionHeight(destination), destination.depth);
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
    const std::uint64_t size = levelByteSize(source.texture.desc.format, detail::regionWidth(source),
                                             detail::regionHeight(source), source.depth);
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
    if (Result<void> claimed = m_impl->claimWindow(window); !claimed) return claimed;
    m_impl->windows.push_back({window});
    setPresentMode(window, mode);
    return {};
}

void Device::releaseWindow(SDL_Window* window) {
    auto it = std::find_if(m_impl->windows.begin(), m_impl->windows.end(),
                           [window](const detail::WindowEntry& w) { return w.window == window; });
    QUENCH_ASSERT(it != m_impl->windows.end(), "releaseWindow on a window that was not claimed");
    if (it == m_impl->windows.end()) return;
    m_impl->waitIdle();
    m_impl->releaseWindow(window);
    m_impl->windows.erase(it);
}

bool Device::supportsPresentMode(SDL_Window* window, PresentMode mode) const {
    return m_impl->supportsPresentMode(window, mode);
}

PresentMode Device::setPresentMode(SDL_Window* window, PresentMode mode) {
    detail::WindowEntry* entry = m_impl->find(window);
    QUENCH_ASSERT(entry, "setPresentMode on a window that was not claimed");
    if (!entry) return PresentMode::Vsync;
    if (!supportsPresentMode(window, mode)) mode = PresentMode::Vsync;
    if (!m_impl->setSwapchainParameters(window, entry->composition, mode)) return entry->presentMode;
    entry->presentMode = mode;
    return mode;
}

PresentMode Device::presentMode(SDL_Window* window) const {
    const detail::WindowEntry* entry = m_impl->find(window);
    return entry ? entry->presentMode : PresentMode::Vsync;
}

bool Device::supportsComposition(SDL_Window* window, SwapchainComposition composition) const {
    return m_impl->supportsComposition(window, composition);
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
    m_impl->waitIdle();
    if (!m_impl->setSwapchainParameters(window, composition, entry->presentMode)) return entry->composition;
    entry->composition = composition;
    return composition;
}

SwapchainComposition Device::composition(SDL_Window* window) const {
    const detail::WindowEntry* entry = m_impl->find(window);
    return entry ? entry->composition : SwapchainComposition::Sdr;
}

TextureFormat Device::swapchainFormat(SDL_Window* window) const { return m_impl->swapchainFormat(window); }

// ------------------------------------------------------------------ fences

namespace {
// The backend's fence behind a Fence: null before its command buffer is submitted and after a cancel.
void* backendFence(const Fence& fence) {
    return static_cast<detail::SharedFence*>(Access::native(fence))->native.load(std::memory_order_acquire);
}
} // namespace

bool Device::isSignaled(const Fence& fence) const {
    QUENCH_ASSERT(fence, "isSignaled on an empty fence");
    void* native = backendFence(fence);
    return native && m_impl->isSignaled(native);
}

void Device::wait(const Fence& fence) const {
    QUENCH_ASSERT(fence, "wait on an empty fence");
    void* native = backendFence(fence);
    QUENCH_ASSERT(native || static_cast<detail::SharedFence*>(Access::native(fence))->cancelled,
                  "wait on the completion fence of a command buffer that was not submitted");
    if (native) m_impl->wait(native);
}

std::optional<double> Device::gpuMilliseconds(const Fence& fence) const {
    QUENCH_ASSERT(fence, "gpuMilliseconds on an empty fence");
    if (!m_impl->caps.gpuTiming || !isSignaled(fence)) return std::nullopt;
    return m_impl->gpuMilliseconds(backendFence(fence));
}

std::vector<GpuTiming> Device::gpuTimings(const Fence& fence) const {
    QUENCH_ASSERT(fence, "gpuTimings on an empty fence");
    if (!m_impl->caps.gpuPassTiming || !isSignaled(fence)) return {};
    return m_impl->gpuTimings(backendFence(fence));
}

void Device::setPassTiming(bool enabled) { m_impl->passTiming = enabled; }
bool Device::passTiming() const { return m_impl->passTiming && m_impl->caps.gpuPassTiming; }

void Device::waitIdle() { m_impl->waitIdle(); }

} // namespace quench::gpu
