#include "physics/JoltJobSystem.h"

#include <quench/core/Assert.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace quench {
namespace {

// A drainer that finds the queue empty looks again this many times before it ends: Jolt queues its jobs in bursts
// as their dependencies complete, and a new drainer is a new engine job.
constexpr int kIdleSpins = 64;

} // namespace

JoltJobSystem::JoltJobSystem(quench::JobSystem& jobs, JPH::uint maxJobs, JPH::uint maxBarriers, int maxConcurrency)
    : JPH::JobSystemWithBarrier(maxBarriers), m_jobs(jobs), m_maxConcurrency(maxConcurrency),
      m_maxDrainers(std::max(1, maxConcurrency - 1)) {
    QUENCH_ASSERT(jobs.workerCount() > 0, "JoltJobSystem needs worker threads");
    m_pool.Init(maxJobs, maxJobs);
}

JoltJobSystem::~JoltJobSystem() {
    while (m_running.load(std::memory_order_acquire) > 0) std::this_thread::yield();
}

JPH::JobHandle JoltJobSystem::CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                                        JPH::uint32 dependencies) {
    JPH::uint32 index;
    for (;;) {
        index = m_pool.ConstructObject(name, color, this, function, dependencies);
        if (index != decltype(m_pool)::cInvalidObjectIndex) break;
        // Jolt sizes its job count for the worst case (cMaxPhysicsJobs); like its own pool, wait for a free one.
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    Job* job = &m_pool.Get(index);
    // The handle keeps a reference: the queued job may complete at once.
    JobHandle handle(job);
    if (dependencies == 0) QueueJob(job);
    return handle;
}

void JoltJobSystem::QueueJob(Job* job) { QueueJobs(&job, 1); }

void JoltJobSystem::QueueJobs(Job** jobs, JPH::uint count) {
    {
        std::lock_guard lock(m_queueMutex);
        for (JPH::uint i = 0; i < count; ++i) {
            jobs[i]->AddRef();
            m_queue.push_back(jobs[i]);
        }
    }
    m_queued.fetch_add(int(count), std::memory_order_seq_cst);
    for (JPH::uint i = 0; i < count; ++i) startDrainer();
}

void JoltJobSystem::FreeJob(Job* job) { m_pool.DestructObject(job); }

void JoltJobSystem::startDrainer() {
    int active = m_drainers.load(std::memory_order_seq_cst);
    while (active < m_maxDrainers) {
        if (m_drainers.compare_exchange_weak(active, active + 1, std::memory_order_seq_cst)) {
            m_running.fetch_add(1, std::memory_order_acq_rel);
            m_jobs.run(
                [this] {
                    drain();
                    m_running.fetch_sub(1, std::memory_order_acq_rel);
                },
                JobPriority::High);
            return;
        }
    }
}

JPH::JobSystem::Job* JoltJobSystem::pop() {
    std::lock_guard lock(m_queueMutex);
    if (m_queue.empty()) return nullptr;
    Job* job = m_queue.front();
    m_queue.pop_front();
    m_queued.fetch_sub(1, std::memory_order_seq_cst);
    return job;
}

void JoltJobSystem::drain() {
    for (;;) {
        int idle = 0;
        while (idle < kIdleSpins) {
            if (Job* job = pop()) {
                // A no-op when a thread waiting in Jolt's barrier has already run it.
                job->Execute();
                job->Release();
                idle = 0;
            } else {
                ++idle;
                std::this_thread::yield();
            }
        }
        m_drainers.fetch_sub(1, std::memory_order_seq_cst);
        // A job queued while this drainer was leaving may have found the drainer count full: take it on again.
        if (m_queued.load(std::memory_order_seq_cst) == 0) return;
        int active = m_drainers.load(std::memory_order_seq_cst);
        bool retaken = false;
        while (!retaken && active < m_maxDrainers)
            retaken = m_drainers.compare_exchange_weak(active, active + 1, std::memory_order_seq_cst);
        if (!retaken) return; // the drainers that are running take it
    }
}

} // namespace quench
