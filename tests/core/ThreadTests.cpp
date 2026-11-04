#include "Test.h"

#include <quench/core/Thread.h>

#include <thread>

using namespace quench;

QUENCH_TEST(thread_priority) {
    bool raised = false;
    std::thread([&] {
        raiseWorkerThreadPriority();
        raised = hasWorkerThreadPriority();
    }).join();
    CHECK(raised, "raiseWorkerThreadPriority() had no effect");
}
