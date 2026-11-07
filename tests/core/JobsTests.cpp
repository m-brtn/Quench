#include "Test.h"

#include <quench/core/Jobs.h>
#include <quench/core/Thread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

using namespace quench;

namespace {

// Each case runs with worker threads (where the build has threads) and without any.
template <class Fn>
void forEachMode(Fn fn) {
    if (threadsAvailable()) {
        JobSystem jobs(JobSystemDesc{.workerThreads = 3});
        fn(jobs);
    }
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    fn(jobs);
}

} // namespace

QUENCH_TEST(jobs_run_and_wait) {
    forEachMode([](JobSystem& jobs) {
        std::atomic<int> sum{0};
        std::vector<Job> handles;
        for (int i = 1; i <= 100; ++i) handles.push_back(jobs.run([&sum, i] { sum.fetch_add(i); }));
        for (const Job& job : handles) jobs.wait(job);
        CHECK(sum.load() == 5050, "sum %d with %d workers", sum.load(), jobs.workerCount());
        for (const Job& job : handles) CHECK(job.done(), "a waited job is not done");
        CHECK(jobs.pendingCount() == 0, "pending %d", jobs.pendingCount());
        CHECK(Job().done(), "an empty handle is not done");
    });
}

// A diamond A -> (B, C) -> D: B and C see A's result, D sees both.
QUENCH_TEST(jobs_dependencies) {
    forEachMode([](JobSystem& jobs) {
        for (int round = 0; round < 50; ++round) {
            std::atomic<int> a{0}, b{0}, c{0}, d{0};
            Job ja = jobs.run([&] { a = 1; });
            Job jb = jobs.run([&] { b = a.load() + 1; }, JobPriority::Normal, std::span(&ja, 1));
            Job jc = jobs.run([&] { c = a.load() + 2; }, JobPriority::High, std::span(&ja, 1));
            const Job both[] = {jb, jc};
            Job jd = jobs.run([&] { d = b.load() * 10 + c.load(); }, JobPriority::Low, both);
            jobs.wait(jd);
            CHECK(d.load() == 23, "round %d: d = %d (%d workers)", round, d.load(), jobs.workerCount());
        }
        // A prerequisite that is already done does not hold the dependent back.
        Job first = jobs.run([] {});
        jobs.wait(first);
        std::atomic<bool> ran{false};
        Job second = jobs.run([&] { ran = true; }, JobPriority::Normal, std::span(&first, 1));
        jobs.wait(second);
        CHECK(ran.load(), "dependent of a done job did not run");
    });
}

QUENCH_TEST(jobs_parallel_for) {
    forEachMode([](JobSystem& jobs) {
        std::vector<int> values(10000, 0);
        Job job = jobs.parallelFor(std::uint32_t(values.size()), 64, [&](std::uint32_t begin, std::uint32_t end) {
            for (std::uint32_t i = begin; i < end; ++i) values[i] = int(i) * 2;
        });
        jobs.wait(job);
        long long sum = std::accumulate(values.begin(), values.end(), 0LL);
        CHECK(sum == 9999LL * 10000LL, "sum %lld", sum);
        Job empty = jobs.parallelFor(0, 1, [](std::uint32_t, std::uint32_t) {});
        jobs.wait(empty);
        CHECK(empty.done(), "an empty parallelFor is not done");
    });
}

// Named jobs (zones in a profile) run like the others.
QUENCH_TEST(jobs_named) {
    forEachMode([](JobSystem& jobs) {
        std::atomic<int> count{0};
        Job first = jobs.run("test.first", [&] { count.fetch_add(1); });
        Job ranges = jobs.parallelFor(
            "test.ranges", 1000, 16, [&](std::uint32_t begin, std::uint32_t end) { count.fetch_add(int(end - begin)); },
            JobPriority::High, std::span(&first, 1));
        jobs.wait(ranges);
        CHECK(first.done() && count.load() == 1001, "count %d with %d workers", count.load(), jobs.workerCount());
    });
}

// Jobs submit jobs; waitAll() returns when the whole tree is done.
QUENCH_TEST(jobs_nested_and_wait_all) {
    forEachMode([](JobSystem& jobs) {
        std::atomic<int> leaves{0};
        for (int i = 0; i < 8; ++i)
            jobs.run([&] {
                for (int j = 0; j < 8; ++j) jobs.run([&] { leaves.fetch_add(1); });
            });
        jobs.waitAll();
        CHECK(leaves.load() == 64, "%d of 64 nested jobs ran", leaves.load());
        CHECK(jobs.pendingCount() == 0, "pending %d", jobs.pendingCount());
    });
}

// Dropping every handle does not cancel or leak a job; destroying the job system waits for queued jobs.
QUENCH_TEST(jobs_fire_and_forget) {
    std::atomic<int> count{0};
    {
        JobSystem jobs(JobSystemDesc{.workerThreads = threadsAvailable() ? 2 : 0});
        for (int i = 0; i < 200; ++i) jobs.run([&] { count.fetch_add(1); });
    }
    CHECK(count.load() == 200, "%d of 200 jobs ran before the job system was destroyed", count.load());
}

// Without workers, runFor() makes progress across frames without blocking for longer than asked (plus one job).
QUENCH_TEST(jobs_run_for_without_workers) {
    JobSystem jobs(JobSystemDesc{.workerThreads = 0});
    CHECK(jobs.workerCount() == 0 && jobs.threadCount() == 1, "workers %d", jobs.workerCount());
    std::atomic<int> count{0};
    for (int i = 0; i < 10; ++i)
        jobs.run([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            count.fetch_add(1);
        });
    CHECK(count.load() == 0, "a job ran before runFor (%d)", count.load());
    const int first = jobs.runFor(0.012);
    CHECK(first >= 1 && first <= 4, "first runFor ran %d jobs", first);
    int frames = 1;
    while (jobs.pendingCount() > 0 && frames < 100) {
        jobs.runFor(0.012);
        ++frames;
    }
    CHECK(count.load() == 10, "%d of 10 jobs ran in %d frames", count.load(), frames);
    CHECK(frames >= 3, "all jobs ran in %d frames", frames);
}

// Workers have the worker priority and their own thread indices; the creating thread is 0, other threads -1.
QUENCH_TEST(jobs_worker_threads) {
    if (!threadsAvailable()) QUENCH_SKIP("no threads in this build");
    JobSystem jobs(JobSystemDesc{.workerThreads = 3});
    CHECK(jobs.threadIndex() == 0, "creating thread index %d", jobs.threadIndex());
    int outside = 0;
    std::thread([&] { outside = jobs.threadIndex(); }).join();
    CHECK(outside == -1, "plain thread index %d", outside);

    std::mutex mutex;
    std::set<int> indices;
    std::atomic<int> withPriority{0}, onWorkers{0};
    std::atomic<int> started{0};
    std::vector<Job> handles;
    // Each job waits until all three run at once, so every worker takes one.
    for (int i = 0; i < 3; ++i)
        handles.push_back(jobs.run([&] {
            started.fetch_add(1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (started.load() < 3 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            const int index = jobs.threadIndex();
            if (index >= 1) {
                onWorkers.fetch_add(1);
                if (hasWorkerThreadPriority()) withPriority.fetch_add(1);
            }
            std::lock_guard lock(mutex);
            indices.insert(index);
        }));
    // Polling, not wait(): the creating thread must not take one of the jobs.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (jobs.pendingCount() > 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(onWorkers.load() == 3, "%d of 3 jobs ran on workers", onWorkers.load());
    CHECK(withPriority.load() == 3, "%d of 3 worker jobs had the worker priority", withPriority.load());
    CHECK(indices.size() == 3, "%zu distinct thread indices", indices.size());
    for (int index : indices) CHECK(index >= 1 && index <= 3, "thread index %d", index);
}

// Many short jobs with dependencies from several producers: a stress case for the sanitizers.
QUENCH_TEST(jobs_stress) {
    forEachMode([](JobSystem& jobs) {
        std::atomic<long long> total{0};
        for (int round = 0; round < 20; ++round) {
            std::vector<Job> layer;
            for (int i = 0; i < 16; ++i) layer.push_back(jobs.run([&total] { total.fetch_add(1); }));
            std::vector<Job> next;
            for (int i = 0; i < 16; ++i)
                next.push_back(jobs.parallelFor(
                    64, 4, [&total](std::uint32_t begin, std::uint32_t end) { total.fetch_add(end - begin); },
                    JobPriority::Normal, layer));
            Job last = jobs.run([] {}, JobPriority::High, next);
            layer.clear();
            next.clear();
            jobs.wait(last);
        }
        jobs.waitAll();
        CHECK(total.load() == 20LL * (16 + 16 * 64), "total %lld", total.load());
    });
}
