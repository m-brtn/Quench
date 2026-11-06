#pragma once

// GPU resources. Each class owns one backend object and releases it when destroyed or reset; the backend defers the
// release until the GPU has finished with the object, so a resource may be dropped right after recording commands
// that use it. Resources are created by gpu::Device and must not outlive it. The *Ref types are non-owning views
// for bindings and pass targets (also of the swapchain texture); they must not outlive what they view.

#include <quench/gpu/Types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace quench::gpu {

namespace detail {

struct DeviceImpl;
struct Access;

enum class Kind : std::uint8_t { Buffer, Texture, Sampler, TransferBuffer, GraphicsPipeline, ComputePipeline, Fence };

void release(DeviceImpl* device, Kind kind, void* native) noexcept;

template <Kind K, class Info>
class Resource {
public:
    Resource() = default;
    ~Resource() { reset(); }
    Resource(Resource&& other) noexcept
        : m_device(std::exchange(other.m_device, nullptr)), m_native(std::exchange(other.m_native, nullptr)),
          m_info(other.m_info) {}
    Resource& operator=(Resource&& other) noexcept {
        if (this != &other) {
            reset();
            m_device = std::exchange(other.m_device, nullptr);
            m_native = std::exchange(other.m_native, nullptr);
            m_info = other.m_info;
        }
        return *this;
    }
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;

    void reset() noexcept {
        if (m_native) release(m_device, K, m_native);
        m_device = nullptr;
        m_native = nullptr;
    }
    explicit operator bool() const { return m_native != nullptr; }

protected:
    friend struct Access;
    DeviceImpl* m_device = nullptr;
    void* m_native = nullptr;
    Info m_info{};
};

struct NoInfo {};

// What a pass checks a pipeline against.
struct PipelineInfo {
    std::array<TextureFormat, 4> colorFormats{};
    std::uint32_t colorTargets = 0;
    TextureFormat depthFormat = TextureFormat::Invalid;
    std::uint32_t sampleCount = 1;
};

} // namespace detail

class Texture : public detail::Resource<detail::Kind::Texture, TextureDesc> {
public:
    // As created, without the debug name.
    const TextureDesc& desc() const { return m_info; }
};

class Buffer : public detail::Resource<detail::Kind::Buffer, BufferDesc> {
public:
    const BufferDesc& desc() const { return m_info; }
};

class Sampler : public detail::Resource<detail::Kind::Sampler, detail::NoInfo> {};

class GraphicsPipeline : public detail::Resource<detail::Kind::GraphicsPipeline, detail::PipelineInfo> {};

class ComputePipeline : public detail::Resource<detail::Kind::ComputePipeline, detail::NoInfo> {};

// CPU-visible staging memory for uploads (CopyPass::upload) and downloads (CopyPass::download).
class TransferBuffer : public detail::Resource<detail::Kind::TransferBuffer, TransferBufferDesc> {
public:
    const TransferBufferDesc& desc() const { return m_info; }
    // cycle: when the GPU may still be reading the previous contents, map fresh memory instead of overwriting them
    // (uploads written every frame). Unmap before the copy pass that uses the buffer.
    std::span<std::byte> map(bool cycle = false);
    void unmap();
};

// Signalled when the GPU has finished a submitted command buffer (CommandBuffer::submitWithFence).
class Fence : public detail::Resource<detail::Kind::Fence, detail::NoInfo> {};

struct TextureRef {
    void* native = nullptr;
    TextureDesc desc{};

    TextureRef() = default;
    TextureRef(const Texture& texture);
    explicit operator bool() const { return native != nullptr; }
};

struct BufferRef {
    void* native = nullptr;
    BufferDesc desc{};

    BufferRef() = default;
    BufferRef(const Buffer& buffer);
    explicit operator bool() const { return native != nullptr; }
};

struct SamplerRef {
    void* native = nullptr;

    SamplerRef() = default;
    SamplerRef(const Sampler& sampler);
    explicit operator bool() const { return native != nullptr; }
};

namespace detail {

// Backend access to the private parts of resources.
struct Access {
    template <class R>
    static void* native(const R& resource) {
        return resource.m_native;
    }
    template <class R>
    static DeviceImpl* device(const R& resource) {
        return resource.m_device;
    }
    template <class R>
    static auto& info(R& resource) {
        return resource.m_info;
    }
    template <class R>
    static void adopt(R& resource, DeviceImpl* device, void* native) {
        resource.reset();
        resource.m_device = device;
        resource.m_native = native;
    }
};

} // namespace detail

inline TextureRef::TextureRef(const Texture& texture) : native(detail::Access::native(texture)), desc(texture.desc()) {}
inline BufferRef::BufferRef(const Buffer& buffer) : native(detail::Access::native(buffer)), desc(buffer.desc()) {}
inline SamplerRef::SamplerRef(const Sampler& sampler) : native(detail::Access::native(sampler)) {}

} // namespace quench::gpu
