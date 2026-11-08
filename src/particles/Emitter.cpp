#include <quench/particles/Emitter.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>

namespace quench::particles {
namespace {

constexpr float kPi = 3.14159265f;

// lowbias32.
std::uint32_t hashBits(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// A spawn's random numbers: a PCG stream from the system's seed and the spawn's index.
class Rng {
public:
    Rng(std::uint32_t seed, std::uint64_t index)
        : m_state(hashBits(seed * 0x9e3779b9u ^
                           hashBits(std::uint32_t(index) ^ hashBits(std::uint32_t(index >> 32) + 0x85ebca6bu)))) {}
    float next() {
        m_state = m_state * 747796405u + 2891336453u;
        std::uint32_t word = ((m_state >> ((m_state >> 28u) + 4u)) ^ m_state) * 277803737u;
        word = (word >> 22u) ^ word;
        return float(word >> 8) * (1.0f / 16777216.0f);
    }

private:
    std::uint32_t m_state;
};

glm::vec3 unitSphere(float u, float v) {
    const float z = u * 2.0f - 1.0f;
    const float s = std::sqrt(std::max(1.0f - z * z, 0.0f));
    const float phi = 2.0f * kPi * v;
    return {s * std::cos(phi), z, s * std::sin(phi)};
}

// A direction in the cone of half angle `spread` around the unit `axis`, even over its solid angle.
glm::vec3 inCone(const glm::vec3& axis, float spread, float u, float v) {
    if (spread >= kPi - 1e-4f) return unitSphere(u, v);
    const float cosTheta = 1.0f - u * (1.0f - std::cos(std::max(spread, 0.0f)));
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
    const float phi = 2.0f * kPi * v;
    const glm::vec3 helper = std::abs(axis.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 t = glm::normalize(glm::cross(helper, axis));
    const glm::vec3 b = glm::cross(axis, t);
    return t * (sinTheta * std::cos(phi)) + b * (sinTheta * std::sin(phi)) + axis * cosTheta;
}

glm::vec3 normalizedOr(const glm::vec3& v, const glm::vec3& fallback) {
    const float l = glm::length(v);
    return l > 1e-6f ? v / l : fallback;
}

} // namespace

Emitter::Emitter(const ParticleDesc& desc) { setDesc(desc); }

void Emitter::setDesc(const ParticleDesc& desc) {
    const std::uint32_t capacity = std::clamp(desc.maxParticles, 1u, kMaxSystemParticles);
    m_desc = desc;
    m_desc.maxParticles = capacity;
    m_path = desc.shape == Shape::Path ? Path(desc.path, desc.pathClosed) : Path{};
    if (capacity != m_capacity || m_deaths.size() != capacity) {
        m_capacity = capacity;
        m_deaths.assign(capacity, 0.0);
        restart();
    }
    m_queued.reserve(capacity);
}

void Emitter::setTransform(const glm::mat4& transform, bool teleport) {
    if (teleport) {
        m_previous = transform;
        m_moved = false;
    } else if (!m_moved) {
        m_previous = m_transform;
        m_moved = transform != m_transform;
    }
    m_transform = transform;
}

void Emitter::burst(std::uint32_t count) { m_burst = std::min(m_burst + count, m_capacity); }

void Emitter::emitAt(std::span<const glm::vec3> positions, const glm::vec3& velocity) {
    for (const glm::vec3& p : positions) {
        if (m_queued.size() >= m_capacity) break;
        m_queued.push_back({p, velocity});
    }
}

void Emitter::restart() {
    m_restart = true;
    m_spawned = 0;
    m_rateAccumulator = 0.0f;
    m_distanceAccumulator = 0.0f;
    std::fill(m_deaths.begin(), m_deaths.end(), m_clock);
}

std::uint32_t Emitter::alive() const {
    std::uint32_t n = 0;
    for (const double death : m_deaths) n += death > m_clock ? 1u : 0u;
    return n;
}

glm::mat4 Emitter::transformAt(float f) const {
    if (!m_moved || f >= 1.0f) return m_transform;
    glm::mat4 m;
    for (int c = 0; c < 4; ++c) m[c] = m_previous[c] + (m_transform[c] - m_previous[c]) * f;
    return m;
}

Particle Emitter::spawn(std::uint64_t index, float f, float dt, float time, const ParticleUniforms& u,
                        const Queued* at) const {
    Rng rng(m_desc.seed, index);
    // Always the same draws in the same order, whatever the shape.
    float r[12];
    for (float& v : r) v = rng.next();
    const glm::vec3 e = m_desc.extent;
    glm::vec3 local(0.0f), centre(0.0f);
    switch (m_desc.shape) {
    case Shape::Point: break;
    case Shape::Sphere: local = unitSphere(r[0], r[1]) * (m_desc.surface ? e.x : e.x * std::cbrt(r[2])); break;
    case Shape::Box: local = (glm::vec3(r[0], r[1], r[2]) * 2.0f - 1.0f) * e; break;
    case Shape::Disc: {
        const float radius = m_desc.surface ? e.x : e.x * std::sqrt(r[0]);
        const float angle = 2.0f * kPi * r[1];
        local = glm::vec3(std::cos(angle) * radius, 0.0f, std::sin(angle) * radius);
        break;
    }
    case Shape::Path:
        if (!m_path.empty()) centre = m_path.at(r[3] * m_path.length());
        local = centre + unitSphere(r[0], r[1]) * (e.x * std::cbrt(r[2]));
        break;
    }
    const glm::vec3 axis = normalizedOr(m_desc.direction, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 dir = inCone(axis, m_desc.spread, r[4], r[5]);
    if (m_desc.outward > 0.0f && !at) {
        const glm::vec3 away = local - centre;
        if (glm::length(away) > 1e-6f) dir = normalizedOr(dir + (glm::normalize(away) - dir) * m_desc.outward, dir);
    }
    const glm::mat4 t = transformAt(f);
    const glm::vec3 position = at ? at->position : glm::vec3(t * glm::vec4(local, 1.0f));
    const glm::vec3 worldDir = normalizedOr(glm::mat3(t) * dir, dir);
    glm::vec3 velocity = worldDir * m_desc.speed.at(r[6]);
    if (at) {
        velocity += at->velocity;
    } else if (m_moved && dt > 0.0f && m_desc.inherit != 0.0f) {
        velocity += (glm::vec3(m_transform[3]) - glm::vec3(m_previous[3])) / dt * m_desc.inherit;
    }
    Particle q{};
    q.a = glm::vec4(position, 0.0f);
    q.b = glm::vec4(velocity, std::max(m_desc.lifetime.at(r[7]), 1e-3f));
    q.c = glm::vec4(r[8], std::max(m_desc.size.at(r[9]), 0.0f), m_desc.rotation.at(r[10]), m_desc.spin.at(r[11]));
    return shader::quenchParticleStep(u, q, (1.0f - f) * dt, time);
}

void Emitter::step(float dt, float time, ParticleUniforms& u, std::vector<Particle>& spawns) {
    dt = std::max(dt, 0.0f);
    const std::size_t first = spawns.size();
    u.range.y = float(m_capacity);
    u.clock = glm::vec4(dt, time, float(first), m_restart ? 1.0f : 0.0f);
    m_clock += dt;

    // How many of each kind, and where in the step the continuous ones fall: spawn j (1..n) of a rate where the
    // accumulator, which keeps the fraction left over from earlier steps, reaches j.
    const float rate = m_emitting ? std::max(m_desc.rate, 0.0f) * m_rateScale : 0.0f;
    const float rateTotal = m_rateAccumulator + rate * dt;
    const auto byRate = std::uint64_t(std::floor(rateTotal));
    const float distance = m_moved ? glm::length(glm::vec3(m_transform[3]) - glm::vec3(m_previous[3])) : 0.0f;
    const float perMeter = m_emitting ? std::max(m_desc.ratePerMeter, 0.0f) * m_rateScale : 0.0f;
    const float distanceTotal = m_distanceAccumulator + perMeter * distance;
    const auto byDistance = std::uint64_t(std::floor(distanceTotal));
    const std::uint64_t total = byRate + byDistance + m_burst + m_queued.size();
    const std::uint64_t skip = total > m_capacity ? total - m_capacity : 0;

    m_spawned += skip;
    u.range.z = float(m_spawned % m_capacity);
    u.range.w = float(total - skip);
    std::uint64_t i = 0;
    const auto add = [&](float f, const Queued* at) {
        if (i++ < skip) return;
        const Particle q = spawn(m_spawned, f, dt, time, u, at);
        m_deaths[std::size_t(m_spawned % m_capacity)] = m_clock + double(q.b.w) - double(q.a.w);
        spawns.push_back(q);
        ++m_spawned;
    };
    const float rateSpan = rate * dt;
    for (std::uint64_t j = 1; j <= byRate; ++j)
        add(std::clamp((float(j) - m_rateAccumulator) / rateSpan, 0.0f, 1.0f), nullptr);
    const float distanceSpan = perMeter * distance;
    for (std::uint64_t j = 1; j <= byDistance; ++j)
        add(std::clamp((float(j) - m_distanceAccumulator) / distanceSpan, 0.0f, 1.0f), nullptr);
    for (std::uint32_t j = 0; j < m_burst; ++j) add(1.0f, nullptr);
    for (const Queued& q : m_queued) add(1.0f, &q);

    m_rateAccumulator = rateTotal - float(byRate);
    m_distanceAccumulator = distanceTotal - float(byDistance);
    m_burst = 0;
    m_queued.clear();
    m_previous = m_transform;
    m_moved = false;
    m_restart = false;
}

} // namespace quench::particles
