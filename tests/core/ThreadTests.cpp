#include "Test.h"

#include <quench/core/Thread.h>

#include <thread>

using namespace quench;

QUENCH_TEST(thread_priority) {
    if (!threadsAvailable()) QUENCH_SKIP("no threads in this build");
    bool raised = false;
    std::thread([&] {
        raiseWorkerThreadPriority();
        raised = hasWorkerThreadPriority();
    }).join();
    CHECK(raised, "raiseWorkerThreadPriority() had no effect");
}
