#pragma once

// Camera math of the render module.

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include <cstdint>

namespace quench::render {

// Right-handed perspective with reversed depth and no far plane: a point at the near plane maps to depth 1, a point
// at infinity to 0 (clip-space z range 0..1).
glm::mat4 reversedPerspective(float fovY, float aspect, float nearPlane);

// Sub-pixel offset of a TAA frame in pixels, -0.5..0.5: the Halton (2, 3) sequence over 8 frames (TrailForge).
glm::vec2 subpixelJitter(std::uint64_t frame);

} // namespace quench::render
