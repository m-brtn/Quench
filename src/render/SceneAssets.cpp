#include <quench/render/SceneAssets.h>

#include "SlotMap.h"

#include <quench/assets/Model.h>
#include <quench/core/Assert.h>
#include <quench/core/Log.h>
#include <quench/gpu/Device.h>
#include <quench/render/MeshBuilder.h>

#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <format>

namespace quench::render {

namespace {

// 8 x 8 magenta and dark gray squares of 1 texel: the texture of a failed load.
std::vector<std::byte> checkerPixels() {
    std::vector<std::byte> pixels(8 * 8 * 4);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const bool on = ((x ^ y) & 1) != 0;
            std::byte* p = &pixels[std::size_t(y * 8 + x) * 4];
            p[0] = std::byte(on ? 255 : 40);
            p[1] = std::byte(on ? 0 : 40);
            p[2] = std::byte(on ? 255 : 40);
            p[3] = std::byte(255);
        }
    return pixels;
}

} // namespace

struct SceneAssets::Impl {
    struct Placed {
        assets::AssetRef<ModelAsset> model;
        glm::mat4 transform{1.0f};
        bool visible = true;
        std::vector<InstanceId> instances;
        std::vector<int> instanceSkins;               // per instance: the part's skin (-1: rigid, placeholder)
        std::vector<std::uint32_t> instanceJoints;    // per instance: its mesh's joints
        std::vector<std::vector<glm::mat4>> palettes; // per skin: the last setJointPalette
        std::vector<Skinning> skinnings;              // per skin: the last setSkinning
        bool placeholder = false;
        bool built = false;
        assets::AssetState builtState = assets::AssetState::Loading;
        std::uint32_t builtVersion = 0;
    };

    detail::SlotMap<ModelInstanceId, Placed> placed;
    std::vector<MeshId> retiredMeshes;
    std::vector<MaterialId> retiredMaterials;
    MeshId box;
    MaterialId loadingMaterial;
    MaterialId failedMaterial;
    std::vector<std::byte> checker = checkerPixels();
    assets::AssetRef<ModelAsset> none;
};

// ------------------------------------------------------------------ textures

TextureAsset::TextureAsset(SceneAssets& owner)
    : m_owner(owner), m_texture(owner.m_scene.addEmptyTexture()), m_skipLevels(owner.m_settings.textureSkipLevels),
      m_bcTextures(owner.m_bcTextures), m_astcTextures(owner.m_astcTextures) {}

Result<void> TextureAsset::decode(fs::Bytes bytes) {
    Result<assets::TextureImage> image = assets::readKtx2(bytes, m_skipLevels);
    if (!image) return Error{image.error()};
    m_pendingDecoded = false;
    const assets::PixelFormat format = image->format;
    if ((format == assets::PixelFormat::Bc7 || format == assets::PixelFormat::Bc7Srgb) && !m_bcTextures) {
        image = assets::decodeBc7(*image);
        if (!image) return Error{image.error()};
        m_pendingDecoded = true;
    } else if ((format == assets::PixelFormat::Astc4x4 || format == assets::PixelFormat::Astc4x4Srgb) &&
               !m_astcTextures) {
        return Error{
            "the device cannot sample ASTC 4x4 and the runtime has no ASTC decoder (cook BC7 or RGBA8 for it)"};
    }
    m_pending = std::move(*image);
    return {};
}

std::uint64_t TextureAsset::uploadBytes() const { return m_pending ? m_pending->data.size() : 0; }

Result<void> TextureAsset::finish() {
    if (!m_pending) return Error{"nothing decoded"};
    const assets::TextureImage image = std::move(*m_pending);
    m_pending.reset();
    if (Result<void> set = m_owner.m_scene.setTexture(m_texture, image, path()); !set) return set;
    m_width = image.width;
    m_height = image.height;
    m_mipLevels = image.mipLevels;
    m_format = image.format;
    m_gpuBytes = image.data.size();
    m_decodedOnCpu = m_pendingDecoded;
    return {};
}

void TextureAsset::failed() {
    m_pending.reset();
    const std::vector<std::byte>& checker = m_owner.m_impl->checker;
    if (Result<void> set = m_owner.m_scene.setTexture(
            m_texture,
            TextureData{.width = 8, .height = 8, .pixels = checker, .mipmaps = false, .name = "missing texture"});
        set) {
        m_width = m_height = 8;
        m_mipLevels = 1;
        m_format = assets::PixelFormat::Rgba8Srgb;
        m_gpuBytes = checker.size();
    }
}

void TextureAsset::unload() {
    m_pending.reset();
    if (m_owner.m_scene.contains(m_texture)) m_owner.m_scene.removeTexture(m_texture);
    m_texture = {};
    m_gpuBytes = 0;
}

assets::AssetMemory TextureAsset::residentMemory() const { return {0, m_gpuBytes}; }

// ------------------------------------------------------------------ models

ModelAsset::ModelAsset(SceneAssets& owner) : m_owner(owner), m_wantedLod(owner.m_settings.modelLod) {}

std::uint32_t ModelAsset::triangles() const {
    std::uint32_t n = 0;
    for (const Part& part : m_parts) n += part.triangles;
    return n;
}

Result<void> ModelAsset::decode(fs::Bytes bytes) {
    Result<assets::ModelData> model = assets::readModel(bytes);
    if (!model) return Error{model.error()};
    if (model->meshes.empty()) return Error{"the model has no meshes"};
    Pending pending;
    pending.materials = std::move(model->materials);
    pending.skins = std::move(model->skins);
    pending.boundsMin = model->boundsMin;
    pending.boundsMax = model->boundsMax;
    for (assets::ModelMesh& mesh : model->meshes) {
        const auto lodCount = std::uint32_t(mesh.lods.size());
        const std::uint32_t lod = std::min(m_wantedLod, lodCount - 1);
        pending.lod = std::max(pending.lod, lod);
        pending.lodCount = std::max(pending.lodCount, lodCount);
        const assets::ModelLod& range = mesh.lods[lod];
        // Only the vertices this LOD uses, in the order it first uses them.
        std::vector<std::uint32_t> remap(mesh.vertices.size(), ~0u);
        PendingMesh out;
        out.name = std::move(mesh.name);
        out.material = mesh.material;
        out.skin = mesh.skin;
        out.indices.reserve(range.indexCount);
        for (std::uint32_t i = range.firstIndex; i < range.firstIndex + range.indexCount; ++i) {
            std::uint32_t& mapped = remap[mesh.indices[i]];
            if (mapped == ~0u) {
                mapped = std::uint32_t(out.vertices.size());
                const assets::ModelVertex& v = mesh.vertices[mesh.indices[i]];
                out.vertices.push_back({v.position, v.normal, v.uv, v.color, v.tangent});
                if (mesh.skin >= 0) {
                    const assets::ModelSkinVertex& w = mesh.skinning[mesh.indices[i]];
                    out.skinning.push_back({{w.joints[0], w.joints[1], w.joints[2], w.joints[3]},
                                            {w.weights[0], w.weights[1], w.weights[2], w.weights[3]}});
                }
            }
            out.indices.push_back(mapped);
        }
        if (out.indices.empty()) continue;
        pending.meshes.push_back(std::move(out));
    }
    if (pending.meshes.empty()) return Error{"the model's level of detail has no triangles"};
    m_pending = std::move(pending);
    return {};
}

std::uint64_t ModelAsset::uploadBytes() const {
    if (!m_pending) return 0;
    std::uint64_t bytes = 0;
    for (const PendingMesh& mesh : m_pending->meshes)
        bytes += mesh.vertices.size() * sizeof(Vertex) + mesh.indices.size() * sizeof(std::uint32_t) +
                 mesh.skinning.size() * sizeof(SkinVertex);
    return bytes;
}

Result<void> ModelAsset::finish() {
    if (!m_pending) return Error{"nothing decoded"};
    Pending pending = std::move(*m_pending);
    m_pending.reset();
    Scene& scene = m_owner.m_scene;

    // The textures load on their own (the same paths as before a reload: cached, not loaded again).
    std::vector<assets::AssetRef<TextureAsset>> textures;
    const auto texture = [&](const std::string& texturePath) -> TextureId {
        if (texturePath.empty()) return {};
        assets::AssetRef<TextureAsset> ref = m_owner.loadTexture(texturePath, priority());
        const TextureId id = ref ? ref->texture() : TextureId{};
        textures.push_back(std::move(ref));
        return id;
    };
    std::vector<MaterialId> materials;
    for (const assets::ModelMaterial& m : pending.materials) {
        MaterialDesc desc;
        desc.baseColor = m.baseColor;
        desc.emissive = m.emissive;
        desc.roughness = m.roughness;
        desc.metallic = m.metallic;
        desc.alphaCutoff = m.alphaMode == assets::AlphaMode::Mask ? std::max(m.alphaCutoff, 1e-3f) : 0.0f;
        desc.blend = m.alphaMode == assets::AlphaMode::Blend ? BlendMode::AlphaBlend : BlendMode::Opaque;
        desc.doubleSided = m.doubleSided;
        desc.textures[0] = texture(m.baseColorTexture);
        desc.textures[1] = texture(m.metallicRoughnessTexture);
        desc.textures[2] = texture(m.normalTexture);
        desc.textures[3] = texture(m.emissiveTexture);
        desc.name = m.name;
        materials.push_back(scene.addMaterial(desc));
    }
    std::vector<Part> parts;
    std::uint64_t gpuBytes = 0;
    for (const PendingMesh& mesh : pending.meshes) {
        const auto joints =
            mesh.skin >= 0 ? std::uint32_t(pending.skins[std::size_t(mesh.skin)].joints.size()) : std::uint32_t(0);
        Result<MeshId> id =
            scene.addMesh({mesh.vertices, mesh.indices, mesh.name.empty() ? path() : mesh.name, mesh.skinning, joints});
        if (!id) {
            for (const Part& part : parts) scene.removeMesh(part.mesh);
            for (MaterialId material : materials) scene.removeMaterial(material);
            return Error{id.error()};
        }
        parts.push_back({*id, materials[mesh.material], std::uint32_t(mesh.indices.size() / 3), mesh.skin});
        gpuBytes += mesh.vertices.size() * sizeof(Vertex) + mesh.indices.size() * sizeof(std::uint32_t) +
                    mesh.skinning.size() * sizeof(SkinVertex);
    }

    retireParts();
    m_parts = std::move(parts);
    m_skins = std::move(pending.skins);
    m_materials = std::move(materials);
    m_textures = std::move(textures);
    m_boundsMin = pending.boundsMin;
    m_boundsMax = pending.boundsMax;
    m_lod = pending.lod;
    m_lodCount = pending.lodCount;
    m_gpuBytes = gpuBytes;
    return {};
}

void ModelAsset::retireParts() {
    for (const Part& part : m_parts) m_owner.retire(part.mesh, {});
    for (MaterialId material : m_materials) m_owner.retire({}, material);
    m_parts.clear();
    m_skins.clear();
    m_materials.clear();
}

void ModelAsset::unload() {
    m_pending.reset();
    retireParts();
    m_textures.clear();
    m_gpuBytes = 0;
}

assets::AssetMemory ModelAsset::residentMemory() const { return {0, m_gpuBytes}; }

// ------------------------------------------------------------------ SceneAssets

SceneAssets::SceneAssets(Scene& scene, assets::AssetManager& manager, const SceneAssetsSettings& settings)
    : m_scene(scene), m_manager(manager), m_settings(settings), m_impl(std::make_unique<Impl>()) {
    const gpu::Caps& caps = scene.device().caps();
    m_bcTextures = caps.bcTextures;
    m_astcTextures = caps.astcTextures;
    const MeshData box = boxMesh(glm::vec3(0.5f));
    if (Result<MeshId> mesh = scene.addMesh(box.desc("asset placeholder")); mesh)
        m_impl->box = *mesh;
    else
        log::error("assets", "placeholder box: {}", mesh.error());
    m_impl->loadingMaterial =
        scene.addMaterial({.baseColor = glm::vec4(0.55f, 0.57f, 0.6f, 1.0f), .roughness = 0.8f, .name = "loading"});
    m_impl->failedMaterial = scene.addMaterial({.baseColor = glm::vec4(0.9f, 0.0f, 0.9f, 1.0f),
                                                .emissive = glm::vec3(0.25f, 0.0f, 0.25f),
                                                .roughness = 0.6f,
                                                .name = "missing"});
    manager.registerType<TextureAsset>(
        "texture", [this](std::string_view) { return std::unique_ptr<TextureAsset>(new TextureAsset(*this)); },
        settings.textureBudget);
    manager.registerType<ModelAsset>(
        "model", [this](std::string_view) { return std::unique_ptr<ModelAsset>(new ModelAsset(*this)); },
        settings.modelBudget);
    log::info("assets", "scene assets: BC7 {}, ASTC {}", m_bcTextures ? "sampled" : "decoded to RGBA8 on load",
              m_astcTextures ? "sampled" : "not supported");
}

SceneAssets::~SceneAssets() {
    for (std::uint32_t i = 0; i < m_impl->placed.capacity(); ++i)
        if (m_impl->placed.slot(i)) removeModel(m_impl->placed.idOf(i));
    // Models first: they hold their textures.
    m_manager.unregisterType<ModelAsset>();
    m_manager.unregisterType<TextureAsset>();
    update();
    if (m_impl->box && m_scene.meshUsers(m_impl->box) == 0) m_scene.removeMesh(m_impl->box);
    for (MaterialId material : {m_impl->loadingMaterial, m_impl->failedMaterial})
        if (m_scene.materialUsers(material) == 0) m_scene.removeMaterial(material);
}

gpu::Device& SceneAssets::device() { return m_scene.device(); }

assets::AssetRef<TextureAsset> SceneAssets::loadTexture(std::string_view path, float priority) {
    return m_manager.load<TextureAsset>(path, priority);
}

assets::AssetRef<ModelAsset> SceneAssets::loadModel(std::string_view path, float priority) {
    return m_manager.load<ModelAsset>(path, priority);
}

namespace {

glm::mat4 placeholderTransform(const glm::mat4& transform) {
    return transform * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f));
}

} // namespace

ModelInstanceId SceneAssets::addModel(assets::AssetRef<ModelAsset> model, const glm::mat4& transform, bool visible) {
    QUENCH_ASSERT(model, "addModel without a model");
    Impl::Placed placed;
    placed.model = std::move(model);
    placed.transform = transform;
    placed.visible = visible;
    const ModelInstanceId id = m_impl->placed.add(std::move(placed));
    update();
    return id;
}

void SceneAssets::setTransform(ModelInstanceId instance, const glm::mat4& transform, bool teleport) {
    Impl::Placed* placed = m_impl->placed.get(instance);
    QUENCH_ASSERT(placed, "setTransform: stale or null model instance");
    if (!placed) return;
    placed->transform = transform;
    const glm::mat4 world = placed->placeholder ? placeholderTransform(transform) : transform;
    for (InstanceId id : placed->instances) m_scene.setTransform(id, world, teleport);
}

void SceneAssets::setVisible(ModelInstanceId instance, bool visible) {
    Impl::Placed* placed = m_impl->placed.get(instance);
    QUENCH_ASSERT(placed, "setVisible: stale or null model instance");
    if (!placed) return;
    placed->visible = visible;
    for (InstanceId id : placed->instances) m_scene.setVisible(id, visible);
}

void SceneAssets::setJointPalette(ModelInstanceId instance, int skin, std::span<const glm::mat4> palette,
                                  bool teleport) {
    Impl::Placed* placed = m_impl->placed.get(instance);
    QUENCH_ASSERT(placed && skin >= 0, "setJointPalette: stale or null model instance, or a negative skin");
    if (!placed || skin < 0) return;
    if (placed->palettes.size() <= std::size_t(skin)) placed->palettes.resize(std::size_t(skin) + 1);
    placed->palettes[std::size_t(skin)].assign(palette.begin(), palette.end());
    for (std::size_t i = 0; i < placed->instances.size(); ++i)
        if (placed->instanceSkins[i] == skin && placed->instanceJoints[i] == palette.size())
            m_scene.setJointPalette(placed->instances[i], palette, teleport);
}

void SceneAssets::setSkinning(ModelInstanceId instance, int skin, Skinning skinning) {
    Impl::Placed* placed = m_impl->placed.get(instance);
    QUENCH_ASSERT(placed && skin >= 0, "setSkinning: stale or null model instance, or a negative skin");
    if (!placed || skin < 0) return;
    if (placed->skinnings.size() <= std::size_t(skin)) placed->skinnings.resize(std::size_t(skin) + 1);
    placed->skinnings[std::size_t(skin)] = skinning;
    for (std::size_t i = 0; i < placed->instances.size(); ++i)
        if (placed->instanceSkins[i] == skin) m_scene.setSkinning(placed->instances[i], skinning);
}

void SceneAssets::removeModel(ModelInstanceId instance) {
    Impl::Placed* placed = m_impl->placed.get(instance);
    QUENCH_ASSERT(placed, "removeModel: stale or null model instance");
    if (!placed) return;
    for (InstanceId id : placed->instances) m_scene.removeInstance(id);
    m_impl->placed.remove(instance);
}

const assets::AssetRef<ModelAsset>& SceneAssets::model(ModelInstanceId instance) const {
    const Impl::Placed* placed = m_impl->placed.get(instance);
    return placed ? placed->model : m_impl->none;
}

std::uint32_t SceneAssets::modelInstanceCount() const { return m_impl->placed.size(); }

void SceneAssets::retire(MeshId mesh, MaterialId material) {
    if (mesh) m_impl->retiredMeshes.push_back(mesh);
    if (material) m_impl->retiredMaterials.push_back(material);
}

void SceneAssets::update() {
    for (std::uint32_t i = 0; i < m_impl->placed.capacity(); ++i) {
        Impl::Placed* placed = m_impl->placed.slot(i);
        if (!placed) continue;
        const ModelAsset& model = *placed->model;
        if (placed->built && placed->builtState == model.state() && placed->builtVersion == model.version()) continue;
        for (InstanceId id : placed->instances) m_scene.removeInstance(id);
        placed->instances.clear();
        placed->instanceSkins.clear();
        placed->instanceJoints.clear();
        placed->placeholder = model.parts().empty();
        if (placed->placeholder) {
            if (m_impl->box) {
                const MaterialId material =
                    model.state() == assets::AssetState::Failed ? m_impl->failedMaterial : m_impl->loadingMaterial;
                placed->instances.push_back(m_scene.addInstance({.mesh = m_impl->box,
                                                                 .material = material,
                                                                 .transform = placeholderTransform(placed->transform),
                                                                 .visible = placed->visible}));
                placed->instanceSkins.push_back(-1);
                placed->instanceJoints.push_back(0);
            }
        } else {
            for (const ModelAsset::Part& part : model.parts()) {
                const bool known = part.skin >= 0 && std::size_t(part.skin) < placed->skinnings.size();
                const InstanceId id = m_scene.addInstance(
                    {.mesh = part.mesh,
                     .material = part.material,
                     .transform = placed->transform,
                     .visible = placed->visible,
                     .skinning = known ? placed->skinnings[std::size_t(part.skin)] : Skinning::Linear});
                placed->instances.push_back(id);
                const std::uint32_t joints = m_scene.jointCount(part.mesh);
                placed->instanceSkins.push_back(part.skin);
                placed->instanceJoints.push_back(joints);
                if (part.skin >= 0 && std::size_t(part.skin) < placed->palettes.size()) {
                    const std::vector<glm::mat4>& palette = placed->palettes[std::size_t(part.skin)];
                    if (!palette.empty() && palette.size() == joints) m_scene.setJointPalette(id, palette, true);
                }
            }
        }
        placed->built = true;
        placed->builtState = model.state();
        placed->builtVersion = model.version();
    }
    std::erase_if(m_impl->retiredMeshes, [this](MeshId mesh) {
        if (!m_scene.contains(mesh)) return true;
        if (m_scene.meshUsers(mesh) != 0) return false;
        m_scene.removeMesh(mesh);
        return true;
    });
    std::erase_if(m_impl->retiredMaterials, [this](MaterialId material) {
        if (!m_scene.contains(material)) return true;
        if (m_scene.materialUsers(material) != 0) return false;
        m_scene.removeMaterial(material);
        return true;
    });
}

} // namespace quench::render
