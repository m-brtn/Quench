#include <quench/render/Scene.h>

#include "SceneImpl.h"

#include <quench/core/Log.h>
#include <quench/gpu/Device.h>

#include <glm/geometric.hpp>

#include <algorithm>
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

Result<TextureId> Scene::addTexture(const TextureData& data) {
    const std::string name(data.name.empty() ? std::string_view("texture") : data.name);
    if (data.width == 0 || data.height == 0 || data.pixels.size() != std::size_t(data.width) * data.height * 4)
        return Error{std::format("texture '{}': {} x {} needs {} bytes of RGBA8, got {}", name, data.width, data.height,
                                 std::size_t(data.width) * data.height * 4, data.pixels.size())};
    gpu::Device& device = *m_impl->device;
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
    return m_impl->textures.add({std::move(*texture)});
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
std::uint32_t Scene::meshCount() const { return m_impl->meshes.size(); }
std::uint32_t Scene::materialCount() const { return m_impl->materials.size(); }
std::uint32_t Scene::instanceCount() const { return m_impl->instances.size(); }
std::uint32_t Scene::lightCount() const { return m_impl->lights.size(); }

} // namespace quench::render
