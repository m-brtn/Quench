#pragma once

#include <quench/core/Jobs.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace quench {

// Jolt's job system on the engine's JobSystem, so physics shares the engine's worker threads instead of owning a pool.
// Queued Jolt jobs go into a queue of their own that up to maxConcurrency - 1 engine jobs ("drainers") empty; the
// thread that waits in step() runs jobs too (Jolt's barrier). One engine job per Jolt job would cost more: enkiTS
// wakes every sleeping worker for each new task, and a step queues dozens of small jobs.
// Needs worker threads (PhysicsWorld uses Jolt's single-threaded job system without them).
class JoltJobSystem final : public JPH::JobSystemWithBarrier {
public:
    JoltJobSystem(quench::JobSystem& jobs, JPH::uint maxJobs, JPH::uint maxBarriers, int maxConcurrency);
    // Waits for the drainers still running (they can outlast the barrier by a moment).
    ~JoltJobSystem() override;

    int GetMaxConcurrency() const override { return m_maxConcurrency; }
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                        JPH::uint32 dependencies = 0) override;

protected:
    void QueueJob(Job* job) override;
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;

private:
    void startDrainer();
    void drain();
    Job* pop();

    quench::JobSystem& m_jobs;
    int m_maxConcurrency;
    int m_maxDrainers;
    JPH::FixedSizeFreeList<Job> m_pool;
    std::mutex m_queueMutex;
    std::deque<Job*> m_queue;
    std::atomic<int> m_queued{0};
    std::atomic<int> m_drainers{0};
    std::atomic<int> m_running{0}; // drainer engine jobs not yet returned
};

} // namespace quench
