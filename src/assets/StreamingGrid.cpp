#include <quench/assets/StreamingGrid.h>

#include <quench/core/Assert.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace quench::assets {

StreamingGrid::StreamingGrid(const Settings& settings) : m_settings(settings) {
    QUENCH_ASSERT(settings.cellsX > 0 && settings.cellsZ > 0 && settings.cellSize > 0.0f,
                  "StreamingGrid needs cells of a positive size");
    QUENCH_ASSERT(settings.keepRadius >= settings.loadRadius, "StreamingGrid: keepRadius below loadRadius");
    m_settings.cellsX = std::max(1, settings.cellsX);
    m_settings.cellsZ = std::max(1, settings.cellsZ);
    m_settings.keepRadius = std::max(settings.keepRadius, settings.loadRadius);
    m_active.assign(std::size_t(m_settings.cellsX) * std::size_t(m_settings.cellsZ), 0);
}

StreamingGrid::Changes StreamingGrid::update(std::span<const glm::vec2> focuses) {
    Changes changes;
    for (int z = 0; z < m_settings.cellsZ; ++z)
        for (int x = 0; x < m_settings.cellsX; ++x) {
            float nearest = std::numeric_limits<float>::infinity();
            for (glm::vec2 focus : focuses) nearest = std::min(nearest, distance(x, z, focus));
            std::uint8_t& active = m_active[std::size_t(z) * std::size_t(m_settings.cellsX) + std::size_t(x)];
            if (!active && nearest <= m_settings.loadRadius) {
                active = 1;
                changes.load.push_back({x, z, nearest});
            } else if (active && nearest > m_settings.keepRadius) {
                active = 0;
                changes.unload.push_back({x, z, nearest});
            }
        }
    const auto byDistance = [](const Cell& a, const Cell& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    };
    std::sort(changes.load.begin(), changes.load.end(), byDistance);
    std::sort(changes.unload.begin(), changes.unload.end(),
              [&](const Cell& a, const Cell& b) { return byDistance(b, a); });
    m_stats.loads += changes.load.size();
    m_stats.unloads += changes.unload.size();
    m_stats.active = m_stats.active + std::uint32_t(changes.load.size()) - std::uint32_t(changes.unload.size());
    return changes;
}

StreamingGrid::Changes StreamingGrid::clear() {
    Changes changes;
    for (int z = 0; z < m_settings.cellsZ; ++z)
        for (int x = 0; x < m_settings.cellsX; ++x) {
            std::uint8_t& active = m_active[std::size_t(z) * std::size_t(m_settings.cellsX) + std::size_t(x)];
            if (!active) continue;
            active = 0;
            changes.unload.push_back({x, z, 0.0f});
        }
    m_stats.unloads += changes.unload.size();
    m_stats.active = 0;
    return changes;
}

bool StreamingGrid::active(int x, int z) const {
    if (x < 0 || z < 0 || x >= m_settings.cellsX || z >= m_settings.cellsZ) return false;
    return m_active[std::size_t(z) * std::size_t(m_settings.cellsX) + std::size_t(x)] != 0;
}

float StreamingGrid::distance(int x, int z, glm::vec2 point) const {
    const glm::vec2 lo = cellMin(x, z);
    const float s = m_settings.cellSize;
    const float dx = std::max({lo.x - point.x, 0.0f, point.x - (lo.x + s)});
    const float dz = std::max({lo.y - point.y, 0.0f, point.y - (lo.y + s)});
    return std::hypot(dx, dz);
}

bool StreamingGrid::cellAt(glm::vec2 point, int& x, int& z) const {
    const float fx = std::floor((point.x - m_settings.origin.x) / m_settings.cellSize);
    const float fz = std::floor((point.y - m_settings.origin.y) / m_settings.cellSize);
    if (fx < 0.0f || fz < 0.0f || fx >= float(m_settings.cellsX) || fz >= float(m_settings.cellsZ)) return false;
    x = int(fx);
    z = int(fz);
    return true;
}

glm::vec2 StreamingGrid::cellMin(int x, int z) const {
    return m_settings.origin + glm::vec2(float(x), float(z)) * m_settings.cellSize;
}

glm::vec2 StreamingGrid::cellCenter(int x, int z) const {
    return cellMin(x, z) + glm::vec2(0.5f * m_settings.cellSize);
}

} // namespace quench::assets
