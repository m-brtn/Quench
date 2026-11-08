#pragma once

// The CPU simulation of one particle system (docs/particles.md, "Simulation"): the ring of slots stepped as the
// render module's compute pass steps it on the GPU, with the same functions (quench/ParticleField.h). The renderer
// takes it where the device has no compute shaders (gpu::Caps::computeShaders) or RenderSettings::particles asks for
// it; tests and tools use it to see what the GPU computes.

#include <quench/particles/Particles.h>

#include <cstdint>
#include <span>
#include <vector>

namespace quench::particles {

class Simulation {
public:
    explicit Simulation(std::uint32_t capacity = 0) { resize(capacity); }

    // Empty slots.
    void resize(std::uint32_t capacity);
    void clear();
    std::uint32_t capacity() const { return std::uint32_t(m_slots.size()); }

    // One step, as Emitter::step filled `u` for it: every slot by u.clock.x (empty after a restart, u.clock.w),
    // then the step's spawns into their slots (u.range.zw), their records from u.clock.z in `spawns` (the buffer
    // Emitter::step appended them to).
    void step(const ParticleUniforms& u, std::span<const Particle> spawns);
    // What the draw reads of every slot at the clock u.clock.y (`out` holds capacity() instances).
    void look(const ParticleUniforms& u, std::span<ParticleInstance> out) const;

    std::span<const Particle> particles() const { return m_slots; }
    std::uint32_t alive() const;

private:
    std::vector<Particle> m_slots;
};

} // namespace quench::particles
