#include <quench/particles/Simulation.h>

#include <algorithm>

namespace quench::particles {

void Simulation::resize(std::uint32_t capacity) { m_slots.assign(capacity, Particle{}); }

void Simulation::clear() { std::fill(m_slots.begin(), m_slots.end(), Particle{}); }

void Simulation::step(const ParticleUniforms& u, std::span<const Particle> spawns) {
    if (u.clock.w > 0.5f) clear();
    const float dt = u.clock.x;
    const float time = u.clock.y;
    for (Particle& q : m_slots)
        if (q.b.w > 0.0f && q.a.w < q.b.w) q = shader::quenchParticleStep(u, q, dt, time);
    const auto capacity = std::uint32_t(m_slots.size());
    if (capacity == 0) return;
    // Spawn i of the step takes slot (first + i) mod capacity, its record at clock.z + i of the spawn buffer.
    const auto first = std::uint32_t(u.range.z);
    const auto count = std::min(std::uint32_t(u.range.w), capacity);
    const auto record = std::size_t(u.clock.z);
    for (std::uint32_t i = 0; i < count && record + i < spawns.size(); ++i)
        m_slots[(first + i) % capacity] = spawns[record + i];
}

void Simulation::look(const ParticleUniforms& u, std::span<ParticleInstance> out) const {
    const std::size_t n = std::min(out.size(), m_slots.size());
    for (std::size_t i = 0; i < n; ++i) out[i] = shader::quenchParticleLook(u, m_slots[i], u.clock.y);
}

std::uint32_t Simulation::alive() const {
    std::uint32_t n = 0;
    for (const Particle& q : m_slots) n += q.b.w > 0.0f && q.a.w < q.b.w ? 1u : 0u;
    return n;
}

} // namespace quench::particles
