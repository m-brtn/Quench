#pragma once

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemThreadPool.h>
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

// Owns a Jolt PhysicsSystem with its temp allocator and worker threads. Gravity is (0, -9.81, 0) m/s^2.
class PhysicsWorld {
public:
    struct Settings {
        JPH::uint maxBodies = 2048;
        JPH::uint maxBodyPairs = 4096;
        JPH::uint maxContactConstraints = 4096;
        int workerThreads = -1; // -1: hardware threads - 1, clamped to 1..4
        JPH::uint tempAllocatorBytes = 32 * 1024 * 1024;
    };

    PhysicsWorld();
    explicit PhysicsWorld(const Settings& settings);
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
    std::unique_ptr<JPH::JobSystemThreadPool> m_jobSystem;
    std::unique_ptr<JPH::PhysicsSystem> m_system;
};

} // namespace quench
