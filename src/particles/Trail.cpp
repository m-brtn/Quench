#include <quench/particles/Path.h>
#include <quench/particles/Trail.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace quench::particles {

Trail::Trail(const TrailDesc& desc) { setDesc(desc); }

void Trail::setDesc(const TrailDesc& desc) {
    const std::uint32_t capacity = std::max(desc.maxPoints, 2u);
    if (capacity != m_ring.size()) {
        std::vector<Point> ring(capacity);
        const std::uint32_t keep = std::min(m_count, capacity);
        for (std::uint32_t i = 0; i < keep; ++i) ring[keep - 1 - i] = point(i);
        m_ring = std::move(ring);
        m_first = 0;
        m_count = keep;
    }
    m_desc = desc;
    m_desc.maxPoints = capacity;
    m_desc.lifetime = std::max(desc.lifetime, 1e-3f);
    m_desc.spacing = std::max(desc.spacing, 1e-4f);
    m_desc.subdivisions = std::clamp(desc.subdivisions, 1u, 16u);
}

const Trail::Point& Trail::point(std::uint32_t newest) const {
    return m_ring[(m_first + m_count - 1 - newest) % m_ring.size()];
}

void Trail::push(const Point& p) {
    const auto size = std::uint32_t(m_ring.size());
    if (m_count == size) {
        m_first = (m_first + 1) % size;
        --m_count;
    }
    m_ring[(m_first + m_count) % size] = p;
    ++m_count;
}

void Trail::expire() {
    // The oldest point goes once it is past its life and the strip no longer ends between it and the next one: the
    // next is past its life too, or of another run, or there is no head after a lone point.
    const auto size = std::uint32_t(m_ring.size());
    while (m_count > 0) {
        const Point& oldest = point(m_count - 1);
        if (m_time - oldest.time < double(m_desc.lifetime)) break;
        bool drop;
        if (m_count >= 2) {
            const Point& next = point(m_count - 2);
            drop = next.run != oldest.run || m_time - next.time >= double(m_desc.lifetime);
        } else {
            drop = !(m_emitting && m_started && oldest.run == m_run);
        }
        if (!drop) break;
        m_first = (m_first + 1) % size;
        --m_count;
    }
}

void Trail::move(const glm::vec3& head, double time, bool teleport) {
    time = std::max(time, m_time);
    if (!m_emitting) {
        m_head = head;
        m_time = time;
        expire();
        return;
    }
    if (!m_started || teleport) {
        if (m_started) ++m_run;
        m_head = head;
        m_time = time;
        m_started = true;
        push({head, time, m_run});
        expire();
        return;
    }
    // Points every `spacing` m along the move from the last head, at the times the head passed them.
    const glm::vec3 from = m_head;
    const double t0 = m_time;
    const glm::vec3 delta = head - from;
    const float length = glm::length(delta);
    if (length > 0.0f) {
        const glm::vec3 last = m_count > 0 && point(0).run == m_run ? point(0).position : from;
        const float carried = glm::length(from - last);
        float s = std::max(m_desc.spacing - carried, 0.0f);
        std::uint32_t added = 0;
        while (s <= length && added < m_desc.maxPoints) {
            const float f = s / length;
            push({from + delta * f, t0 + (time - t0) * double(f), m_run});
            s += m_desc.spacing;
            ++added;
        }
    }
    m_head = head;
    m_time = time;
    expire();
}

void Trail::setEmitting(bool on) {
    if (on == m_emitting) return;
    if (!on && m_started && (m_count == 0 || point(0).run != m_run || glm::length(point(0).position - m_head) > 1e-5f))
        push({m_head, m_time, m_run}); // the strip ends where the head was
    m_emitting = on;
    m_started = false;
    ++m_run;
}

void Trail::clear() {
    m_first = 0;
    m_count = 0;
    m_started = false;
    ++m_run;
}

std::uint32_t Trail::maxVertices() const { return (m_count + 1) * m_desc.subdivisions + 2; }

bool Trail::visible(double time) const {
    const double life = double(m_desc.lifetime);
    if (m_emitting && m_started && m_count > 0) return true;
    return m_count >= 2 && time - point(0).time < life;
}

std::uint32_t Trail::build(double time, std::span<TrailVertex> out) const {
    if (out.empty() || m_count == 0) return 0;
    const bool withHead =
        m_emitting && m_started && !(point(0).run == m_run && glm::length(point(0).position - m_head) <= 1e-5f);
    const std::uint32_t n = m_count + (withHead ? 1u : 0u);
    const Point headPoint{m_head, m_time, m_run};
    const auto at = [&](std::uint32_t k) -> const Point& {
        if (withHead) return k == 0 ? headPoint : point(k - 1);
        return point(k);
    };
    const double life = double(m_desc.lifetime);
    const float maxLength = m_desc.maxLength;
    const glm::vec4 tint(m_desc.color, m_desc.opacity);
    std::uint32_t written = 0;
    bool breakNext = false;
    const auto emit = [&](const glm::vec3& p, float x) {
        const float half = 0.5f * m_desc.width * std::max(m_desc.widthOverAge(x), 0.0f);
        out[written++] = {glm::vec4(p, breakNext ? -half - 1e-7f : half), m_desc.colorOverAge(x) * tint};
        breakNext = false;
    };

    float length = 0.0f;
    glm::vec3 prevPos(0.0f);
    float prevX = 0.0f;
    bool runOpen = false; // the current run has written its first vertex
    for (std::uint32_t k = 0; k + 1 < n; ++k) {
        const Point& a = at(k);
        const Point& b = at(k + 1);
        if (a.run != b.run) {
            if (runOpen) breakNext = true;
            runOpen = false;
            continue;
        }
        const glm::vec3 p0 = k >= 1 && at(k - 1).run == a.run ? at(k - 1).position : a.position * 2.0f - b.position;
        const glm::vec3 p3 = k + 2 < n && at(k + 2).run == a.run ? at(k + 2).position : b.position * 2.0f - a.position;
        const float segment = glm::length(b.position - a.position);
        const auto steps =
            std::uint32_t(std::clamp(std::ceil(segment / (m_desc.spacing * 0.5f)), 1.0f, float(m_desc.subdivisions)));
        for (std::uint32_t s = runOpen ? 1u : 0u; s <= steps; ++s) {
            const float u = float(s) / float(steps);
            const glm::vec3 p = s == 0       ? a.position
                                : s == steps ? b.position
                                             : catmullRom(p0, a.position, b.position, p3, u);
            const double t = a.time + (b.time - a.time) * double(u);
            if (runOpen) length += glm::length(p - prevPos);
            float x = float((time - t) / life);
            if (maxLength > 0.0f) x = std::max(x, length / maxLength);
            x = std::max(x, 0.0f);
            if (x >= 1.0f) {
                // The end of the tail between the last vertex and this one.
                if (runOpen && x > prevX) {
                    const float f = (1.0f - prevX) / (x - prevX);
                    emit(prevPos + (p - prevPos) * f, 1.0f);
                }
                return written;
            }
            emit(p, x);
            if (written == out.size()) return written;
            prevPos = p;
            prevX = x;
            runOpen = true;
        }
    }
    return written;
}

} // namespace quench::particles
