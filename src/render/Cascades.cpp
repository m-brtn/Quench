#include <quench/render/Cascades.h>

#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>

namespace quench::render {

Cascades computeCascades(const glm::mat4& view, float fovY, float aspect, float nearPlane, const glm::vec3& towardLight,
                         const CascadeSettings& settings) {
    Cascades out;
    out.count = std::clamp(settings.count, 0, kMaxCascades);
    if (out.count == 0 || settings.mapSize == 0) {
        out.count = 0;
        return out;
    }
    constexpr float kLogStart = 0.5f; // m: where the logarithmic split distribution starts
    const float farD = std::max(settings.distance, kLogStart * 2.0f);
    const glm::mat4 invView = glm::inverse(view);
    const float tanY = std::tan(fovY * 0.5f), tanX = tanY * aspect;
    const glm::vec3 up = std::abs(towardLight.y) > 0.98f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    float prev = nearPlane;
    for (int c = 0; c < out.count; ++c) {
        const float t = float(c + 1) / float(out.count);
        const float logD = kLogStart * std::pow(farD / kLogStart, t), linD = kLogStart + (farD - kLogStart) * t;
        const float split = glm::mix(linD, logD, settings.splitMix);
        // Bounding sphere of the frustum slice: its radius depends only on the slice, so it does not change when
        // the camera turns.
        glm::vec3 corners[8];
        int k = 0;
        for (const float d : {prev, split})
            for (const float sy : {-1.0f, 1.0f})
                for (const float sx : {-1.0f, 1.0f})
                    corners[k++] = glm::vec3(invView * glm::vec4(sx * tanX * d, sy * tanY * d, -d, 1.0f));
        glm::vec3 center(0.0f);
        for (const glm::vec3& p : corners) center += p;
        center /= 8.0f;
        float radius = 0.0f;
        for (const glm::vec3& p : corners) radius = std::max(radius, glm::length(p - center));
        radius = std::ceil(radius * 4.0f) / 4.0f;
        const float back = settings.casterDistance;
        const glm::mat4 lightView = glm::lookAt(center + towardLight * (radius + back), center, up);
        glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + back);
        // Snap the projection to whole shadow texels.
        const float halfSize = float(settings.mapSize) * 0.5f;
        const glm::vec4 origin = lightProj * lightView * glm::vec4(0, 0, 0, 1) * halfSize;
        const glm::vec2 offset = (glm::round(glm::vec2(origin)) - glm::vec2(origin)) / halfSize;
        lightProj[3][0] += offset.x;
        lightProj[3][1] += offset.y;
        out.viewProj[std::size_t(c)] = lightProj * lightView;
        out.splits[std::size_t(c)] = split;
        out.texel[std::size_t(c)] = 2.0f * radius / float(settings.mapSize);
        prev = split;
    }
    return out;
}

} // namespace quench::render
