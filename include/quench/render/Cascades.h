#pragma once

// Cascaded shadow maps of a directional light (TrailForge's cascades): the view frustum is split with a mix of
// logarithmic and linear distances, each slice is covered by an orthographic light view around its bounding sphere,
// snapped to whole shadow texels so the shadows do not shimmer when the camera moves.

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace quench::render {

inline constexpr int kMaxCascades = 4;

struct CascadeSettings {
    int count = 3;                // 1..4
    float distance = 80.0f;       // m: the far end of the last cascade
    std::uint32_t mapSize = 2048; // texels per side of each cascade
    float splitMix = 0.72f;       // 0: linear splits, 1: logarithmic
    float casterDistance = 80.0f; // m: casters this far behind a slice (toward the light) still cast into it
};

struct Cascades {
    int count = 0;
    std::array<glm::mat4, kMaxCascades> viewProj{}; // world -> light clip space (standard depth: 0 near the light)
    std::array<float, kMaxCascades> splits{};       // view distance of the far end of each cascade (m)
    std::array<float, kMaxCascades> texel{};        // world size of one shadow texel (m)
};

// view: world -> view of the camera; towardLight: unit direction toward the light.
Cascades computeCascades(const glm::mat4& view, float fovY, float aspect, float nearPlane, const glm::vec3& towardLight,
                         const CascadeSettings& settings);

} // namespace quench::render
