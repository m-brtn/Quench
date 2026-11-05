#include <quench/render/Scene.h>

#include "SceneImpl.h"

#include <quench/core/Log.h>
#include <quench/gpu/Device.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>

namespace quench::render {

using detail::InstanceEntry;
using detail::MaterialEntry;
using detail::MeshEntry;

namespace {

// Stale or null handles are programming errors: asserted, then ignored in release builds.
template <class Map, class Id>
auto* lookup(Map& map, Id id, [[maybe_unused]] const char* what) {
    auto* entry = map.get(id);
    QUENCH_ASSERT(entry, what);
    return entry;
}

} // namespace

Scene::Scene(gpu::Device& device) : m_impl(std::make_unique<detail::SceneImpl>()) { m_impl->device = &device; }
Scene::Scene(Scene&& other) noexcept = default;
Scene& Scene::operator=(Scene&& other) noexcept = default;
Scene::~Scene() = default;

gpu::Device& Scene::device() const { return *m_impl->device; }

Result<MeshId> Scene::addMesh(const MeshDesc& desc) {
    const std::string name(desc.name.empty() ? std::string_view("mesh") : desc.name);
    if (desc.vertices.empty() || desc.indices.empty() || desc.indices.size() % 3 != 0)
        return Error{std::format("mesh '{}': {} vertices, {} indices (need a triangle list)", name,
                                 desc.vertices.size(), desc.indices.size())};
    const auto vertexCount = std::uint32_t(desc.vertices.size());
    if (const auto bad =
            std::ranges::find_if(desc.indices, [vertexCount](std::uint32_t i) { return i >= vertexCount; });
        bad != desc.indices.end())
        return Error{std::format("mesh '{}': index {} out of range ({} vertices)", name, *bad, vertexCount)};

    MeshEntry mesh;
    glm::vec3 lo(desc.vertices[0].position), hi(lo);
    for (const Vertex& v : desc.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    mesh.center = (lo + hi) * 0.5f;
    for (const Vertex& v : desc.vertices) mesh.radius = std::max(mesh.radius, glm::length(v.position - mesh.center));

    gpu::Device& device = *m_impl->device;
    const std::span<const std::byte> vertexBytes = std::as_bytes(desc.vertices);
    const std::span<const std::byte> indexBytes = std::as_bytes(desc.indices);
    Result<gpu::Buffer> vb = device.createBuffer(
        {.usage = gpu::BufferUsage::Vertex, .size = std::uint32_t(vertexBytes.size()), .name = name});
    if (!vb) return Error{std::format("mesh '{}': {}", name, vb.error())};
    Result<gpu::Buffer> ib =
        device.createBuffer({.usage = gpu::BufferUsage::Index, .size = std::uint32_t(indexBytes.size()), .name = name});
    if (!ib) return Error{std::format("mesh '{}': {}", name, ib.error())};
    if (Result<void> r = device.upload(*vb, 0, vertexBytes); !r)
        return Error{std::format("mesh '{}': {}", name, r.error())};
    if (Result<void> r = device.upload(*ib, 0, indexBytes); !r)
        return Error{std::format("mesh '{}': {}", name, r.error())};
    mesh.vertices = std::move(*vb);
    mesh.indices = std::move(*ib);
    mesh.indexCount = std::uint32_t(desc.indices.size());
    return m_impl->meshes.add(std::move(mesh));
}

void Scene::removeMesh(MeshId mesh) {
    const MeshEntry* entry = lookup(m_impl->meshes, mesh, "removeMesh: stale or null mesh");
    if (!entry) return;
    QUENCH_ASSERT(entry->users == 0, "removeMesh: instances still use the mesh");
    m_impl->meshes.remove(mesh);
}

namespace {

Result<gpu::Texture> createTexture(gpu::Device& device, const TextureData& data) {
    const std::string name(data.name.empty() ? std::string_view("texture") : data.name);
    if (data.width == 0 || data.height == 0 || data.pixels.size() != std::size_t(data.width) * data.height * 4)
        return Error{std::format("texture '{}': {} x {} needs {} bytes of RGBA8, got {}", name, data.width, data.height,
                                 std::size_t(data.width) * data.height * 4, data.pixels.size())};
    gpu::TextureUsage usage = gpu::TextureUsage::Sampled;
    if (data.mipmaps) usage |= gpu::TextureUsage::ColorTarget;
    const gpu::TextureFormat format = data.srgb ? gpu::TextureFormat::Rgba8UnormSrgb : gpu::TextureFormat::Rgba8Unorm;
    Result<gpu::Texture> texture = device.createTexture({
        .format = format,
        .usage = usage,
        .width = data.width,
        .height = data.height,
        .mipLevels = data.mipmaps ? gpu::fullMipCount(data.width, data.height) : 1u,
        .name = name,
    });
    if (!texture) return Error{std::format("texture '{}': {}", name, texture.error())};
    if (Result<void> r = device.upload(gpu::TextureRegion{.texture = *texture}, data.pixels); !r)
        return Error{std::format("texture '{}': {}", name, r.error())};
    if (data.mipmaps && texture->desc().mipLevels > 1) {
        gpu::CommandBuffer commands = device.acquireCommandBuffer();
        if (!commands) return Error{std::format("texture '{}': no command buffer for the mipmaps", name)};
        commands.generateMipmaps(*texture);
        if (Result<void> r = commands.submit(); !r) return Error{std::format("texture '{}': {}", name, r.error())};
    }
    return std::move(*texture);
}

gpu::TextureFormat gpuFormat(assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::Rgba8: return gpu::TextureFormat::Rgba8Unorm;
    case assets::PixelFormat::Rgba8Srgb: return gpu::TextureFormat::Rgba8UnormSrgb;
    case assets::PixelFormat::Bc7: return gpu::TextureFormat::Bc7RgbaUnorm;
    case assets::PixelFormat::Bc7Srgb: return gpu::TextureFormat::Bc7RgbaUnormSrgb;
    case assets::PixelFormat::Astc4x4: return gpu::TextureFormat::Astc4x4Unorm;
    case assets::PixelFormat::Astc4x4Srgb: return gpu::TextureFormat::Astc4x4UnormSrgb;
    }
    return gpu::TextureFormat::Invalid;
}

// Every level through one transfer buffer and one copy pass.
Result<gpu::Texture> createTexture(gpu::Device& device, const assets::TextureImage& image, std::string_view label) {
    const std::string name(label.empty() ? std::string_view("texture") : label);
    const gpu::TextureFormat format = gpuFormat(image.format);
    if (image.width == 0 || image.height == 0 || image.mipLevels == 0 ||
        image.mipLevels > gpu::fullMipCount(image.width, image.height))
        return Error{
            std::format("texture '{}': {} x {} with {} levels", name, image.width, image.height, image.mipLevels)};
    if (image.levelOffset(image.mipLevels) != image.data.size())
        return Error{std::format("texture '{}': {} bytes of data, the levels need {}", name, image.data.size(),
                                 image.levelOffset(image.mipLevels))};
    if (image.data.size() > 0xffffffffu)
        return Error{std::format("texture '{}': {} bytes is too large to upload at once", name, image.data.size())};
    if (!device.supportsFormat(format, gpu::TextureType::Texture2D, gpu::TextureUsage::Sampled))
        return Error{std::format("texture '{}': the device cannot sample {}", name, gpu::formatInfo(format).name)};
    Result<gpu::Texture> texture = device.createTexture({
        .format = format,
        .usage = gpu::TextureUsage::Sampled,
        .width = image.width,
        .height = image.height,
        .mipLevels = image.mipLevels,
        .name = name,
    });
    if (!texture) return Error{std::format("texture '{}': {}", name, texture.error())};
    Result<gpu::TransferBuffer> staging = device.createTransferBuffer(
        {gpu::TransferDirection::Upload, std::uint32_t(image.data.size()), "texture upload"});
    if (!staging) return Error{std::format("texture '{}': {}", name, staging.error())};
    std::span<std::byte> mapped = staging->map();
    if (mapped.size() < image.data.size()) return Error{std::format("texture '{}': cannot map the upload", name)};
    std::memcpy(mapped.data(), image.data.data(), image.data.size());
    staging->unmap();
    gpu::CommandBuffer commands = device.acquireCommandBuffer();
    if (!commands) return Error{std::format("texture '{}': no command buffer for the upload", name)};
    {
        gpu::CopyPass copy = commands.beginCopyPass();
        for (std::uint32_t level = 0; level < image.mipLevels; ++level)
            copy.upload(*staging, std::uint32_t(image.levelOffset(level)),
                        gpu::TextureRegion{.texture = *texture, .mipLevel = level});
    }
    if (Result<void> r = commands.submit(); !r) return Error{std::format("texture '{}': {}", name, r.error())};
    return std::move(*texture);
}

} // namespace

Result<TextureId> Scene::addTexture(const TextureData& data) {
    Result<gpu::Texture> texture = createTexture(*m_impl->device, data);
    if (!texture) return Error{texture.error()};
    return m_impl->textures.add({std::move(*texture)});
}

Result<TextureId> Scene::addTexture(const assets::TextureImage& image, std::string_view name) {
    Result<gpu::Texture> texture = createTexture(*m_impl->device, image, name);
    if (!texture) return Error{texture.error()};
    return m_impl->textures.add({std::move(*texture)});
}

TextureId Scene::addEmptyTexture() { return m_impl->textures.add({}); }

Result<void> Scene::setTexture(TextureId texture, const TextureData& data) {
    detail::TextureEntry* entry = lookup(m_impl->textures, texture, "setTexture: stale or null texture");
    if (!entry) return Error{"setTexture: stale or null texture"};
    Result<gpu::Texture> created = createTexture(*m_impl->device, data);
    if (!created) return Error{created.error()};
    entry->texture = std::move(*created);
    return {};
}

Result<void> Scene::setTexture(TextureId texture, const assets::TextureImage& image, std::string_view name) {
    detail::TextureEntry* entry = lookup(m_impl->textures, texture, "setTexture: stale or null texture");
    if (!entry) return Error{"setTexture: stale or null texture"};
    Result<gpu::Texture> created = createTexture(*m_impl->device, image, name);
    if (!created) return Error{created.error()};
    entry->texture = std::move(*created);
    return {};
}

void Scene::removeTexture(TextureId texture) {
    const bool removed = m_impl->textures.remove(texture);
    QUENCH_ASSERT(removed, "removeTexture: stale or null handle");
    (void)removed;
}

MaterialId Scene::addMaterial(const MaterialDesc& desc) {
    MaterialEntry entry{desc, 0};
    entry.desc.name = {};
    return m_impl->materials.add(std::move(entry));
}

void Scene::setMaterial(MaterialId material, const MaterialDesc& desc) {
    MaterialEntry* entry = lookup(m_impl->materials, material, "setMaterial: stale or null material");
    if (!entry) return;
    entry->desc = desc;
    entry->desc.name = {};
}

void Scene::removeMaterial(MaterialId material) {
    const MaterialEntry* entry = lookup(m_impl->materials, material, "removeMaterial: stale or null material");
    if (!entry) return;
    QUENCH_ASSERT(entry->users == 0, "removeMaterial: instances still use the material");
    m_impl->materials.remove(material);
}

InstanceId Scene::addInstance(const InstanceDesc& desc) {
    MeshEntry* mesh = lookup(m_impl->meshes, desc.mesh, "addInstance: stale or null mesh");
    MaterialEntry* material = lookup(m_impl->materials, desc.material, "addInstance: stale or null material");
    if (!mesh || !material) return {};
    mesh->users++;
    material->users++;
    return m_impl->instances.add({desc, desc.transform, false});
}

void Scene::setTransform(InstanceId instance, const glm::mat4& transform, bool teleport) {
    InstanceEntry* entry = lookup(m_impl->instances, instance, "setTransform: stale or null instance");
    if (!entry) return;
    if (teleport) {
        entry->previous = transform;
        entry->moved = false;
    } else if (!entry->moved) {
        entry->previous = entry->desc.transform;
        entry->moved = true;
    }
    entry->desc.transform = transform;
}

void Scene::setVisible(InstanceId instance, bool visible) {
    if (InstanceEntry* entry = lookup(m_impl->instances, instance, "setVisible: stale or null instance"))
        entry->desc.visible = visible;
}

void Scene::setInstanceMaterial(InstanceId instance, MaterialId material) {
    InstanceEntry* entry = lookup(m_impl->instances, instance, "setInstanceMaterial: stale or null instance");
    MaterialEntry* next = lookup(m_impl->materials, material, "setInstanceMaterial: stale or null material");
    if (!entry || !next) return;
    next->users++;
    if (MaterialEntry* old = m_impl->materials.get(entry->desc.material)) old->users--;
    entry->desc.material = material;
}

void Scene::setInstanceColor(InstanceId instance, const glm::vec4& color, const glm::vec4& custom) {
    InstanceEntry* entry = lookup(m_impl->instances, instance, "setInstanceColor: stale or null instance");
    if (!entry) return;
    entry->desc.color = color;
    entry->desc.custom = custom;
}

void Scene::removeInstance(InstanceId instance) {
    const InstanceEntry* entry = lookup(m_impl->instances, instance, "removeInstance: stale or null instance");
    if (!entry) return;
    if (MeshEntry* mesh = m_impl->meshes.get(entry->desc.mesh)) mesh->users--;
    if (MaterialEntry* material = m_impl->materials.get(entry->desc.material)) material->users--;
    m_impl->instances.remove(instance);
}

LightId Scene::addLight(const LightDesc& desc) { return m_impl->lights.add({desc}); }

void Scene::setLight(LightId light, const LightDesc& desc) {
    if (detail::LightEntry* entry = lookup(m_impl->lights, light, "setLight: stale or null light")) entry->desc = desc;
}

void Scene::removeLight(LightId light) {
    const bool removed = m_impl->lights.remove(light);
    QUENCH_ASSERT(removed, "removeLight: stale or null handle");
    (void)removed;
}

void Scene::setEnvironment(const Environment& environment) { m_impl->environment = environment; }
const Environment& Scene::environment() const { return m_impl->environment; }

bool Scene::contains(MeshId mesh) const { return m_impl->meshes.get(mesh) != nullptr; }
bool Scene::contains(TextureId texture) const { return m_impl->textures.get(texture) != nullptr; }
bool Scene::contains(MaterialId material) const { return m_impl->materials.get(material) != nullptr; }
bool Scene::contains(InstanceId instance) const { return m_impl->instances.get(instance) != nullptr; }
bool Scene::contains(LightId light) const { return m_impl->lights.get(light) != nullptr; }
std::uint32_t Scene::meshUsers(MeshId mesh) const {
    const MeshEntry* entry = m_impl->meshes.get(mesh);
    return entry ? entry->users : 0;
}
std::uint32_t Scene::materialUsers(MaterialId material) const {
    const MaterialEntry* entry = m_impl->materials.get(material);
    return entry ? entry->users : 0;
}
std::uint32_t Scene::meshCount() const { return m_impl->meshes.size(); }
std::uint32_t Scene::textureCount() const { return m_impl->textures.size(); }
std::uint32_t Scene::materialCount() const { return m_impl->materials.size(); }
std::uint32_t Scene::instanceCount() const { return m_impl->instances.size(); }
std::uint32_t Scene::lightCount() const { return m_impl->lights.size(); }

} // namespace quench::render
