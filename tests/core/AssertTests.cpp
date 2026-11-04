#include "Test.h"

#include <quench/core/Assert.h>

#include <cstring>

using namespace quench;

namespace {

int g_calls = 0;
AssertInfo g_last{};

void recordingHandler(const AssertInfo& info) {
    ++g_calls;
    g_last = info;
}

} // namespace

// In builds with asserts on, a failed QUENCH_ASSERT reaches the handler with the condition, message, file and line;
// with asserts off, the condition is not even evaluated.
QUENCH_TEST(assert_handler) {
    setAssertHandler(recordingHandler);
    int evaluated = 0;
    QUENCH_ASSERT(++evaluated == 1, "first is fine");
    QUENCH_ASSERT(++evaluated == 0, "second fails");
    setAssertHandler(nullptr);
#if QUENCH_ENABLE_ASSERTS
    CHECK(evaluated == 2, "condition evaluated %d times", evaluated);
    CHECK(g_calls == 1, "handler called %d times", g_calls);
    CHECK(std::strcmp(g_last.message, "second fails") == 0, "message '%s'", g_last.message);
    CHECK(std::strstr(g_last.condition, "evaluated") != nullptr, "condition '%s'", g_last.condition);
    CHECK(std::strstr(g_last.file, "AssertTests.cpp") != nullptr && g_last.line > 0, "location %s:%d", g_last.file,
          g_last.line);
#else
    CHECK(evaluated == 0, "condition evaluated with asserts off");
    CHECK(g_calls == 0, "handler called with asserts off");
#endif
}

QUENCH_TEST(assert_handler_restore) {
    const AssertHandler first = setAssertHandler(recordingHandler);
    CHECK(first != nullptr, "default handler is null");
    const AssertHandler second = setAssertHandler(nullptr);
    CHECK(second == recordingHandler, "previous handler not returned");
    CHECK(setAssertHandler(nullptr) == first, "nullptr did not restore the default handler");
}
