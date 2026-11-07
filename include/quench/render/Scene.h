#pragma once

// The retained scene of the render module: meshes, textures, materials, instances and lights are registered once and
// referenced by plain handles; a frame only updates what changed (transforms, visibility). render::Renderer draws a
// Scene from a camera. The registered data is what later backends need as well (ray tracing builds its acceleration
// structures from the meshes and instances).
//
// Units: meters, linear HDR colors. Light values follow the shading of quench/Surface.hlsli: a directional light's
// color times intensity is the irradiance on a surface facing it (a white diffuse surface reflects 1 / pi of it), a
// point or spot light's is the irradiance at 1 m, an area light's is the radiance of its surface (as
// MaterialDesc::emissive: the camera sees its shape so); ambient values are the radiance of a uniform surround (a white
// surface reflects all of it).

#include <quench/assets/Texture.h>
#include <quench/core/Result.h>
#include <quench/render/Handles.h>
#include <quench/weather/Weather.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace quench::gpu {
class Device;
}

namespace quench::render {

class Renderer;

namespace detail {
struct SceneImpl;
}

// The engine's vertex (64 bytes, the layout of assets::ModelVertex), read by surface shaders as QuenchVertexIn
// (quench/Surface.hlsli).
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f}; // linear; multiplied with InstanceDesc::color and the material's base color
    // For normal maps. xyz: unit tangent along +u; w: +-1, the bitangent (toward -v, the normal map's +y as in glTF)
    // is w * cross(normal, tangent). MikkTSpace tangents from quench_cook or computeTangents (MeshBuilder.h).
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};
static_assert(sizeof(Vertex) == 64);

// The joints that move one vertex of a skinned mesh (the layout of assets::ModelSkinVertex): indices into the mesh's
// joints and their weights in 1/65535 (normalized to their sum when skinned).
struct SkinVertex {
    std::array<std::uint16_t, 4> joints{};
    std::array<std::uint16_t, 4> weights{65535, 0, 0, 0};
};
static_assert(sizeof(SkinVertex) == 16);

// One vertex's offsets in a blend shape (morph target; the layout of assets::ModelMorphVertex).
struct MorphVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec3 tangent{0.0f};
};
static_assert(sizeof(MorphVertex) == 36);

struct MeshDesc {
    std::span<const Vertex> vertices;
    std::span<const std::uint32_t> indices; // triangle list, counter-clockwise front faces
    std::string_view name{};                // debug name for GPU captures
    // A skinned mesh: one entry per vertex (vertices in the bind pose) and the number of joints its instances get
    // matrices for (Scene::setJointPalette). Empty: a rigid mesh.
    std::span<const SkinVertex> skin{};
    std::uint32_t jointCount = 0;
    // Blend shapes: morphTargetCount targets of one offset per vertex each, the first target's first
    // (morphs.size() == morphTargetCount * vertices.size()). Their instances get weights (Scene::setMorphWeights);
    // the renderer adds the weighted offsets to the vertices on the GPU before skinning, in the same pass.
    std::span<const MorphVertex> morphs{};
    std::uint32_t morphTargetCount = 0;
};

// RGBA8 pixel data, rows tightly packed, top row first.
struct TextureData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::byte> pixels;
    bool srgb = true;    // color (base color, emissive); false for data (metallic-roughness, masks)
    bool mipmaps = true; // generated on the GPU
    std::string_view name{};
};

enum class BlendMode : std::uint8_t {
    Opaque,     // depth write, drawn front to back by material
    AlphaBlend, // straight alpha (MaterialDesc::baseColor.a, the surface's opacity), drawn back to front
    Additive,   // color added, drawn back to front
};

// How a material casts the sun's shadows. Cut-out holes (MaterialDesc::alphaCutoff) let the light through in every
// mode that casts.
enum class ShadowMode : std::uint8_t {
    Auto,        // by the blend mode: Opaque is Solid, AlphaBlend Translucent, Additive (light) casts none
    None,        // no shadows
    Solid,       // blocks the light
    Translucent, // lets the light through by the surface's opacity, tinted by its hue
                 // (RenderSettings::translucentShadows)
};

struct MaterialDesc {
    SurfaceId surface{};       // Renderer::addSurface of the renderer that draws the scene; null: the standard surface
    glm::vec4 baseColor{1.0f}; // linear rgb, a: opacity
    glm::vec3 emissive{0.0f};  // linear radiance, added after lighting (bloom picks up values above about 1.6)
    float roughness = 0.5f;    // perceptual, 0..1
    float metallic = 0.0f;     // 0..1
    float alphaCutoff = 0.0f;  // > 0: pixels with a lower opacity are discarded, in the shadows too
    BlendMode blend = BlendMode::Opaque;
    bool doubleSided = false; // no back-face culling; back faces are shaded with the flipped normal
    ShadowMode shadows = ShadowMode::Auto;
    // The standard surface reads 0: base color (sRGB), 1: metallic-roughness (green: roughness, blue: metallic, as
    // in glTF), 2: normal map (linear, tangent space as in glTF; needs the vertex tangents), 3: emissive (sRGB,
    // multiplied with `emissive`). A custom surface reads them as quenchTexture0..3. Unset slots sample white.
    std::array<TextureId, 4> textures{};
    std::array<glm::vec4, 4> params{}; // MaterialUniforms::params, for custom surfaces
    std::string_view name{};
    // Anisotropic GGX (the standard surface, ray hits and the path tracer; KHR_materials_anisotropy's model): 0
    // isotropic, up to 1 where the roughness along the direction is 1 (brushed metal). The direction is the vertex
    // tangent (+u) turned by anisotropyRotation (radians) toward the bitangent; needs vertex tangents.
    float anisotropy = 0.0f;
    float anisotropyRotation = 0.0f;
};

// How a skinned mesh's vertices follow their joints. Linear blends the joints' matrices: cheap and exact for one joint,
// but a vertex between two joints twisted against each other moves toward the bone (a joint twisted by 180 degrees
// collapses to a line, the "candy wrapper"). Dual quaternion blends rigid motions: the limb keeps its thickness under
// twists and bends, and joints bulge slightly where they bend. Dual quaternions take a joint's rotation, translation
// and uniform scale (the cube root of its matrix's determinant, blended linearly); a non-uniform scale or a shear in
// a palette is lost.
enum class Skinning : std::uint8_t { Linear, DualQuaternion };

struct InstanceDesc {
    MeshId mesh{};
    MaterialId material{};
    glm::mat4 transform{1.0f}; // object -> world
    glm::vec4 color{1.0f};     // multiplies the vertex color (the standard surface: also the base color)
    glm::vec4 custom{0.0f};    // passed to the surface shader as QuenchSurfaceInput::custom
    bool visible = true;
    bool castsShadows = true;             // as the material's ShadowMode says; false: none
    Skinning skinning = Skinning::Linear; // for skinned meshes
};

// Rect, Disk and Tube are area lights: an emitting rectangle, disk or cylinder (a light strip, a neon tube).
enum class LightType : std::uint8_t { Directional, Point, Spot, Rect, Disk, Tube };

// The first directional light is the sun: it lights the scene with cascaded shadow maps (when castsShadows and the
// renderer has cascades) and places the sun in the sky. Further directional lights, point, spot and area lights light
// the surfaces; per frame the renderer uses at most 16 of them, the nearest to the camera, of which at most 8 area
// lights. Local lights have shadows where they are ray traced (RenderSettings::localLightShadows: the nearest ones
// with castsShadows; area lights soft by their area) and, without ray tracing, from shadow maps where
// RenderSettings::localShadowMapSize asks for them. A light with zero color or intensity is off (a sun that is off
// still places the sun disc and casts no shadows).
//
// Area lights are shaded with linearly transformed cosines (diffuse and specular, docs/render.md, "Area lights").
// The scene draws their shape with the light's radiance (an instance, a material and a mesh of its own, not in the
// counts or handles of the scene); a one-sided shape shows it from its front only, and it casts no shadows.
struct LightDesc {
    LightType type = LightType::Point;
    glm::vec3 color{1.0f}; // linear
    float intensity = 1.0f;
    glm::vec3 position{0.0f};               // area lights: the center
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; // the way the light shines (directional, spot; rect, disk: the normal)
    float range = 10.0f;      // local lights: the light fades to zero at this distance (m; area lights: from the shape)
    float innerAngle = 0.35f; // spot: half angle of the full-intensity cone (radians)
    float outerAngle = 0.5f;  // spot: half angle where the light ends
    bool castsShadows = true; // the sun; local lights with traced local light shadows or shadow maps
    float sourceRadius = 0.0f; // point, spot: the radius of the emitting sphere (m): soft traced shadows
    // Area lights (m): rect: width along `tangent` and height; disk: x the radius; tube: x the length along
    // `tangent`, y the radius.
    glm::vec2 size{1.0f, 1.0f};
    glm::vec3 tangent{1.0f, 0.0f, 0.0f}; // rect: the direction of its width; tube: its axis (unit, across direction)
    bool twoSided = false;               // rect, disk: light from both faces
};

// Sky, ambient light, fog and exposure. The defaults are a clear day with the sun about 25 degrees high
// (render::daylightFor() makes them for any sun elevation).
struct Environment {
    glm::vec3 skyZenith{0.17f, 0.42f, 1.10f};     // sky radiance overhead
    glm::vec3 skyHorizon{0.68f, 0.86f, 1.12f};    // sky radiance at the horizon
    glm::vec3 skyGround{0.25f, 0.27f, 0.30f};     // radiance below the horizon
    glm::vec3 sunGlow{2.6f, 2.4f, 2.1f};          // glow around the sun and haze toward it
    float atmosphere = 1.0f;                      // 0: the horizon-zenith gradient only, 1: single-scattering sky model
    glm::vec3 sunDisc{40.0f, 37.0f, 32.0f};       // radiance of the sun disc (0: no disc)
    float sunAngularRadius = 0.0175f;             // radians (stylized: the real sun has 0.0047)
    glm::vec3 skyAmbient{0.21f, 0.34f, 0.62f};    // ambient radiance from above
    glm::vec3 groundAmbient{0.18f, 0.15f, 0.11f}; // ambient radiance from below
    float fogDensity = 0.0f;                      // at fogBaseHeight (1/m); 0: no fog
    float fogFalloff = 0.08f;                     // density falls by e every 1 / falloff meters upward
    float fogBaseHeight = 0.0f;                   // m
    float fogMaxAmount = 1.0f;                    // 0..1
    float fogSunScatter = 0.6f;                   // 0..1: fog brightens toward the sun
    glm::vec3 fogColor{0.66f, 0.82f, 1.05f};      // fog radiance away from the sun
    float exposure = 1.0f;
};

class Scene {
public:
    // Meshes and textures are uploaded through the device; the scene must not outlive it.
    explicit Scene(gpu::Device& device);
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&& other) noexcept;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    ~Scene();

    gpu::Device& device() const;

    // Uploads the vertices and indices (without waiting for the GPU). Bounds for culling are computed here.
    Result<MeshId> addMesh(const MeshDesc& desc);
    // Instances that use the mesh must be removed first.
    void removeMesh(MeshId mesh);

    Result<TextureId> addTexture(const TextureData& data);
    // Pixel data with its mip levels (cooked KTX2 textures), in a format the device can sample.
    Result<TextureId> addTexture(const assets::TextureImage& image, std::string_view name = {});
    // A texture without pixels yet: materials sample it as white until setTexture() fills it (asset loading).
    TextureId addEmptyTexture();
    // New pixels (size and format may change) under the same handle; on an error the texture keeps its old ones.
    Result<void> setTexture(TextureId texture, const TextureData& data);
    Result<void> setTexture(TextureId texture, const assets::TextureImage& image, std::string_view name = {});
    // Materials that use the texture sample white afterwards.
    void removeTexture(TextureId texture);

    MaterialId addMaterial(const MaterialDesc& desc);
    void setMaterial(MaterialId material, const MaterialDesc& desc);
    // Instances that use the material must be removed or given another material first.
    void removeMaterial(MaterialId material);

    InstanceId addInstance(const InstanceDesc& desc);
    // The previous transform is kept for motion vectors (TAA); teleport: no motion from the old place (a respawn).
    void setTransform(InstanceId instance, const glm::mat4& transform, bool teleport = false);
    void setVisible(InstanceId instance, bool visible);
    void setInstanceMaterial(InstanceId instance, MaterialId material);
    void setInstanceColor(InstanceId instance, const glm::vec4& color, const glm::vec4& custom = glm::vec4(0.0f));
    // An instance of a skinned mesh: one matrix per joint of the mesh, each taking a bind-pose vertex to its posed
    // place in the mesh's space (the joint's model matrix times its inverse bind matrix, as
    // animation::AnimationSystem::skinPalette computes). Until the first call the instance shows the bind pose. The
    // renderer skins on the GPU (a compute pass), culls with bounds from the palette and measures motion vectors from
    // the previous palette, as setTransform does for the transform; teleport: no motion from the old pose.
    void setJointPalette(InstanceId instance, std::span<const glm::mat4> palette, bool teleport = false);
    void setSkinning(InstanceId instance, Skinning skinning);
    // The weights of the mesh's blend shapes, one per target (fewer: the rest 0); all 0 until the first call. A vertex
    // moves by the sum of its targets' offsets times their weights (any value, also negative or above 1); its normal
    // and tangent are the sums of theirs, normalized, the tangent made perpendicular to the normal. Targets at weight 0
    // are skipped: with every weight 0 the vertices are the mesh's own, bit for bit. The culling bounds grow by the
    // largest offsets times the weights; motion vectors follow the change from the previous weights.
    void setMorphWeights(InstanceId instance, std::span<const float> weights);
    void removeInstance(InstanceId instance);

    LightId addLight(const LightDesc& desc);
    void setLight(LightId light, const LightDesc& desc);
    void removeLight(LightId light);

    void setEnvironment(const Environment& environment);
    const Environment& environment() const;
    // The weather the renderer draws the clouds of (RenderSettings::clouds; docs/weather.md): a copy, so a game sets
    // it again after each Weather::update. Without one (the default, or after clearWeather) no clouds are drawn.
    void setWeather(const weather::Weather& weather);
    void clearWeather();
    const weather::Weather* weather() const; // null: none

    bool contains(MeshId mesh) const;
    bool contains(TextureId texture) const;
    bool contains(MaterialId material) const;
    bool contains(InstanceId instance) const;
    bool contains(LightId light) const;
    // Instances that use a mesh or a material (0 for a stale handle).
    std::uint32_t meshUsers(MeshId mesh) const;
    std::uint32_t materialUsers(MaterialId material) const;
    std::uint32_t jointCount(MeshId mesh) const;       // MeshDesc::jointCount; 0: rigid (or a stale handle)
    std::uint32_t morphTargetCount(MeshId mesh) const; // MeshDesc::morphTargetCount (0 for a stale handle)
    std::uint32_t meshCount() const;
    std::uint32_t textureCount() const;
    std::uint32_t materialCount() const;
    std::uint32_t instanceCount() const;
    std::uint32_t lightCount() const;

private:
    friend class Renderer;
    std::unique_ptr<detail::SceneImpl> m_impl;
};

} // namespace quench::render
