#pragma once

// An entity component system for a game's own objects (docs/ecs.md): enemies, projectiles, pickups, anything a game
// has many of and wants to process by what it is made of. Optional: nothing else in the engine uses it, and engine
// APIs keep taking plain handles, which a game stores in components.
//
// It is Flecs (https://github.com/SanderMertens/flecs), not a wrapper around it: `World::flecs()` is the Flecs world
// and a game uses Flecs's own C++ API for entities, components, queries and systems. What the engine adds:
//
// - Two ways to run systems, matching the game interface: systems of the fixed step (`.kind<ecs::FixedStep>()`) run in
//   `World::fixedStep(dt)`, called from `Game::fixedStep`; every other system (Flecs's phases, `flecs::OnUpdate` by
//   default) runs in `World::update(dt)`, called from `Game::update`. Systems of one phase run in the order they
//   were made.
// - Flecs's messages go to the engine's log (category "ecs").
// - Flecs starts no thread: its thread functions are taken out, so asking it for worker threads is an error it
//   reports. Systems run on the thread that calls fixedStep and update (a game's main thread). Work for other
//   threads goes to the engine's job system from inside a system.
// - Counters for a debug panel and for scenario values (`World::stats()`).
//
// Not thread-safe: one thread uses a world. The Flecs build has no reflection, JSON, script, REST or statistics
// addons (cmake/Dependencies.cmake).

#include <flecs.h>

#include <cstdint>

namespace quench::ecs {

// The phase of systems that run in World::fixedStep: `world.flecs().system<...>().kind<ecs::FixedStep>()`.
struct FixedStep {};

struct Stats {
    std::int32_t entities = 0;     // alive entities made since the world was created (components, systems included)
    std::int32_t tables = 0;       // Flecs's tables (one per combination of components in use)
    std::int32_t fixedSystems = 0; // systems of the fixed step
    std::int32_t systems = 0;      // all systems
    std::uint64_t fixedSteps = 0;  // calls of fixedStep
    std::uint64_t updates = 0;     // calls of update
    double fixedStepSeconds = 0.0; // wall time spent in fixedStep, summed over all calls
    double updateSeconds = 0.0;    // wall time spent in update, summed over all calls
};

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // The Flecs world: entities, components, queries and systems are made with its API.
    flecs::world& flecs() { return m_world; }
    const flecs::world& flecs() const { return m_world; }

    // Runs the systems of the fixed step once with this time step (seconds, above 0).
    void fixedStep(float dt);
    // Runs every other system once with this frame time (seconds, above 0).
    void update(float dt);

    Stats stats() const;

private:
    // First member: sets Flecs's OS layer (the log, no threads) before the first world is made.
    struct OsApi {
        OsApi();
    } m_osApi;
    flecs::world m_world;
    flecs::entity m_fixedPipeline;
    std::int32_t m_baseEntities = 0;
    std::uint64_t m_fixedSteps = 0;
    std::uint64_t m_updates = 0;
    double m_fixedStepSeconds = 0.0;
    double m_updateSeconds = 0.0;
};

} // namespace quench::ecs
