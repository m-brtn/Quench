#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

// The engine's one thread pool (enkiTS underneath). Every engine system that runs work off the calling thread uses it:
// asset reading and decoding, Jolt's physics jobs (PhysicsWorld), and game work. Worker threads get the priority of
// work the frame thread waits for (raiseWorkerThreadPriority()).
//
// Submitting (run, parallelFor) and waiting (wait, waitAll, runFor) is allowed on the thread that created the job
// system and inside jobs. Without worker threads (workerCount() == 0: builds without threads, or a desc asking for
// none) jobs run on the creating thread inside wait(), waitAll() and runFor(); code that must make progress across
// frames without blocking calls runFor() once per frame.
//
// Jobs must not throw: an exception leaving a job on a worker thread terminates the program.
namespace quench {

enum class JobPriority : std::uint8_t { High, Normal, Low };

namespace detail {
struct JobState;
}

// Handle to a submitted job: cheap to copy, keeps the job's bookkeeping alive (not the job system). An empty handle
// counts as done.
class Job {
public:
    Job() = default;

    // True once the job's function has returned on every range and its dependents are queued. Any thread.
    bool done() const;
    bool valid() const { return m_state != nullptr; }

private:
    friend class JobSystem;
    explicit Job(std::shared_ptr<detail::JobState> state) : m_state(std::move(state)) {}

    std::shared_ptr<detail::JobState> m_state;
};

struct JobSystemDesc {
    // -1: hardware threads - 1, at most 15; 0: no worker threads. Builds without threads have none either way.
    int workerThreads = -1;
};

class JobSystem {
public:
    explicit JobSystem(const JobSystemDesc& desc = {});
    // Waits for every queued job, then stops the workers.
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    int workerCount() const;
    int threadCount() const { return workerCount() + 1; }
    // 0 on the creating thread, 1..workerCount() on the workers, -1 on any other thread.
    int threadIndex() const;
    // Jobs submitted and not yet done.
    int pendingCount() const;

    // Runs `fn` once, after every job in `after` is done.
    Job run(std::function<void()> fn, JobPriority priority = JobPriority::Normal, std::span<const Job> after = {});
    // Calls fn(begin, end) over [0, count) in ranges of at least `minRange` items, spread over the threads.
    Job parallelFor(std::uint32_t count, std::uint32_t minRange, std::function<void(std::uint32_t, std::uint32_t)> fn,
                    JobPriority priority = JobPriority::Normal, std::span<const Job> after = {});
    // The same with a name for profiles (quench/core/Profile.h): each run of the job, or each range, is a zone with
    // it. A string that outlives the job (a literal); jobs without one are "job".
    Job run(const char* name, std::function<void()> fn, JobPriority priority = JobPriority::Normal,
            std::span<const Job> after = {});
    Job parallelFor(const char* name, std::uint32_t count, std::uint32_t minRange,
                    std::function<void(std::uint32_t, std::uint32_t)> fn, JobPriority priority = JobPriority::Normal,
                    std::span<const Job> after = {});

    // Returns when `job` is done, running queued jobs on the calling thread meanwhile.
    void wait(const Job& job);
    // Returns when every submitted job is done. Creating thread only.
    void waitAll();
    // Runs queued jobs on the creating thread for up to `seconds` (at least one job if any is queued); returns how
    // many ran. With worker threads it returns 0 at once: the workers make the progress.
    int runFor(double seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace quench
