#include <quench/render/Projection.h>

#include <cmath>

namespace quench::render {

glm::mat4 reversedPerspective(float fovY, float aspect, float nearPlane) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    // clip.z = near, clip.w = -z_view: depth = near / distance.
    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = f;
    p[2][3] = -1.0f;
    p[3][2] = nearPlane;
    return p;
}

glm::vec2 subpixelJitter(std::uint64_t frame) {
    const auto halton = [](std::uint64_t i, std::uint64_t base) {
        float f = 1.0f, r = 0.0f;
        for (; i > 0; i /= base) {
            f /= float(base);
            r += f * float(i % base);
        }
        return r;
    };
    const std::uint64_t k = frame % 8 + 1;
    return {halton(k, 2) - 0.5f, halton(k, 3) - 0.5f};
}

} // namespace quench::render
