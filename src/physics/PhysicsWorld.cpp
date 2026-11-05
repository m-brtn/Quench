#include <quench/physics/PhysicsWorld.h>

#include <quench/core/Log.h>

#include "physics/JoltJobSystem.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace quench {
namespace {

constexpr JPH::BroadPhaseLayer kBroadPhaseStatic{0};
constexpr JPH::BroadPhaseLayer kBroadPhaseMoving{1};

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == PhysicsLayers::kStatic ? kBroadPhaseStatic : kBroadPhaseMoving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == kBroadPhaseStatic ? "Static" : "Moving";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        return layer == PhysicsLayers::kMoving || bp == kBroadPhaseMoving;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == PhysicsLayers::kMoving || b == PhysicsLayers::kMoving;
    }
};

std::mutex g_runtimeMutex;
int g_runtimeRefs = 0;

void trace(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log::info("physics", "{}", buffer);
}

} // namespace

struct PhysicsWorld::Filters {
    BroadPhaseLayerMap broadPhaseLayers;
    ObjectVsBroadPhaseFilter objectVsBroadPhase;
    ObjectPairFilter objectPair;
};

JoltRuntime::JoltRuntime() {
    std::lock_guard lock(g_runtimeMutex);
    if (g_runtimeRefs++ == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

JoltRuntime::~JoltRuntime() {
    std::lock_guard lock(g_runtimeMutex);
    if (--g_runtimeRefs == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

PhysicsWorld::PhysicsWorld(JobSystem* jobs) : PhysicsWorld(jobs, Settings{}) {}

PhysicsWorld::PhysicsWorld(JobSystem* jobs, const Settings& settings)
    : m_filters(std::make_unique<Filters>()),
      m_tempAllocator(std::make_unique<JPH::TempAllocatorImpl>(settings.tempAllocatorBytes)),
      m_system(std::make_unique<JPH::PhysicsSystem>()) {
    if (!jobs || jobs->workerCount() == 0) {
        m_jobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    } else {
        int concurrency = settings.maxConcurrency < 0 ? std::min(jobs->threadCount(), 5) : settings.maxConcurrency;
        concurrency = std::clamp(concurrency, 1, jobs->threadCount());
        m_jobSystem =
            std::make_unique<JoltJobSystem>(*jobs, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, concurrency);
    }

    m_system->Init(settings.maxBodies, 0, settings.maxBodyPairs, settings.maxContactConstraints,
                   m_filters->broadPhaseLayers, m_filters->objectVsBroadPhase, m_filters->objectPair);
    m_system->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
}

PhysicsWorld::~PhysicsWorld() = default;

JPH::EPhysicsUpdateError PhysicsWorld::step(float dt, int collisionSteps) {
    return m_system->Update(dt, collisionSteps, m_tempAllocator.get(), m_jobSystem.get());
}

} // namespace quench
