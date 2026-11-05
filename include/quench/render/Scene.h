#pragma once

// The retained scene of the render module: meshes, textures, materials, instances and lights are registered once and
// referenced by plain handles; a frame only updates what changed (transforms, visibility). render::Renderer draws a
// Scene from a camera. The registered data is what later backends need as well (ray tracing builds its acceleration
// structures from the meshes and instances).
//
// Units: meters, linear HDR colors. Light values follow the shading of quench/Surface.hlsli: a directional light's
// color times intensity is the irradiance on a surface facing it (a white diffuse surface reflects 1 / pi of it), a
// point or spot light's is the irradiance at 1 m; ambient values are the radiance of a uniform surround (a white
// surface reflects all of it).

#include <quench/core/Result.h>
#include <quench/render/Handles.h>

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

// The engine's vertex (48 bytes), read by surface shaders as QuenchVertexIn (quench/Surface.hlsli).
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 color{1.0f}; // linear; multiplied with InstanceDesc::color and the material's base color
};
static_assert(sizeof(Vertex) == 48);

struct MeshDesc {
    std::span<const Vertex> vertices;
    std::span<const std::uint32_t> indices; // triangle list, counter-clockwise front faces
    std::string_view name{};                // debug name for GPU captures
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

struct MaterialDesc {
    SurfaceId surface{};       // Renderer::addSurface of the renderer that draws the scene; null: the standard surface
    glm::vec4 baseColor{1.0f}; // linear rgb, a: opacity
    glm::vec3 emissive{0.0f};  // linear radiance, added after lighting (bloom picks up values above about 1.6)
    float roughness = 0.5f;    // perceptual, 0..1
    float metallic = 0.0f;     // 0..1
    float alphaCutoff = 0.0f;  // > 0: pixels with a lower opacity are discarded (the shadows stay solid)
    BlendMode blend = BlendMode::Opaque;
    bool doubleSided = false; // no back-face culling; back faces are shaded with the flipped normal
    bool castsShadows = true;
    // The standard surface reads 0: base color (sRGB), 1: metallic-roughness (green: roughness, blue: metallic, as
    // in glTF). A custom surface reads them as quenchTexture0..3. Unset slots sample white.
    std::array<TextureId, 4> textures{};
    std::array<glm::vec4, 4> params{}; // MaterialUniforms::params, for custom surfaces
    std::string_view name{};
};

struct InstanceDesc {
    MeshId mesh{};
    MaterialId material{};
    glm::mat4 transform{1.0f}; // object -> world
    glm::vec4 color{1.0f};     // multiplies the vertex color (the standard surface: also the base color)
    glm::vec4 custom{0.0f};    // passed to the surface shader as QuenchSurfaceInput::custom
    bool visible = true;
    bool castsShadows = true; // and the material's castsShadows
};

enum class LightType : std::uint8_t { Directional, Point, Spot };

// The first directional light is the sun: it lights the scene with cascaded shadow maps (when castsShadows and the
// renderer has cascades) and places the sun in the sky. Further directional lights, point and spot lights light the
// surfaces without shadows; per frame the renderer uses at most 16 of them, the nearest to the camera. A light with
// zero color or intensity is off (a sun that is off still places the sun disc and casts no shadows).
struct LightDesc {
    LightType type = LightType::Point;
    glm::vec3 color{1.0f}; // linear
    float intensity = 1.0f;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; // the way the light shines (directional, spot)
    float range = 10.0f;                    // point, spot: the light fades to zero at this distance (m)
    float innerAngle = 0.35f;               // spot: half angle of the full-intensity cone (radians)
    float outerAngle = 0.5f;                // spot: half angle where the light ends
    bool castsShadows = true;               // the sun only
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

    // Uploads the vertices and indices (without waiting for the GPU). Bounds for culling are computed here.
    Result<MeshId> addMesh(const MeshDesc& desc);
    // Instances that use the mesh must be removed first.
    void removeMesh(MeshId mesh);

    Result<TextureId> addTexture(const TextureData& data);
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
    void removeInstance(InstanceId instance);

    LightId addLight(const LightDesc& desc);
    void setLight(LightId light, const LightDesc& desc);
    void removeLight(LightId light);

    void setEnvironment(const Environment& environment);
    const Environment& environment() const;

    bool contains(MeshId mesh) const;
    bool contains(TextureId texture) const;
    bool contains(MaterialId material) const;
    bool contains(InstanceId instance) const;
    bool contains(LightId light) const;
    std::uint32_t meshCount() const;
    std::uint32_t materialCount() const;
    std::uint32_t instanceCount() const;
    std::uint32_t lightCount() const;

private:
    friend class Renderer;
    std::unique_ptr<detail::SceneImpl> m_impl;
};

} // namespace quench::render
