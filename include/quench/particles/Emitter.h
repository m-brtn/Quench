#pragma once

// A particle system's spawns (docs/particles.md, "Spawning"): its rate over time and over the distance it moves,
// bursts, and particles placed by the game (trails shed their sparkles this way), each spawned at its own time within
// a step and moved on to the step's end. Spawn k of a system has random numbers of its own (from the seed and k), so
// the same steps spawn the same particles on every run. Slots are a ring: spawn k takes slot k mod maxParticles,
// replacing the oldest particle there.

#include <quench/particles/Particles.h>
#include <quench/particles/Path.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace quench::particles {

class Emitter {
public:
    explicit Emitter(const ParticleDesc& desc = {});

    const ParticleDesc& desc() const { return m_desc; }
    // A new desc; a new maxParticles forgets the particles (restart).
    void setDesc(const ParticleDesc& desc);
    std::uint32_t capacity() const { return m_capacity; }

    // Where the emitter is (its space to world). Spawns between steps are placed along the motion from the last
    // step's transform; teleport: none (a jump, and no spawns per metre for it).
    void setTransform(const glm::mat4& transform, bool teleport = false);
    const glm::mat4& transform() const { return m_transform; }

    // Off: no spawns by rate or distance (bursts and emitAt still spawn); live particles go on.
    void setEmitting(bool on) { m_emitting = on; }
    bool emitting() const { return m_emitting; }
    // Times rate and ratePerMeter (quality presets: RenderSettings::particleDensity); not bursts or emitAt.
    void setRateScale(float scale) { m_rateScale = scale > 0.0f ? scale : 0.0f; }

    // `count` particles at the next step's end (at most maxParticles).
    void burst(std::uint32_t count);
    // One particle at each world position at the next step's end, with `velocity` plus the desc's start velocity
    // (a direction in the cone, a speed in the range); queued up to maxParticles.
    void emitAt(std::span<const glm::vec3> positions, const glm::vec3& velocity = glm::vec3(0.0f));

    // A step of `dt` s ending at `time` on the particles' clock (the renderer's: View::time modulo
    // QUENCH_PARTICLE_PERIOD): appends its spawns to `spawns`, each at the step's end, and fills the step's parts of
    // `u` (range: the slots they take; clock: dt and time; clock.w 1 after a restart). `u` holds the desc's parts
    // (uniformsOf) and the wind; at most maxParticles spawns (the newest).
    void step(float dt, float time, ParticleUniforms& u, std::vector<Particle>& spawns);

    // Forgets the particles: the next step clears every slot.
    void restart();
    // Spawns so far (also the next spawn's index).
    std::uint64_t spawned() const { return m_spawned; }
    // Particles alive after the last step (from their spawn and lifetime: exact for both simulations).
    std::uint32_t alive() const;

private:
    struct Queued {
        glm::vec3 position;
        glm::vec3 velocity;
    };
    // Spawn `index` at f (0..1) through the step, at `at` (world) or from the shape, moved to the step's end.
    Particle spawn(std::uint64_t index, float f, float dt, float time, const ParticleUniforms& u,
                   const Queued* at) const;
    glm::mat4 transformAt(float f) const;

    ParticleDesc m_desc;
    Path m_path;
    std::uint32_t m_capacity = 1;
    glm::mat4 m_transform{1.0f};
    glm::mat4 m_previous{1.0f};
    bool m_moved = false; // m_previous differs from m_transform
    bool m_emitting = true;
    bool m_restart = true;
    float m_rateScale = 1.0f;
    float m_rateAccumulator = 0.0f;
    float m_distanceAccumulator = 0.0f;
    std::uint32_t m_burst = 0;
    std::vector<Queued> m_queued;
    std::uint64_t m_spawned = 0;
    double m_clock = 0.0;
    std::vector<double> m_deaths; // per slot: when its particle dies on m_clock
};

} // namespace quench::particles
