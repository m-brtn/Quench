#pragma once

// A smooth curve through points (docs/particles.md, "Paths"): centripetal Catmull-Rom (no cusps or loops between
// close points), open or closed, measured by its length. Path emitters spawn along one; games move trail heads and
// light streams along them.

#include <glm/vec3.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace quench::particles {

// The centripetal Catmull-Rom segment from p1 (u 0) to p2 (u 1), p0 and p3 its neighbours.
glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float u);

class Path {
public:
    Path() = default;
    // At least two points (fewer: an empty path, or a point).
    explicit Path(std::span<const glm::vec3> points, bool closed = false);

    bool empty() const { return m_points.empty(); }
    bool closed() const { return m_closed; }
    float length() const { return m_lengths.empty() ? 0.0f : m_lengths.back(); }
    std::span<const glm::vec3> points() const { return m_points; }

    // The point `distance` m along it from its first point: wrapped on a closed path, clamped on an open one.
    glm::vec3 at(float distance) const;
    // The unit direction there.
    glm::vec3 tangent(float distance) const;

private:
    static constexpr std::size_t kSteps = 16; // length samples per segment
    std::size_t segments() const;
    glm::vec3 segmentPoint(std::size_t segment, float u) const;
    // Segment and u at a distance (already wrapped or clamped).
    void locate(float distance, std::size_t& segment, float& u) const;

    std::vector<glm::vec3> m_points;
    std::vector<float> m_lengths; // the length up to each sample (segments() * kSteps + 1)
    bool m_closed = false;
};

} // namespace quench::particles
