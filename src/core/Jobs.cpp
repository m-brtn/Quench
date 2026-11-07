#include <quench/core/Jobs.h>

#include <quench/core/Assert.h>
#include <quench/core/Log.h>
#include <quench/core/Profile.h>
#include <quench/core/Thread.h>

#include <TaskScheduler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace quench {

namespace detail {

struct JobCounters {
    std::atomic<int> pending{0};          // submitted, not done
    std::atomic<std::uint64_t> ranges{0}; // ranges executed, for runFor()
};

// One submitted job. Prerequisites are tracked here instead of with enkiTS dependencies: those are intrusive lists that
// a dependent has to unlink itself from when it is destroyed, which is not safe when two dependents of one job are
// destroyed on two threads. A job is added to enkiTS once its last prerequisite is done; a completion action
// (Releaser, the CompletionAction pattern of enkiTS's examples) then queues its dependents and drops the job's
// reference to itself.
struct JobState final : enki::ITaskSet {
    struct Releaser final : enki::ICompletable {
        JobState* owner = nullptr;
        void OnDependenciesComplete(enki::TaskScheduler* scheduler, std::uint32_t thread) override;
    };

    JobState(enki::TaskScheduler& scheduler, JobCounters& counters) : scheduler(scheduler), counters(counters) {
        releaser.owner = this;
        releaser.SetDependency(releaserDependency, this);
    }

    void ExecuteRange(enki::TaskSetPartition range, std::uint32_t) override {
        {
            QUENCH_PROFILE_ZONE_DYNAMIC(name ? name : "job");
            if (rangeFn)
                rangeFn(range.start, range.end);
            else
                fn();
        }
        counters.ranges.fetch_add(1, std::memory_order_relaxed);
    }

    enki::TaskScheduler& scheduler;
    JobCounters& counters;
    const char* name = nullptr;
    std::function<void()> fn;
    std::function<void(std::uint32_t, std::uint32_t)> rangeFn;

    std::mutex mutex;
    bool finished = false; // guarded by mutex; set together with taking the continuations
    std::vector<std::shared_ptr<JobState>> continuations;
    std::atomic<int> waitingOn{1};
    std::atomic<bool> launched{false};
    std::atomic<bool> done{false};
    std::atomic<bool> released{false}; // done and no longer in pendingCount(): what wait() waits for
    std::shared_ptr<JobState> self;    // set while the job is in enkiTS

    Releaser releaser;
    enki::Dependency releaserDependency;
};

namespace {

void launch(const std::shared_ptr<JobState>& state) {
    state->self = state;
    state->scheduler.AddTaskSetToPipe(state.get());
    state->launched.store(true, std::memory_order_release);
}

} // namespace

void JobState::Releaser::OnDependenciesComplete(enki::TaskScheduler* scheduler, std::uint32_t thread) {
    enki::ICompletable::OnDependenciesComplete(scheduler, thread);
    JobState* state = owner;
    std::vector<std::shared_ptr<JobState>> next;
    {
        std::lock_guard lock(state->mutex);
        state->finished = true;
        next.swap(state->continuations);
    }
    for (const std::shared_ptr<JobState>& job : next) {
        if (job->waitingOn.fetch_sub(1, std::memory_order_acq_rel) == 1) launch(job);
    }
    state->done.store(true, std::memory_order_release);
    JobCounters& counters = state->counters;
    std::shared_ptr<JobState> keep = std::move(state->self);
    counters.pending.fetch_sub(1, std::memory_order_acq_rel);
    state->released.store(true, std::memory_order_release);
    // May destroy the job and this releaser with it: nothing is touched after this line.
    keep.reset();
}

} // namespace detail

bool Job::done() const { return !m_state || m_state->done.load(std::memory_order_acquire); }

struct JobSystem::Impl {
    enki::TaskScheduler scheduler;
    detail::JobCounters counters;
    int workers = 0;

    Job submit(std::shared_ptr<detail::JobState> state, JobPriority priority, std::span<const Job> after) {
        switch (priority) {
        case JobPriority::High: state->m_Priority = enki::TASK_PRIORITY_HIGH; break;
        case JobPriority::Normal: state->m_Priority = enki::TASK_PRIORITY_MED; break;
        case JobPriority::Low: state->m_Priority = enki::TASK_PRIORITY_LOW; break;
        }
        counters.pending.fetch_add(1, std::memory_order_acq_rel);
        for (const Job& job : after) {
            if (!job.m_state) continue;
            std::lock_guard lock(job.m_state->mutex);
            if (!job.m_state->finished) {
                state->waitingOn.fetch_add(1, std::memory_order_relaxed);
                job.m_state->continuations.push_back(state);
            }
        }
        if (state->waitingOn.fetch_sub(1, std::memory_order_acq_rel) == 1) detail::launch(state);
        return Job(std::move(state));
    }
};

JobSystem::JobSystem(const JobSystemDesc& desc) : m_impl(std::make_unique<Impl>()) {
    int workers = desc.workerThreads;
    if (workers < 0) {
        const int hardware = int(std::thread::hardware_concurrency());
        workers = std::clamp(hardware - 1, 0, 15);
    }
    if (!threadsAvailable()) workers = 0;
    m_impl->workers = workers;

    enki::TaskSchedulerConfig config;
    config.numTaskThreadsToCreate = std::uint32_t(workers);
    config.numExternalTaskThreads = 0;
    config.profilerCallbacks.threadStart = [](std::uint32_t thread) {
        raiseWorkerThreadPriority();
        if constexpr (profile::kEnabled) {
            char name[32];
            std::snprintf(name, sizeof(name), "jobs.worker %u", unsigned(thread));
            QUENCH_PROFILE_THREAD_NAME(name);
        }
    };
    m_impl->scheduler.Initialize(config);
    log::info("jobs", "job system: {} worker threads", workers);
}

JobSystem::~JobSystem() { m_impl->scheduler.WaitforAllAndShutdown(); }

int JobSystem::workerCount() const { return m_impl->workers; }

int JobSystem::threadIndex() const {
    const std::uint32_t index = m_impl->scheduler.GetThreadNum();
    return index == enki::NO_THREAD_NUM ? -1 : int(index);
}

int JobSystem::pendingCount() const { return m_impl->counters.pending.load(std::memory_order_acquire); }

Job JobSystem::run(std::function<void()> fn, JobPriority priority, std::span<const Job> after) {
    return run(nullptr, std::move(fn), priority, after);
}

Job JobSystem::run(const char* name, std::function<void()> fn, JobPriority priority, std::span<const Job> after) {
    QUENCH_ASSERT(threadIndex() >= 0, "JobSystem::run from a thread that is neither the creating thread nor a worker");
    auto state = std::make_shared<detail::JobState>(m_impl->scheduler, m_impl->counters);
    state->name = name;
    state->fn = std::move(fn);
    state->m_SetSize = 1;
    return m_impl->submit(std::move(state), priority, after);
}

Job JobSystem::parallelFor(std::uint32_t count, std::uint32_t minRange,
                           std::function<void(std::uint32_t, std::uint32_t)> fn, JobPriority priority,
                           std::span<const Job> after) {
    return parallelFor(nullptr, count, minRange, std::move(fn), priority, after);
}

Job JobSystem::parallelFor(const char* name, std::uint32_t count, std::uint32_t minRange,
                           std::function<void(std::uint32_t, std::uint32_t)> fn, JobPriority priority,
                           std::span<const Job> after) {
    QUENCH_ASSERT(threadIndex() >= 0,
                  "JobSystem::parallelFor from a thread that is neither the creating thread nor a worker");
    auto state = std::make_shared<detail::JobState>(m_impl->scheduler, m_impl->counters);
    state->name = name;
    if (count == 0)
        state->rangeFn = [](std::uint32_t, std::uint32_t) {};
    else
        state->rangeFn = std::move(fn);
    state->m_SetSize = std::max<std::uint32_t>(count, 1);
    state->m_MinRange = std::max<std::uint32_t>(minRange, 1);
    return m_impl->submit(std::move(state), priority, after);
}

void JobSystem::wait(const Job& job) {
    QUENCH_ASSERT(threadIndex() >= 0, "JobSystem::wait from a thread that is neither the creating thread nor a worker");
    detail::JobState* state = job.m_state.get();
    if (!state) return;
    while (!state->released.load(std::memory_order_acquire)) {
        if (state->launched.load(std::memory_order_acquire))
            m_impl->scheduler.WaitforTask(state);
        else
            m_impl->scheduler.WaitforTask(nullptr);
        // The task can be complete while its releaser still runs on another thread for a moment.
        if (!state->released.load(std::memory_order_acquire)) std::this_thread::yield();
    }
}

void JobSystem::waitAll() {
    QUENCH_ASSERT(threadIndex() == 0, "JobSystem::waitAll from a thread other than the creating thread");
    while (m_impl->counters.pending.load(std::memory_order_acquire) > 0) m_impl->scheduler.WaitforAll();
}

int JobSystem::runFor(double seconds) {
    QUENCH_ASSERT(threadIndex() == 0, "JobSystem::runFor from a thread other than the creating thread");
    if (m_impl->workers > 0) return 0;
    const auto start = std::chrono::steady_clock::now();
    const int pendingBefore = m_impl->counters.pending.load(std::memory_order_acquire);
    while (m_impl->counters.pending.load(std::memory_order_acquire) > 0) {
        const std::uint64_t ranges = m_impl->counters.ranges.load(std::memory_order_relaxed);
        m_impl->scheduler.WaitforTask(nullptr);
        if (m_impl->counters.ranges.load(std::memory_order_relaxed) == ranges) break; // nothing runnable
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= seconds) break;
    }
    return std::max(0, pendingBefore - m_impl->counters.pending.load(std::memory_order_acquire));
}

} // namespace quench
