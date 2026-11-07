#pragma once

// Cooked textures (KTX2) and models (.qmodel) loaded through an assets::AssetManager into a render::Scene. Files are
// read and decoded on the job system; the GPU uploads happen in AssetManager::update() within its per-frame upload
// budget. What a load gives is usable at once: a texture handle samples white until its pixels arrive (a magenta
// checker if the load failed), and a placed model shows a placeholder box until its meshes are uploaded (a magenta
// box if the load failed). Reloaded files (hot reload) replace the data under the same handles.
//
// One SceneAssets per AssetManager: it registers the asset types "texture" and "model" and unregisters them when it
// is destroyed, which unloads their assets; every AssetRef to them must be gone by then, and the Scene must still
// exist. Main thread only.

#include <quench/assets/AssetManager.h>
#include <quench/assets/Model.h>
#include <quench/assets/Texture.h>
#include <quench/render/Scene.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::render {

class SceneAssets;

class TextureAsset final : public assets::Asset {
public:
    // Valid from the load on, also while loading and after a failure.
    TextureId texture() const { return m_texture; }
    // Of the resident pixels (0 before the first upload).
    std::uint32_t width() const { return m_width; }
    std::uint32_t height() const { return m_height; }
    std::uint32_t mipLevels() const { return m_mipLevels; }
    assets::PixelFormat format() const { return m_format; }
    // The texture is RGBA8 the file did not hold: its BC7 decoded on the CPU (the device cannot sample BC formats),
    // or its Basis Universal data transcoded to RGBA8 (the device samples none of BC7, ASTC 4x4 and ETC2, or a side
    // is not a multiple of 4).
    bool decodedOnCpu() const { return m_decodedOnCpu; }
    // The file's codec when it is a Basis Universal file (format() is what it was transcoded to).
    std::optional<assets::BasisCodec> basisCodec() const { return m_basis; }
    // How long reading the file took on its job (with BC7 decoding or Basis Universal transcoding), in milliseconds.
    double decodeMilliseconds() const { return m_decodeMs; }

private:
    friend class SceneAssets;
    explicit TextureAsset(SceneAssets& owner);

    Result<void> decode(fs::Bytes bytes) override;
    std::uint64_t uploadBytes() const override;
    Result<void> finish() override;
    void failed() override;
    void unload() override;
    assets::AssetMemory residentMemory() const override;

    SceneAssets& m_owner;
    TextureId m_texture;
    // Fixed at creation: decode() runs on a job.
    std::uint32_t m_skipLevels = 0;
    assets::TextureSupport m_support;
    std::optional<assets::PreparedTexture> m_pending; // decoded on a job, uploaded by finish()
    double m_pendingMs = 0.0;
    std::uint32_t m_width = 0, m_height = 0, m_mipLevels = 0;
    assets::PixelFormat m_format = assets::PixelFormat::Rgba8;
    std::uint64_t m_gpuBytes = 0;
    bool m_decodedOnCpu = false;
    std::optional<assets::BasisCodec> m_basis;
    double m_decodeMs = 0.0;
};

class ModelAsset final : public assets::Asset {
public:
    struct Part {
        MeshId mesh;
        MaterialId material;
        std::uint32_t triangles = 0;
        int skin = -1;                     // index into skins(); -1: rigid
        std::vector<std::uint32_t> morphs; // per blend shape of the mesh: its index in morphTargets()
        std::vector<float> morphDefaults;  // per blend shape of the mesh: its default weight (the file's)
    };

    // Empty while loading and after a failure; replaced by a reload (version() changes).
    std::span<const Part> parts() const { return m_parts; }
    // The skins of skinned parts: joint names and inverse bind matrices, for animation::AnimationSystem::addSkin.
    std::span<const assets::ModelSkin> skins() const { return m_skins; }
    // The blend shapes of its parts by name (a name the parts share is one), in the order the parts first have them,
    // and their default weights (of the first part that has each); for SceneAssets::setMorphWeights.
    std::span<const std::string> morphTargets() const { return m_morphTargets; }
    std::span<const float> morphDefaults() const { return m_morphDefaults; }
    glm::vec3 boundsMin() const { return m_boundsMin; }
    glm::vec3 boundsMax() const { return m_boundsMax; }
    std::uint32_t triangles() const;
    std::uint32_t lod() const { return m_lod; } // the level of detail that was uploaded
    std::uint32_t lodCount() const { return m_lodCount; }
    // The textures its materials use (loading on their own).
    std::span<const assets::AssetRef<TextureAsset>> textures() const { return m_textures; }

private:
    friend class SceneAssets;
    explicit ModelAsset(SceneAssets& owner);

    Result<void> decode(fs::Bytes bytes) override;
    std::uint64_t uploadBytes() const override;
    Result<void> finish() override;
    void unload() override;
    assets::AssetMemory residentMemory() const override;
    void retireParts();

    struct PendingMesh {
        std::string name;
        std::uint32_t material = 0;
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices; // of the chosen LOD
        int skin = -1;
        std::vector<SkinVertex> skinning;
        std::vector<MorphVertex> morphs;         // target-major
        std::vector<std::uint32_t> morphTargets; // per target of the mesh: its index in Pending::morphTargets
        std::vector<float> morphDefaults;
    };
    struct Pending {
        std::vector<PendingMesh> meshes;
        std::vector<std::string> morphTargets;
        std::vector<float> morphDefaults;
        std::vector<assets::ModelMaterial> materials;
        std::vector<assets::ModelSkin> skins;
        glm::vec3 boundsMin{0.0f}, boundsMax{0.0f};
        std::uint32_t lod = 0, lodCount = 0;
    };

    SceneAssets& m_owner;
    std::uint32_t m_wantedLod = 0;    // fixed at creation: decode() runs on a job
    std::optional<Pending> m_pending; // decoded on a job, uploaded by finish()
    std::vector<Part> m_parts;
    std::vector<assets::ModelSkin> m_skins;
    std::vector<std::string> m_morphTargets;
    std::vector<float> m_morphDefaults;
    std::vector<MaterialId> m_materials;
    std::vector<assets::AssetRef<TextureAsset>> m_textures;
    glm::vec3 m_boundsMin{0.0f}, m_boundsMax{0.0f};
    std::uint32_t m_lod = 0, m_lodCount = 0;
    std::uint64_t m_gpuBytes = 0;
};

struct SceneAssetsSettings {
    std::uint32_t textureSkipLevels = 0; // largest mip levels left out of the upload (lower quality, less memory)
    std::uint32_t modelLod = 0;          // level of detail of cooked models to upload (their last one if fewer)
    assets::AssetMemory textureBudget = assets::AssetManager::kUnlimited; // for unreferenced cached textures
    assets::AssetMemory modelBudget = assets::AssetManager::kUnlimited;
};

class SceneAssets {
public:
    SceneAssets(Scene& scene, assets::AssetManager& manager, const SceneAssetsSettings& settings = {});
    ~SceneAssets();
    SceneAssets(const SceneAssets&) = delete;
    SceneAssets& operator=(const SceneAssets&) = delete;

    assets::AssetRef<TextureAsset> loadTexture(std::string_view path, float priority = 0.0f);
    assets::AssetRef<ModelAsset> loadModel(std::string_view path, float priority = 0.0f);

    // A model placed in the scene; the instances follow its state (placeholder, meshes, rebuilt after a reload).
    ModelInstanceId addModel(assets::AssetRef<ModelAsset> model, const glm::mat4& transform, bool visible = true);
    void setTransform(ModelInstanceId instance, const glm::mat4& transform, bool teleport = false);
    void setVisible(ModelInstanceId instance, bool visible);
    // The joint matrices of one of the model's skins (ModelAsset::skins(), Scene::setJointPalette) for the parts that
    // use it; kept and applied again when the parts are rebuilt (loaded, reloaded) with a skin of as many joints.
    void setJointPalette(ModelInstanceId instance, int skin, std::span<const glm::mat4> palette, bool teleport = false);
    // The skinning of the parts that use one of the model's skins (Linear until set); kept like the palette.
    void setSkinning(ModelInstanceId instance, int skin, Skinning skinning);
    // The weights of the model's blend shapes, one per ModelAsset::morphTargets() (fewer: the rest at 0), for every
    // part that has them (Scene::setMorphWeights). Until the first call the parts show the file's default weights.
    // Kept by name and applied again when the parts are rebuilt (a reload); ignored while the model has no blend
    // shapes (also while it loads).
    void setMorphWeights(ModelInstanceId instance, std::span<const float> weights);
    void removeModel(ModelInstanceId instance);
    const assets::AssetRef<ModelAsset>& model(ModelInstanceId instance) const;
    std::uint32_t modelInstanceCount() const;

    // Once per frame after AssetManager::update(): updates placed models whose state changed and releases the meshes
    // and materials that reloads replaced.
    void update();

    Scene& scene() { return m_scene; }
    gpu::Device& device();
    assets::AssetManager& manager() { return m_manager; }
    const SceneAssetsSettings& settings() const { return m_settings; }
    // For textures and models loaded from now on.
    void setTextureSkipLevels(std::uint32_t levels) { m_settings.textureSkipLevels = levels; }
    void setModelLod(std::uint32_t lod) { m_settings.modelLod = lod; }
    // The device samples BC7 (otherwise cooked BC7 textures are decoded to RGBA8 on load).
    bool bcTextures() const { return m_support.bc7; }
    bool astcTextures() const { return m_support.astc4x4; }
    bool etc2Textures() const { return m_support.etc2; }
    // What Basis Universal files are transcoded to (assets::basisTarget) from gpu::Caps.
    const assets::TextureSupport& textureSupport() const { return m_support; }

private:
    friend class TextureAsset;
    friend class ModelAsset;
    struct Impl;

    void retire(MeshId mesh, MaterialId material);

    Scene& m_scene;
    assets::AssetManager& m_manager;
    SceneAssetsSettings m_settings;
    assets::TextureSupport m_support;
    bool m_loggedBasisRgba8 = false;
    std::unique_ptr<Impl> m_impl;
};

} // namespace quench::render
