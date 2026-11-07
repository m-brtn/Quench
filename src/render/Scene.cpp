#include <quench/render/Scene.h>

#include "SceneImpl.h"

#include <quench/core/Log.h>
#include <quench/gpu/Device.h>
#include <quench/render/MeshBuilder.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace quench::render {

using detail::InstanceEntry;
using detail::MaterialEntry;
using detail::MeshEntry;

namespace {

constexpr std::uint32_t kMaxJoints = 65536; // SkinVertex::joints are 16 bits
constexpr std::uint32_t kMaxMorphTargets = 256;

// Stale or null handles are programming errors: asserted, then ignored in release builds.
template <class Map, class Id>
auto* lookup(Map& map, Id id, [[maybe_unused]] const char* what) {
    auto* entry = map.get(id);
    QUENCH_ASSERT(entry, what);
    return entry;
}

} // namespace

Scene::Scene(gpu::Device& device) : m_impl(std::make_unique<detail::SceneImpl>()) {
    static std::atomic<std::uint64_t> serials{0};
    m_impl->device = &device;
    m_impl->serial = ++serials;
}
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
    const bool skinned = !desc.skin.empty();
    if (skinned && desc.skin.size() != desc.vertices.size())
        return Error{std::format("mesh '{}': {} skin entries for {} vertices", name, desc.skin.size(), vertexCount)};
    if (skinned && (desc.jointCount == 0 || desc.jointCount > kMaxJoints))
        return Error{std::format("mesh '{}': {} joints (1..{})", name, desc.jointCount, kMaxJoints)};
    if (desc.morphTargetCount > kMaxMorphTargets ||
        desc.morphs.size() != std::size_t(desc.morphTargetCount) * desc.vertices.size())
        return Error{std::format("mesh '{}': {} morph offsets for {} targets of {} vertices (at most {} targets)", name,
                                 desc.morphs.size(), desc.morphTargetCount, vertexCount, kMaxMorphTargets)};
    const bool morphed = desc.morphTargetCount > 0;

    MeshEntry mesh;
    glm::vec3 lo(desc.vertices[0].position), hi(lo);
    for (const Vertex& v : desc.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    mesh.center = (lo + hi) * 0.5f;
    for (const Vertex& v : desc.vertices) mesh.radius = std::max(mesh.radius, glm::length(v.position - mesh.center));

    // Joints of zero weight are moved to joint 0, so the shader reads only valid matrices.
    std::vector<SkinVertex> skin(desc.skin.begin(), desc.skin.end());
    if (skinned) {
        std::vector<glm::vec3> jointLo(desc.jointCount, glm::vec3(1e30f)), jointHi(desc.jointCount, glm::vec3(-1e30f));
        for (std::size_t v = 0; v < skin.size(); ++v)
            for (int k = 0; k < 4; ++k) {
                if (skin[v].weights[std::size_t(k)] == 0) {
                    skin[v].joints[std::size_t(k)] = 0;
                    continue;
                }
                const std::uint32_t joint = skin[v].joints[std::size_t(k)];
                if (joint >= desc.jointCount)
                    return Error{
                        std::format("mesh '{}': vertex {} uses joint {} of {}", name, v, joint, desc.jointCount)};
                jointLo[joint] = glm::min(jointLo[joint], desc.vertices[v].position);
                jointHi[joint] = glm::max(jointHi[joint], desc.vertices[v].position);
            }
        mesh.jointBounds.assign(desc.jointCount, glm::vec4(0.0f, 0.0f, 0.0f, -1.0f));
        for (std::size_t v = 0; v < skin.size(); ++v)
            for (int k = 0; k < 4; ++k) {
                if (skin[v].weights[std::size_t(k)] == 0) continue;
                const std::uint32_t joint = skin[v].joints[std::size_t(k)];
                glm::vec4& b = mesh.jointBounds[joint];
                const glm::vec3 c = (jointLo[joint] + jointHi[joint]) * 0.5f;
                b = glm::vec4(c, std::max(b.w, glm::length(desc.vertices[v].position - c)));
            }
        mesh.jointCount = desc.jointCount;
    }
    mesh.vertexCount = vertexCount;

    gpu::Device& device = *m_impl->device;
    if (morphed) {
        std::vector<glm::vec4> rows;
        rows.reserve(desc.morphs.size() * 3);
        mesh.morphReach.assign(desc.morphTargetCount, 0.0f);
        for (std::size_t i = 0; i < desc.morphs.size(); ++i) {
            const MorphVertex& m = desc.morphs[i];
            rows.push_back(glm::vec4(m.position, m.normal.x));
            rows.push_back(glm::vec4(m.normal.y, m.normal.z, m.tangent.x, m.tangent.y));
            rows.push_back(glm::vec4(m.tangent.z, 0.0f, 0.0f, 0.0f));
            float& reach = mesh.morphReach[i / vertexCount];
            reach = std::max(reach, glm::length(m.position));
        }
        const std::span<const std::byte> morphBytes = std::as_bytes(std::span(rows));
        Result<gpu::Buffer> mb = device.createBuffer(
            {.usage = gpu::BufferUsage::ComputeStorageRead, .size = std::uint32_t(morphBytes.size()), .name = name});
        if (!mb) return Error{std::format("mesh '{}': {}", name, mb.error())};
        if (Result<void> r = device.upload(*mb, 0, morphBytes); !r)
            return Error{std::format("mesh '{}': {}", name, r.error())};
        mesh.morphs = std::move(*mb);
        mesh.morphTargetCount = desc.morphTargetCount;
    }
    const std::span<const std::byte> vertexBytes = std::as_bytes(desc.vertices);
    const std::span<const std::byte> indexBytes = std::as_bytes(desc.indices);
    const gpu::BufferUsage vertexUsage =
        skinned || morphed ? gpu::BufferUsage::Vertex | gpu::BufferUsage::ComputeStorageRead : gpu::BufferUsage::Vertex;
    Result<gpu::Buffer> vb =
        device.createBuffer({.usage = vertexUsage, .size = std::uint32_t(vertexBytes.size()), .name = name});
    if (!vb) return Error{std::format("mesh '{}': {}", name, vb.error())};
    if (skinned) {
        const std::span<const std::byte> skinBytes = std::as_bytes(std::span(skin));
        Result<gpu::Buffer> sb = device.createBuffer(
            {.usage = gpu::BufferUsage::ComputeStorageRead, .size = std::uint32_t(skinBytes.size()), .name = name});
        if (!sb) return Error{std::format("mesh '{}': {}", name, sb.error())};
        if (Result<void> r = device.upload(*sb, 0, skinBytes); !r)
            return Error{std::format("mesh '{}': {}", name, r.error())};
        mesh.skin = std::move(*sb);
    }
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
    case assets::PixelFormat::Etc2Rgb8: return gpu::TextureFormat::Etc2Rgb8Unorm;
    case assets::PixelFormat::Etc2Rgb8Srgb: return gpu::TextureFormat::Etc2Rgb8UnormSrgb;
    case assets::PixelFormat::Etc2Rgba8: return gpu::TextureFormat::Etc2Rgba8Unorm;
    case assets::PixelFormat::Etc2Rgba8Srgb: return gpu::TextureFormat::Etc2Rgba8UnormSrgb;
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

namespace {

// With linear skinning every vertex is a weighted mean of its joints' images of it, so it lies in the hull of the
// joints' transformed bind-pose spheres. Dual quaternion skinning moves a vertex by a blend of rigid motions, which can
// swing it out of that hull along an arc around the joint: a margin of the largest sphere covers it for vertices
// blended near their joints, as in a skeleton (not a proof for any palette). Blend shapes move a vertex before
// skinning by at most the weighted sum of their largest offsets, which the joints scale by at most their largest
// axis.
void updatePoseBounds(InstanceEntry& entry, const MeshEntry& mesh) {
    entry.center = mesh.center;
    entry.radius = mesh.radius;
    glm::vec3 lo(1e30f), hi(-1e30f);
    float largest = 0.0f, scale = entry.palette.empty() ? 1.0f : 0.0f;
    for (std::size_t j = 0; j < entry.palette.size(); ++j) {
        const glm::vec4& b = mesh.jointBounds[j];
        if (b.w < 0.0f) continue;
        const glm::mat4& m = entry.palette[j];
        const glm::vec3 c(m * glm::vec4(glm::vec3(b), 1.0f));
        const float s =
            std::sqrt(std::max({glm::dot(glm::vec3(m[0]), glm::vec3(m[0])), glm::dot(glm::vec3(m[1]), glm::vec3(m[1])),
                                glm::dot(glm::vec3(m[2]), glm::vec3(m[2]))}));
        const float r = b.w * s;
        lo = glm::min(lo, c - r);
        hi = glm::max(hi, c + r);
        largest = std::max(largest, r);
        scale = std::max(scale, s);
    }
    if (lo.x <= hi.x) {
        entry.center = (lo + hi) * 0.5f;
        entry.radius = glm::length(hi - lo) * 0.5f;
        if (entry.desc.skinning == Skinning::DualQuaternion) entry.radius += largest;
    }
    float reach = 0.0f;
    for (std::size_t k = 0; k < entry.morphWeights.size() && k < mesh.morphReach.size(); ++k)
        reach += std::abs(entry.morphWeights[k]) * mesh.morphReach[k];
    entry.radius += reach * scale;
}

} // namespace

InstanceId Scene::addInstance(const InstanceDesc& desc) {
    MeshEntry* mesh = lookup(m_impl->meshes, desc.mesh, "addInstance: stale or null mesh");
    MaterialEntry* material = lookup(m_impl->materials, desc.material, "addInstance: stale or null material");
    if (!mesh || !material) return {};
    mesh->users++;
    material->users++;
    InstanceEntry entry;
    entry.desc = desc;
    entry.previous = desc.transform;
    entry.center = mesh->center;
    entry.radius = mesh->radius;
    return m_impl->instances.add(std::move(entry));
}

void Scene::setJointPalette(InstanceId instance, std::span<const glm::mat4> palette, bool teleport) {
    InstanceEntry* entry = lookup(m_impl->instances, instance, "setJointPalette: stale or null instance");
    if (!entry) return;
    const MeshEntry* mesh = m_impl->meshes.get(entry->desc.mesh);
    QUENCH_ASSERT(mesh && palette.size() == mesh->jointCount, "setJointPalette: one matrix per joint of the mesh");
    if (!mesh || palette.size() != mesh->jointCount || palette.empty()) return;
    // The same pose (a held animation): no motion, and the renderer copies the vertices it skinned last frame.
    if (!teleport && std::ranges::equal(palette, entry->palette)) return;
    entry->palette.assign(palette.begin(), palette.end());
    entry->posed = !teleport;
    if (teleport) entry->skinnedFrame = ~0ull;
    updatePoseBounds(*entry, *mesh);
}

void Scene::setSkinning(InstanceId instance, Skinning skinning) {
    InstanceEntry* entry = lookup(m_impl->instances, instance, "setSkinning: stale or null instance");
    if (!entry || entry->desc.skinning == skinning) return;
    entry->desc.skinning = skinning;
    entry->skinnedFrame = ~0ull; // skinned again, not copied from the last frame
    if (const MeshEntry* mesh = m_impl->meshes.get(entry->desc.mesh); mesh && !entry->palette.empty())
        updatePoseBounds(*entry, *mesh);
}

void Scene::setMorphWeights(InstanceId instance, std::span<const float> weights) {
    InstanceEntry* entry = lookup(m_impl->instances, instance, "setMorphWeights: stale or null instance");
    if (!entry) return;
    const MeshEntry* mesh = m_impl->meshes.get(entry->desc.mesh);
    if (!mesh || mesh->morphTargetCount == 0) return;
    QUENCH_ASSERT(weights.size() <= mesh->morphTargetCount, "setMorphWeights: more weights than the mesh's targets");
    std::vector<float> w(mesh->morphTargetCount, 0.0f);
    std::copy_n(weights.begin(), std::min<std::size_t>(weights.size(), w.size()), w.begin());
    const bool zero = std::ranges::all_of(w, [](float v) { return v == 0.0f; });
    if (zero ? entry->morphWeights.empty() : w == entry->morphWeights) return;
    if (zero)
        entry->morphWeights.clear();
    else
        entry->morphWeights = std::move(w);
    entry->posed = true;
    updatePoseBounds(*entry, *mesh);
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

namespace detail {

bool isAreaLight(LightType type) {
    return type == LightType::Rect || type == LightType::Disk || type == LightType::Tube;
}

AreaLightFrame areaLightFrame(const LightDesc& desc) {
    AreaLightFrame f;
    const auto unit = [](const glm::vec3& v, const glm::vec3& fallback) {
        const float l = glm::length(v);
        return l > 1e-6f ? v / l : fallback;
    };
    const auto across = [](const glm::vec3& n) {
        return glm::normalize(glm::cross(n, std::abs(n.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    };
    if (desc.type == LightType::Tube) {
        f.tangent = unit(desc.tangent, glm::vec3(1, 0, 0));
        f.normal = across(f.tangent);
        f.half = glm::vec2(std::max(desc.size.x, 0.0f) * 0.5f, std::max(desc.size.y, 1e-4f));
    } else {
        f.normal = unit(desc.direction, glm::vec3(0, -1, 0));
        f.tangent = unit(desc.tangent - f.normal * glm::dot(f.normal, desc.tangent), across(f.normal));
        f.half = desc.type == LightType::Rect ? glm::max(desc.size * 0.5f, glm::vec2(1e-4f))
                                              : glm::vec2(std::max(desc.size.x, 1e-4f));
    }
    f.bitangent = glm::cross(f.normal, f.tangent);
    f.radius = desc.type == LightType::Disk ? f.half.x : glm::length(f.half);
    return f;
}

} // namespace detail

namespace {

// The shape of an area light: a unit mesh per type, placed by the light's frame (its local y is the rect's and the
// disk's normal, the tube's axis).
glm::mat4 areaShapeTransform(const LightDesc& desc) {
    const detail::AreaLightFrame f = detail::areaLightFrame(desc);
    glm::mat4 m(1.0f);
    if (desc.type == LightType::Tube) {
        const glm::vec3 x = f.normal, z = glm::cross(x, f.tangent);
        m[0] = glm::vec4(x * f.half.y, 0.0f);
        m[1] = glm::vec4(f.tangent * (f.half.x * 2.0f), 0.0f);
        m[2] = glm::vec4(z * f.half.y, 0.0f);
    } else {
        const float scale = desc.type == LightType::Rect ? 2.0f : 1.0f;
        m[0] = glm::vec4(f.tangent * (f.half.x * scale), 0.0f);
        m[1] = glm::vec4(f.normal, 0.0f);
        m[2] = glm::vec4(glm::cross(f.tangent, f.normal) * (f.half.y * scale), 0.0f);
    }
    m[3] = glm::vec4(desc.position, 1.0f);
    return m;
}

MaterialDesc areaShapeMaterial(const LightDesc& desc) {
    MaterialDesc m;
    m.baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    m.emissive = desc.color * desc.intensity;
    m.roughness = 1.0f;
    m.doubleSided = true; // the back of a one-sided shape is drawn black (MaterialEntry::lightTwoSided)
    m.shadows = ShadowMode::None;
    return m;
}

void dropAreaShape(Scene& scene, detail::SceneImpl& impl, detail::LightEntry& light) {
    if (light.shape) {
        scene.removeInstance(light.shape);
        --impl.ownInstances;
    }
    if (light.shapeMaterial) {
        scene.removeMaterial(light.shapeMaterial);
        --impl.ownMaterials;
    }
    light.shape = {};
    light.shapeMaterial = {};
}

// Makes, updates or removes the shape of a light after its description changed.
void syncAreaShape(Scene& scene, detail::SceneImpl& impl, LightId id) {
    detail::LightEntry* light = impl.lights.get(id);
    if (!light) return;
    const LightDesc desc = light->desc;
    if (!detail::isAreaLight(desc.type)) {
        dropAreaShape(scene, impl, *light);
        return;
    }
    const auto shape = std::size_t(desc.type) - std::size_t(LightType::Rect);
    MeshId& mesh = impl.areaShapes[shape];
    if (!mesh) {
        const MeshData data = desc.type == LightType::Rect   ? planeMesh(glm::vec2(0.5f))
                              : desc.type == LightType::Disk ? diskMesh(1.0f)
                                                             : cylinderMesh(1.0f, 0.5f, 24);
        Result<MeshId> made = scene.addMesh(data.desc("area light"));
        if (!made) {
            log::error("render", "area light shape: {}", made.error());
            return;
        }
        mesh = *made;
        ++impl.ownMeshes;
    }
    if (light->shape) {
        const InstanceEntry* instance = impl.instances.get(light->shape);
        if (!instance || instance->desc.mesh != mesh) dropAreaShape(scene, impl, *light);
    }
    MaterialDesc material = areaShapeMaterial(desc);
    if (!light->shapeMaterial) {
        light->shapeMaterial = scene.addMaterial(material);
        ++impl.ownMaterials;
    } else {
        scene.setMaterial(light->shapeMaterial, material);
    }
    detail::MaterialEntry* entry = impl.materials.get(light->shapeMaterial);
    entry->light = id.index + 1;
    entry->lightTwoSided = desc.twoSided && desc.type != LightType::Tube;
    const glm::mat4 transform = areaShapeTransform(desc);
    if (!light->shape) {
        light->shape = scene.addInstance(
            {.mesh = mesh, .material = light->shapeMaterial, .transform = transform, .castsShadows = false});
        ++impl.ownInstances;
    } else {
        scene.setTransform(light->shape, transform);
    }
}

} // namespace

LightId Scene::addLight(const LightDesc& desc) {
    const LightId id = m_impl->lights.add({desc});
    if (detail::isAreaLight(desc.type)) syncAreaShape(*this, *m_impl, id);
    return id;
}

void Scene::setLight(LightId light, const LightDesc& desc) {
    detail::LightEntry* entry = lookup(m_impl->lights, light, "setLight: stale or null light");
    if (!entry) return;
    entry->desc = desc;
    if (detail::isAreaLight(desc.type) || entry->shape) syncAreaShape(*this, *m_impl, light);
}

void Scene::removeLight(LightId light) {
    detail::LightEntry* entry = lookup(m_impl->lights, light, "removeLight: stale or null handle");
    if (!entry) return;
    dropAreaShape(*this, *m_impl, *entry);
    m_impl->lights.remove(light);
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
std::uint32_t Scene::jointCount(MeshId mesh) const {
    const MeshEntry* entry = m_impl->meshes.get(mesh);
    return entry ? entry->jointCount : 0;
}

std::uint32_t Scene::morphTargetCount(MeshId mesh) const {
    const MeshEntry* entry = m_impl->meshes.get(mesh);
    return entry ? entry->morphTargetCount : 0;
}
std::uint32_t Scene::meshCount() const { return m_impl->meshes.size() - m_impl->ownMeshes; }
std::uint32_t Scene::textureCount() const { return m_impl->textures.size(); }
std::uint32_t Scene::materialCount() const { return m_impl->materials.size() - m_impl->ownMaterials; }
std::uint32_t Scene::instanceCount() const { return m_impl->instances.size() - m_impl->ownInstances; }
std::uint32_t Scene::lightCount() const { return m_impl->lights.size(); }

} // namespace quench::render
