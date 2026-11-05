#include "Test.h"

#include <quench/core/Jobs.h>
#include <quench/core/Thread.h>
#include <quench/physics/PhysicsWorld.h>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
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
    JobSystem jobs;
    PhysicsWorld world(&jobs);
    const JPH::BodyID sphere = addScene(world, 5.0f);
    world.optimizeBroadPhase();
    const float y = settle(world, sphere, 3.0);
    std::printf("    sphere centre after 3 s: y = %.4f m (%d job threads, Jolt concurrency %d)\n", y,
                jobs.threadCount(), world.jobSystem().GetMaxConcurrency());
    CHECK(restsOnBox(world, y), "y = %.4f", y);
    CHECK(world.bodies().GetLinearVelocity(sphere).Length() < 0.05f, "still moving");
}

// Without a job system (and with one without workers), Jolt's jobs run on the thread that calls step(), with the same
// result.
QUENCH_TEST(physics_single_threaded) {
    {
        PhysicsWorld world(nullptr);
        const JPH::BodyID sphere = addScene(world, 5.0f);
        world.optimizeBroadPhase();
        const float y = settle(world, sphere, 3.0);
        CHECK(world.jobSystem().GetMaxConcurrency() == 1, "max concurrency %d", world.jobSystem().GetMaxConcurrency());
        CHECK(restsOnBox(world, y), "y = %.4f", y);
    }
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    PhysicsWorld world(&jobs);
    const JPH::BodyID sphere = addScene(world, 5.0f);
    const float y = settle(world, sphere, 3.0);
    CHECK(world.jobSystem().GetMaxConcurrency() == 1, "max concurrency %d", world.jobSystem().GetMaxConcurrency());
    CHECK(restsOnBox(world, y), "y = %.4f", y);
}

// The Jolt runtime is reference-counted: destroying one world keeps another one working. Both share one job system.
QUENCH_TEST(physics_two_worlds) {
    JobSystem jobs;
    PhysicsWorld kept(&jobs);
    const JPH::BodyID sphere = addScene(kept, 2.0f);
    {
        PhysicsWorld other(&jobs);
        addScene(other, 1.0f);
        other.step(1.0f / 120.0f);
    }
    const float y = settle(kept, sphere, 2.0);
    CHECK(restsOnBox(kept, y), "y = %.4f after the other world was destroyed", y);
}

// Jolt's jobs run on the engine job system's workers, with the worker priority (user-interactive QoS on Apple
// platforms). The main thread only polls, so it never runs a job itself.
QUENCH_TEST(physics_worker_priority) {
    if (!threadsAvailable()) QUENCH_SKIP("no threads in this build");
    {
        bool plain = false;
        std::thread([&] { plain = hasWorkerThreadPriority(); }).join();
        std::printf("    a plain std::thread has the worker priority: %s\n", plain ? "yes" : "no");
    }
    JobSystem jobs(JobSystemDesc{.workerThreads = 2});
    PhysicsWorld world(&jobs);
    CHECK(world.jobSystem().GetMaxConcurrency() == 3, "max concurrency %d", world.jobSystem().GetMaxConcurrency());
    constexpr int kJobs = 64;
    std::atomic<int> done{0}, withPriority{0}, onWorkers{0};
    std::vector<JPH::JobHandle> handles;
    for (int i = 0; i < kJobs; ++i)
        handles.push_back(world.jobSystem().CreateJob("priority check", JPH::Color::sGreen, [&] {
            if (hasWorkerThreadPriority()) withPriority.fetch_add(1);
            if (jobs.threadIndex() >= 1) onWorkers.fetch_add(1);
            done.fetch_add(1);
        }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (done.load() < kJobs && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(done.load() == kJobs, "%d of %d jobs ran", done.load(), kJobs);
    CHECK(withPriority.load() == kJobs, "%d of %d jobs had the worker priority", withPriority.load(), kJobs);
    CHECK(onWorkers.load() == kJobs, "%d of %d jobs ran on the job system's workers", onWorkers.load(), kJobs);
}

// A pile of boxes steps on the shared pool while slow engine jobs keep workers busy: the step does not wait for them
// (Jolt's barrier runs the physics jobs on the stepping thread too) and the boxes come to rest on the floor.
QUENCH_TEST(physics_shared_pool) {
    if (!threadsAvailable()) QUENCH_SKIP("no threads in this build");
    JobSystem jobs(JobSystemDesc{.workerThreads = 3});
    PhysicsWorld world(&jobs);
    addScene(world, 1.0f);
    JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(JPH::Vec3(0.25f, 0.25f, 0.25f));
    std::vector<JPH::BodyID> boxes;
    for (int i = 0; i < 200; ++i)
        boxes.push_back(world.bodies().CreateAndAddBody(
            JPH::BodyCreationSettings(box,
                                      JPH::RVec3(float(i % 10) * 0.6f - 3.0f, 0.5f + float(i / 100) * 0.6f,
                                                 float((i / 10) % 10) * 0.6f - 3.0f),
                                      JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, PhysicsLayers::kMoving),
            JPH::EActivation::Activate));
    world.optimizeBroadPhase();

    std::atomic<int> background{0};
    std::vector<Job> others;
    for (int i = 0; i < 8; ++i)
        others.push_back(jobs.run(
            [&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                background.fetch_add(1);
            },
            JobPriority::Low));
    int errors = 0;
    for (int i = 0; i < 240; ++i)
        if (world.step(1.0f / 120.0f) != JPH::EPhysicsUpdateError::None) ++errors;
    for (const Job& job : others) jobs.wait(job);
    float lowest = 1e9f;
    for (JPH::BodyID id : boxes) lowest = std::min(lowest, float(world.bodies().GetCenterOfMassPosition(id).GetY()));
    CHECK(errors == 0, "%d steps reported errors", errors);
    CHECK(background.load() == 8, "%d of 8 other jobs ran", background.load());
    CHECK(lowest > 0.2f, "lowest box centre %.3f m", lowest);
}
