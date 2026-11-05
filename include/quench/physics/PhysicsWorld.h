#pragma once

#include <quench/core/Jobs.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <memory>

namespace quench {

// Object layers of the default collision setup: static bodies collide only with moving ones.
struct PhysicsLayers {
    static constexpr JPH::ObjectLayer kStatic = 0;
    static constexpr JPH::ObjectLayer kMoving = 1;
    static constexpr JPH::uint kCount = 2;
};

// RAII counter for the global Jolt registration (allocator, factory, types, trace output to quench::log): the first
// instance registers, the last one unregisters. Needed before any Jolt object is created (PhysicsMaterial too).
class JoltRuntime {
public:
    JoltRuntime();
    ~JoltRuntime();
    JoltRuntime(const JoltRuntime&) = delete;
    JoltRuntime& operator=(const JoltRuntime&) = delete;
};

// Owns a Jolt PhysicsSystem with its temp allocator. Jolt's jobs run on the engine's JobSystem (no thread pool of its
// own). Gravity is (0, -9.81, 0) m/s^2.
class PhysicsWorld {
public:
    struct Settings {
        JPH::uint maxBodies = 2048;
        JPH::uint maxBodyPairs = 4096;
        JPH::uint maxContactConstraints = 4096;
        // Threads working on one step at most (Jolt splits its work by this): -1 for the job system's threads, at
        // most 5.
        int maxConcurrency = -1;
        JPH::uint tempAllocatorBytes = 32 * 1024 * 1024;
    };

    // Jolt's jobs run on `jobs`, which must outlive the world. nullptr, or a job system without worker threads: they
    // run on the thread that calls step().
    explicit PhysicsWorld(JobSystem* jobs);
    PhysicsWorld(JobSystem* jobs, const Settings& settings);
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    JPH::PhysicsSystem& system() { return *m_system; }
    const JPH::PhysicsSystem& system() const { return *m_system; }
    JPH::BodyInterface& bodies() { return m_system->GetBodyInterface(); }
    // For Jolt calls that take them outside step() (character and vehicle updates, for example).
    JPH::TempAllocator& tempAllocator() { return *m_tempAllocator; }
    JPH::JobSystem& jobSystem() { return *m_jobSystem; }

    // One fixed step; returns Jolt's error flags.
    JPH::EPhysicsUpdateError step(float dt, int collisionSteps = 1);

    void optimizeBroadPhase() { m_system->OptimizeBroadPhase(); }

private:
    struct Filters;

    JoltRuntime m_runtime;
    std::unique_ptr<Filters> m_filters;
    std::unique_ptr<JPH::TempAllocatorImpl> m_tempAllocator;
    std::unique_ptr<JPH::JobSystem> m_jobSystem;
    std::unique_ptr<JPH::PhysicsSystem> m_system;
};

} // namespace quench
