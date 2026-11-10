#include <quench/render/SceneAssets.h>

#include "SlotMap.h"

#include <quench/assets/Model.h>
#include <quench/core/Assert.h>
#include <quench/core/Log.h>
#include <quench/gpu/Device.h>
#include <quench/render/MeshBuilder.h>

#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
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
        bool castsShadows = true;
        std::vector<InstanceId> instances;
        std::vector<int> instanceSkins;               // per instance: the part's skin (-1: rigid, placeholder)
        std::vector<std::uint32_t> instanceJoints;    // per instance: its mesh's joints
        std::vector<std::vector<glm::mat4>> palettes; // per skin: the last setJointPalette
        std::vector<Skinning> skinnings;              // per skin: the last setSkinning
        std::vector<std::string> morphNames;          // the model's blend shapes at the last setMorphWeights
        std::vector<float> morphWeights;              // their weights then
        bool morphSet = false;                        // setMorphWeights was called
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
      m_support(owner.m_support) {}

Result<void> TextureAsset::decode(fs::Bytes bytes) {
    const auto start = std::chrono::steady_clock::now();
    Result<assets::PreparedTexture> prepared = assets::prepareTexture(bytes, m_support, m_skipLevels);
    if (!prepared) return Error{prepared.error()};
    m_pending = std::move(*prepared);
    m_pendingMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return {};
}

std::uint64_t TextureAsset::uploadBytes() const { return m_pending ? m_pending->image.data.size() : 0; }

Result<void> TextureAsset::finish() {
    if (!m_pending) return Error{"nothing decoded"};
    const assets::PreparedTexture prepared = std::move(*m_pending);
    m_pending.reset();
    const assets::TextureImage& image = prepared.image;
    if (Result<void> set = m_owner.m_scene.setTexture(m_texture, image, path()); !set) return set;
    m_width = image.width;
    m_height = image.height;
    m_mipLevels = image.mipLevels;
    m_format = image.format;
    m_gpuBytes = image.data.size();
    m_decodedOnCpu = prepared.decodedOnCpu;
    m_basis = prepared.basis;
    m_decodeMs = m_pendingMs;
    if (m_basis && m_decodedOnCpu) {
        const assets::TextureSupport& support = m_support;
        if (support.bc7 || support.astc4x4 || support.etc2) {
            log::info("assets",
                      "{}: Basis Universal file of {} x {} transcoded to RGBA8 (block formats need sides "
                      "that are multiples of 4)",
                      path(), image.width, image.height);
        } else if (!m_owner.m_loggedBasisRgba8) {
            m_owner.m_loggedBasisRgba8 = true;
            log::warning("assets",
                         "{}: the device samples none of BC7, ASTC 4x4 and ETC2: Basis Universal files are "
                         "transcoded to RGBA8 (4 times the memory; logged once)",
                         path());
        }
    }
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
        std::vector<std::uint32_t> source;
        for (std::uint32_t i = range.firstIndex; i < range.firstIndex + range.indexCount; ++i) {
            std::uint32_t& mapped = remap[mesh.indices[i]];
            if (mapped == ~0u) {
                mapped = std::uint32_t(out.vertices.size());
                source.push_back(mesh.indices[i]);
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
        out.morphs.reserve(mesh.morphTargets.size() * source.size());
        for (const assets::ModelMorphTarget& target : mesh.morphTargets) {
            for (const std::uint32_t v : source) {
                const assets::ModelMorphVertex& m = target.offsets[v];
                out.morphs.push_back({m.position, m.normal, m.tangent});
            }
            const auto known = std::ranges::find(pending.morphTargets, target.name);
            out.morphTargets.push_back(std::uint32_t(known - pending.morphTargets.begin()));
            out.morphDefaults.push_back(target.weight);
            if (known == pending.morphTargets.end()) {
                pending.morphTargets.push_back(target.name);
                pending.morphDefaults.push_back(target.weight);
            }
        }
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
                 mesh.skinning.size() * sizeof(SkinVertex) + mesh.morphs.size() * 3 * sizeof(glm::vec4);
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
        Result<MeshId> id = scene.addMesh({.vertices = mesh.vertices,
                                           .indices = mesh.indices,
                                           .name = mesh.name.empty() ? path() : mesh.name,
                                           .skin = mesh.skinning,
                                           .jointCount = joints,
                                           .morphs = mesh.morphs,
                                           .morphTargetCount = std::uint32_t(mesh.morphTargets.size())});
        if (!id) {
            for (const Part& part : parts) scene.removeMesh(part.mesh);
            for (MaterialId material : materials) scene.removeMaterial(material);
            return Error{id.error()};
        }
        parts.push_back({*id, materials[mesh.material], std::uint32_t(mesh.indices.size() / 3), mesh.skin,
                         mesh.morphTargets, mesh.morphDefaults});
        gpuBytes += mesh.vertices.size() * sizeof(Vertex) + mesh.indices.size() * sizeof(std::uint32_t) +
                    mesh.skinning.size() * sizeof(SkinVertex) + mesh.morphs.size() * 3 * sizeof(glm::vec4);
    }

    retireParts();
    m_parts = std::move(parts);
    m_skins = std::move(pending.skins);
    m_morphTargets = std::move(pending.morphTargets);
    m_morphDefaults = std::move(pending.morphDefaults);
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
    m_morphTargets.clear();
    m_morphDefaults.clear();
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
    m_support = {.bc7 = caps.bcTextures, .astc4x4 = caps.astcTextures, .etc2 = caps.etc2Textures};
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
    const auto basisTarget = [this](assets::BasisCodec codec) {
        return assets::pixelFormatInfo(assets::basisTarget({.codec = codec, .width = 4, .height = 4}, m_support)).name;
    };
    log::info("assets", "scene assets: BC7 {}, ASTC {}, ETC2 {}; Basis Universal UASTC to {}, ETC1S to {}",
              m_support.bc7 ? "sampled" : "decoded to RGBA8 on load", m_support.astc4x4 ? "sampled" : "not supported",
              m_support.etc2 ? "sampled" : "not supported", basisTarget(assets::BasisCodec::Uastc),
              basisTarget(assets::BasisCodec::Etc1s));
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

// A part's blend shape weights: what setMorphWeights gave for their names, else the file's defaults.
std::vector<float> partWeights(const ModelAsset& model, const ModelAsset::Part& part,
                               std::span<const std::string> names, std::span<const float> weights, bool set) {
    if (!set) return part.morphDefaults;
    std::vector<float> out;
    out.reserve(part.morphs.size());
    for (const std::uint32_t m : part.morphs) {
        const auto i = std::size_t(std::ranges::find(names, model.morphTargets()[m]) - names.begin());
        out.push_back(i < weights.size() ? weights[i] : 0.0f);
    }
    return out;
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

void SceneAssets::setCastsShadows(ModelInstanceId instance, bool castsShadows) {
    Impl::Placed* placed = m_impl->placed.get(instance);
    QUENCH_ASSERT(placed, "setCastsShadows: stale or null model instance");
    if (!placed) return;
    placed->castsShadows = castsShadows;
    for (InstanceId id : placed->instances) m_scene.setCastsShadows(id, castsShadows);
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

void SceneAssets::setMorphWeights(ModelInstanceId instance, std::span<const float> weights) {
    Impl::Placed* placed = m_impl->placed.get(instance);
    QUENCH_ASSERT(placed, "setMorphWeights: stale or null model instance");
    if (!placed) return;
    const ModelAsset& model = *placed->model;
    if (model.morphTargets().empty()) return;
    placed->morphNames.assign(model.morphTargets().begin(), model.morphTargets().end());
    placed->morphWeights.assign(weights.begin(),
                                weights.begin() + std::ptrdiff_t(std::min(weights.size(), placed->morphNames.size())));
    placed->morphSet = true;
    if (placed->placeholder) return;
    for (std::size_t i = 0; i < placed->instances.size() && i < model.parts().size(); ++i)
        if (const ModelAsset::Part& part = model.parts()[i]; !part.morphs.empty())
            m_scene.setMorphWeights(placed->instances[i],
                                    partWeights(model, part, placed->morphNames, placed->morphWeights, true));
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
                                                                 .visible = placed->visible,
                                                                 .castsShadows = placed->castsShadows}));
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
                     .castsShadows = placed->castsShadows,
                     .skinning = known ? placed->skinnings[std::size_t(part.skin)] : Skinning::Linear});
                placed->instances.push_back(id);
                const std::uint32_t joints = m_scene.jointCount(part.mesh);
                placed->instanceSkins.push_back(part.skin);
                placed->instanceJoints.push_back(joints);
                if (part.skin >= 0 && std::size_t(part.skin) < placed->palettes.size()) {
                    const std::vector<glm::mat4>& palette = placed->palettes[std::size_t(part.skin)];
                    if (!palette.empty() && palette.size() == joints) m_scene.setJointPalette(id, palette, true);
                }
                if (!part.morphs.empty())
                    m_scene.setMorphWeights(
                        id, partWeights(model, part, placed->morphNames, placed->morphWeights, placed->morphSet));
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
