#pragma once

// Decals: marks laid on flat surfaces of a scene (docs/render.md, "Decals"): a scorch where something burst, a
// splash, a ring, a painted stripe on a floor or a wall. Each is a thin lit quad of the scene with a soft-edged
// shape, so it takes the scene's light and shadows like the surface under it; it fades out and goes when its time is
// up, and the oldest goes when there are too many.
//
// These are flat decals: a quad on a plane the game names (a point and its normal). They do not wrap around
// corners or follow curved geometry: a mark that crosses an edge sticks out past it. For floors, walls and other
// flat faces.

#include <quench/render/Scene.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace quench::render {

// The shapes the engine makes itself (128 x 128 textures, white with the shape in alpha; made on first use).
enum class DecalShape : std::uint8_t {
    Disc,    // a soft-edged circle
    Ring,    // a circle's outline
    Splat,   // a blot with an uneven edge and a few drops around it
    Scorch,  // a burn: dark in the middle, ragged and fading outward
    Square,  // a rounded square with a crisp edge (a painted plate)
    Stripes, // diagonal hazard stripes in a rounded rectangle
    Count
};

struct DecalDesc {
    glm::vec3 position{0.0f};           // a point on the surface
    glm::vec3 normal{0.0f, 1.0f, 0.0f}; // the surface's normal there
    float rotation = 0.0f;              // radians around the normal
    glm::vec2 size{1.0f};               // m along its two sides
    DecalShape shape = DecalShape::Disc;
    TextureId texture{};    // the game's own picture instead of a shape (colour times its alpha)
    glm::vec4 color{1.0f};  // linear rgb and opacity
    float roughness = 0.9f; // of the mark: lower for something wet
    float life = 0.0f;      // seconds until it starts to fade; 0: it stays
    float fade = 1.0f;      // seconds it takes to fade out at the end of its life
    float lift = 0.004f;    // m above the surface, against flicker with it
};

using DecalId = std::uint32_t; // 0: none

class Decals {
public:
    // In a scene, at most `capacity` at once: one more replaces the oldest. The scene must outlive it.
    explicit Decals(Scene& scene, std::uint32_t capacity = 256);
    ~Decals();
    Decals(const Decals&) = delete;
    Decals& operator=(const Decals&) = delete;

    // Lays a decal; 0 when its mesh or texture could not be made (logged).
    DecalId add(const DecalDesc& desc);
    void remove(DecalId id);
    void clear();
    // Ages the decals that have a life: call once a frame with the frame's time.
    void update(float dt);
    std::uint32_t count() const { return std::uint32_t(m_decals.size()); }
    std::uint32_t capacity() const { return m_capacity; }
    // The current opacity of a decal (its colour's times its fade); 0 for one that is gone.
    float opacity(DecalId id) const;

private:
    struct Decal {
        DecalId id = 0;
        InstanceId instance;
        glm::vec4 color{1.0f};
        float age = 0.0f, life = 0.0f, fade = 1.0f;
    };
    MaterialId material(const DecalDesc& desc);

    Scene& m_scene;
    std::uint32_t m_capacity;
    MeshId m_quad{};
    std::array<TextureId, std::size_t(DecalShape::Count)> m_textures{};
    struct MaterialKey {
        std::uint32_t texture = 0;
        int roughness = 0; // in hundredths
        MaterialId material;
    };
    std::vector<MaterialKey> m_materials;
    std::vector<Decal> m_decals; // oldest first
    DecalId m_next = 1;
};

} // namespace quench::render
