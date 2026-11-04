#include "Test.h"

#include <quench/core/Thread.h>
#include <quench/physics/PhysicsWorld.h>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace quench;

namespace {

// A 20 x 1 x 20 m static box with its top face at y = 0 and a 0.5 m sphere dropped from `height`.
JPH::BodyID addScene(PhysicsWorld& world, float height) {
    JPH::BodyInterface& bodies = world.bodies();
    bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(10.0f, 0.5f, 10.0f)),
                                                      JPH::RVec3(0.0f, -0.5f, 0.0f), JPH::Quat::sIdentity(),
                                                      JPH::EMotionType::Static, PhysicsLayers::kStatic),
                            JPH::EActivation::DontActivate);
    return bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::SphereShape(0.5f), JPH::RVec3(0.0f, height, 0.0f),
                                                             JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic,
                                                             PhysicsLayers::kMoving),
                                   JPH::EActivation::Activate);
}

float settle(PhysicsWorld& world, JPH::BodyID sphere, double seconds) {
    const int steps = int(std::lround(seconds * 120.0));
    for (int i = 0; i < steps; ++i) world.step(1.0f / 120.0f);
    return float(world.bodies().GetCenterOfMassPosition(sphere).GetY());
}

// Jolt lets a resting contact sink by up to the penetration slop (0.02 m by default); how deep depends on the impact.
bool restsOnBox(PhysicsWorld& world, float y) {
    const float slop = world.system().GetPhysicsSettings().mPenetrationSlop;
    return y > 0.5f - slop - 0.001f && y < 0.5f + 0.001f;
}

} // namespace

QUENCH_TEST(physics_sphere_settles) {
    PhysicsWorld world;
    const JPH::BodyID sphere = addScene(world, 5.0f);
    world.optimizeBroadPhase();
    const float y = settle(world, sphere, 3.0);
    std::printf("    sphere centre after 3 s: y = %.4f m\n", y);
    CHECK(restsOnBox(world, y), "y = %.4f", y);
    CHECK(world.bodies().GetLinearVelocity(sphere).Length() < 0.05f, "still moving");
}

// The Jolt runtime is reference-counted: destroying one world keeps another one working.
QUENCH_TEST(physics_two_worlds) {
    PhysicsWorld kept;
    const JPH::BodyID sphere = addScene(kept, 2.0f);
    {
        PhysicsWorld other;
        addScene(other, 1.0f);
        other.step(1.0f / 120.0f);
    }
    const float y = settle(kept, sphere, 2.0);
    CHECK(restsOnBox(kept, y), "y = %.4f after the other world was destroyed", y);
}

// Jobs run on Jolt's worker threads with the worker priority (user-interactive QoS on Apple platforms). The main thread
// only polls, so it never runs a job itself.
QUENCH_TEST(physics_worker_priority) {
    {
        bool plain = false;
        std::thread([&] { plain = hasWorkerThreadPriority(); }).join();
        std::printf("    a plain std::thread has the worker priority: %s\n", plain ? "yes" : "no");
    }
    PhysicsWorld::Settings settings;
    settings.workerThreads = 2;
    PhysicsWorld world(settings);
    constexpr int kJobs = 64;
    std::atomic<int> done{0}, withPriority{0};
    std::vector<JPH::JobHandle> handles;
    for (int i = 0; i < kJobs; ++i)
        handles.push_back(world.jobSystem().CreateJob("priority check", JPH::Color::sGreen, [&] {
            if (hasWorkerThreadPriority()) withPriority.fetch_add(1);
            done.fetch_add(1);
        }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (done.load() < kJobs && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(done.load() == kJobs, "%d of %d jobs ran", done.load(), kJobs);
    CHECK(withPriority.load() == kJobs, "%d of %d jobs had the worker priority", withPriority.load(), kJobs);
}
