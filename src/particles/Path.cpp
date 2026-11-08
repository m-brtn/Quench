#include <quench/particles/Path.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace quench::particles {

glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float u) {
    // Barry and Goldman's pyramid over knots spaced by the square root of the distances (centripetal).
    const auto knot = [](const glm::vec3& a, const glm::vec3& b) {
        return std::max(std::sqrt(glm::length(b - a)), 1e-4f);
    };
    const float t0 = 0.0f;
    const float t1 = t0 + knot(p0, p1);
    const float t2 = t1 + knot(p1, p2);
    const float t3 = t2 + knot(p2, p3);
    const float t = t1 + (t2 - t1) * u;
    const glm::vec3 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
    const glm::vec3 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
    const glm::vec3 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
    const glm::vec3 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
    const glm::vec3 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
    return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

Path::Path(std::span<const glm::vec3> points, bool closed) : m_points(points.begin(), points.end()), m_closed(closed) {
    if (m_points.size() < 2) {
        m_closed = false;
        if (!m_points.empty()) m_lengths.assign(1, 0.0f);
        return;
    }
    const std::size_t n = segments();
    m_lengths.reserve(n * kSteps + 1);
    m_lengths.push_back(0.0f);
    glm::vec3 last = m_points[0];
    float length = 0.0f;
    for (std::size_t s = 0; s < n; ++s)
        for (std::size_t k = 1; k <= kSteps; ++k) {
            const glm::vec3 p = segmentPoint(s, float(k) / float(kSteps));
            length += glm::length(p - last);
            m_lengths.push_back(length);
            last = p;
        }
}

std::size_t Path::segments() const {
    if (m_points.size() < 2) return 0;
    return m_closed ? m_points.size() : m_points.size() - 1;
}

glm::vec3 Path::segmentPoint(std::size_t segment, float u) const {
    const std::size_t n = m_points.size();
    const glm::vec3& p1 = m_points[segment % n];
    const glm::vec3& p2 = m_points[(segment + 1) % n];
    glm::vec3 p0, p3;
    if (m_closed) {
        p0 = m_points[(segment + n - 1) % n];
        p3 = m_points[(segment + 2) % n];
    } else {
        // Mirrored past the ends: the curve leaves the first point and reaches the last along their segments.
        p0 = segment > 0 ? m_points[segment - 1] : p1 * 2.0f - p2;
        p3 = segment + 2 < n ? m_points[segment + 2] : p2 * 2.0f - p1;
    }
    return catmullRom(p0, p1, p2, p3, u);
}

void Path::locate(float distance, std::size_t& segment, float& u) const {
    // The sample at or before the distance, then linear between it and the next.
    const auto it = std::upper_bound(m_lengths.begin(), m_lengths.end(), distance);
    std::size_t i = it == m_lengths.begin() ? 0 : std::size_t(it - m_lengths.begin()) - 1;
    i = std::min(i, m_lengths.size() - 2);
    const float span = m_lengths[i + 1] - m_lengths[i];
    const float f = span > 0.0f ? std::clamp((distance - m_lengths[i]) / span, 0.0f, 1.0f) : 0.0f;
    segment = i / kSteps;
    u = (float(i % kSteps) + f) / float(kSteps);
}

glm::vec3 Path::at(float distance) const {
    if (m_points.empty()) return glm::vec3(0.0f);
    if (m_points.size() == 1) return m_points[0];
    const float total = length();
    if (total <= 0.0f) return m_points[0];
    float d = distance;
    if (m_closed) {
        d = std::fmod(d, total);
        if (d < 0.0f) d += total;
    } else {
        d = std::clamp(d, 0.0f, total);
    }
    std::size_t segment = 0;
    float u = 0.0f;
    locate(d, segment, u);
    return segmentPoint(segment, u);
}

glm::vec3 Path::tangent(float distance) const {
    const float total = length();
    if (total <= 0.0f) return glm::vec3(0.0f, 0.0f, 1.0f);
    const float h = std::min(total * 1e-3f, 0.01f);
    float a = distance - h, b = distance + h;
    if (!m_closed) {
        a = std::clamp(a, 0.0f, total - 2.0f * h);
        b = a + 2.0f * h;
    }
    const glm::vec3 d = at(b) - at(a);
    const float l = glm::length(d);
    return l > 0.0f ? d / l : glm::vec3(0.0f, 0.0f, 1.0f);
}

} // namespace quench::particles
