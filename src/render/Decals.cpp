#include <quench/render/Decals.h>

#include <quench/core/Log.h>
#include <quench/render/MeshBuilder.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace quench::render {

namespace {

constexpr std::uint32_t kTextureSize = 128;

float smooth(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A fixed pattern of numbers in 0..1 (the same on every platform).
float hash(std::uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return float(n & 0xffffffu) / float(0x1000000u);
}

// A smooth bump of the radius around the circle: a few sines with fixed phases per seed.
float ragged(float angle, std::uint32_t seed, float amount) {
    float v = 0.0f;
    for (std::uint32_t k = 0; k < 5; ++k)
        v += std::sin(angle * float(2 + k * 2) + hash(seed + k) * 6.2831853f) / float(1 + k);
    return 1.0f + amount * v * 0.5f;
}

// The shape's texel at uv in -1..1: its brightness (rgb) and its opacity.
void shapeAt(DecalShape shape, float x, float y, float& value, float& alpha) {
    const float r = std::sqrt(x * x + y * y);
    const float angle = std::atan2(y, x);
    value = 1.0f;
    switch (shape) {
    case DecalShape::Disc: alpha = 1.0f - smooth(0.82f, 0.98f, r); break;
    case DecalShape::Ring: alpha = smooth(0.66f, 0.74f, r) * (1.0f - smooth(0.88f, 0.96f, r)); break;
    case DecalShape::Splat: {
        const float edge = 0.62f * ragged(angle, 11u, 0.35f);
        alpha = 1.0f - smooth(edge - 0.06f, edge + 0.02f, r);
        // Drops thrown out around it.
        for (std::uint32_t k = 0; k < 9; ++k) {
            const float a = hash(100u + k) * 6.2831853f, d = 0.7f + 0.22f * hash(200u + k);
            const float size = 0.04f + 0.07f * hash(300u + k);
            const float dx = x - std::cos(a) * d, dy = y - std::sin(a) * d;
            alpha = std::max(alpha, 1.0f - smooth(size * 0.7f, size, std::sqrt(dx * dx + dy * dy)));
        }
        break;
    }
    case DecalShape::Scorch: {
        const float edge = 0.8f * ragged(angle, 31u, 0.3f);
        alpha = (1.0f - smooth(0.25f * edge, edge, r)) * 0.95f;
        // Charred in the middle, lighter soot toward the edge.
        value = 0.25f + 0.75f * smooth(0.0f, 0.7f, r);
        break;
    }
    case DecalShape::Square: {
        const float d = std::max(std::abs(x), std::abs(y));
        const float corner = std::sqrt(std::max(std::abs(x) - 0.7f, 0.0f) * std::max(std::abs(x) - 0.7f, 0.0f) +
                                       std::max(std::abs(y) - 0.7f, 0.0f) * std::max(std::abs(y) - 0.7f, 0.0f));
        alpha = d <= 0.7f ? 1.0f : 1.0f - smooth(0.18f, 0.24f, corner);
        break;
    }
    case DecalShape::Stripes: {
        const float d = std::max(std::abs(x), std::abs(y));
        const float band = std::fmod((x + y) * 2.5f + 100.0f, 1.0f);
        alpha = (1.0f - smooth(0.88f, 0.94f, d)) * (band < 0.5f ? 1.0f : 0.0f);
        break;
    }
    case DecalShape::Count: alpha = 0.0f; break;
    }
}

} // namespace

Decals::Decals(Scene& scene, std::uint32_t capacity) : m_scene(scene), m_capacity(std::max(capacity, 1u)) {}

Decals::~Decals() { clear(); }

MaterialId Decals::material(const DecalDesc& desc) {
    TextureId texture = desc.texture;
    if (!texture) {
        TextureId& made = m_textures[std::size_t(desc.shape)];
        if (!made) {
            std::vector<std::byte> pixels(std::size_t(kTextureSize) * kTextureSize * 4);
            for (std::uint32_t y = 0; y < kTextureSize; ++y)
                for (std::uint32_t x = 0; x < kTextureSize; ++x) {
                    float value = 1.0f, alpha = 0.0f;
                    shapeAt(desc.shape, (float(x) + 0.5f) / float(kTextureSize) * 2.0f - 1.0f,
                            (float(y) + 0.5f) / float(kTextureSize) * 2.0f - 1.0f, value, alpha);
                    std::byte* p = &pixels[(std::size_t(y) * kTextureSize + x) * 4];
                    const auto level = std::byte(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                    p[0] = p[1] = p[2] = level;
                    p[3] = std::byte(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            Result<TextureId> result = m_scene.addTexture(
                {.width = kTextureSize, .height = kTextureSize, .pixels = pixels, .srgb = true, .name = "decal shape"});
            if (!result) {
                log::error("render", "decal shape texture: {}", result.error());
                return {};
            }
            made = *result;
        }
        texture = made;
    }
    const int roughness = int(std::clamp(desc.roughness, 0.0f, 1.0f) * 100.0f + 0.5f);
    for (const MaterialKey& key : m_materials)
        if (key.texture == texture.index && key.roughness == roughness) return key.material;
    MaterialDesc material{.baseColor = glm::vec4(1.0f),
                          .roughness = float(roughness) / 100.0f,
                          .blend = BlendMode::AlphaBlend,
                          .shadows = ShadowMode::None,
                          .name = "decal"};
    material.textures[0] = texture;
    const MaterialId id = m_scene.addMaterial(material);
    m_materials.push_back({texture.index, roughness, id});
    return id;
}

DecalId Decals::add(const DecalDesc& desc) {
    if (!m_quad) {
        Result<MeshId> quad = m_scene.addMesh(planeMesh(glm::vec2(0.5f)).desc("decal"));
        if (!quad) {
            log::error("render", "decal mesh: {}", quad.error());
            return 0;
        }
        m_quad = *quad;
    }
    const MaterialId materialId = material(desc);
    if (!materialId) return 0;
    if (m_decals.size() >= m_capacity) remove(m_decals.front().id);

    // The quad lies in its own xz plane facing +y: y goes to the normal, x and z to the surface, turned around it.
    const glm::vec3 normal = glm::length(desc.normal) > 1.0e-5f ? glm::normalize(desc.normal) : glm::vec3(0, 1, 0);
    const glm::vec3 reference = std::abs(normal.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 tangent = glm::normalize(glm::cross(reference, normal));
    const glm::vec3 bitangent = glm::cross(tangent, normal); // x, y (the normal), z: right-handed, the face up
    const float c = std::cos(desc.rotation), s = std::sin(desc.rotation);
    const glm::vec3 x = tangent * c + bitangent * s, z = bitangent * c - tangent * s;
    glm::mat4 transform(1.0f);
    transform[0] = glm::vec4(x * desc.size.x, 0.0f);
    transform[1] = glm::vec4(normal, 0.0f);
    transform[2] = glm::vec4(z * desc.size.y, 0.0f);
    transform[3] = glm::vec4(desc.position + normal * desc.lift, 1.0f);

    Decal decal;
    decal.id = m_next++;
    if (m_next == 0) m_next = 1;
    decal.color = desc.color;
    decal.life = desc.life;
    decal.fade = std::max(desc.fade, 1.0e-3f);
    decal.instance = m_scene.addInstance(
        {.mesh = m_quad, .material = materialId, .transform = transform, .color = desc.color, .castsShadows = false});
    m_decals.push_back(decal);
    return decal.id;
}

void Decals::remove(DecalId id) {
    const auto found = std::find_if(m_decals.begin(), m_decals.end(), [id](const Decal& d) { return d.id == id; });
    if (found == m_decals.end()) return;
    if (m_scene.contains(found->instance)) m_scene.removeInstance(found->instance);
    m_decals.erase(found);
}

void Decals::clear() {
    for (const Decal& decal : m_decals)
        if (m_scene.contains(decal.instance)) m_scene.removeInstance(decal.instance);
    m_decals.clear();
}

void Decals::update(float dt) {
    for (Decal& decal : m_decals) {
        if (decal.life <= 0.0f) continue;
        decal.age += dt;
        if (decal.age <= decal.life) continue;
        glm::vec4 color = decal.color;
        color.a *= std::clamp(1.0f - (decal.age - decal.life) / decal.fade, 0.0f, 1.0f);
        m_scene.setInstanceColor(decal.instance, color);
    }
    // Those that have faded out go.
    for (std::size_t i = m_decals.size(); i-- > 0;) {
        const Decal& decal = m_decals[i];
        if (decal.life > 0.0f && decal.age >= decal.life + decal.fade) remove(decal.id);
    }
}

float Decals::opacity(DecalId id) const {
    for (const Decal& decal : m_decals) {
        if (decal.id != id) continue;
        if (decal.life <= 0.0f || decal.age <= decal.life) return decal.color.a;
        return decal.color.a * std::clamp(1.0f - (decal.age - decal.life) / decal.fade, 0.0f, 1.0f);
    }
    return 0.0f;
}

} // namespace quench::render
