#pragma once

// QUENCH_ASSERT(condition, message): checks a programming error (a broken invariant, a wrong use of an API), never
// bad run-time data. Active unless NDEBUG; the QUENCH_ASSERTS CMake option forces it on or off
// (QUENCH_ENABLE_ASSERTS=1 or 0). On failure the handler runs; the default one logs the file, line, condition and
// message, stops in the debugger and aborts. When asserts are off, the condition is not evaluated.

#ifndef QUENCH_ENABLE_ASSERTS
#ifdef NDEBUG
#define QUENCH_ENABLE_ASSERTS 0
#else
#define QUENCH_ENABLE_ASSERTS 1
#endif
#endif

namespace quench {

struct AssertInfo {
    const char* condition;
    const char* message;
    const char* file;
    int line;
};

using AssertHandler = void (*)(const AssertInfo& info);

// Replaces the handler (nullptr restores the default) and returns the previous one. If a handler returns, execution
// continues after the assert. Not thread-safe: set it at startup.
AssertHandler setAssertHandler(AssertHandler handler);

namespace detail {
void assertFailed(const char* condition, const char* message, const char* file, int line);
} // namespace detail

} // namespace quench

#if QUENCH_ENABLE_ASSERTS
#define QUENCH_ASSERT(condition, message)                                                                              \
    do {                                                                                                               \
        if (!(condition)) ::quench::detail::assertFailed(#condition, message, __FILE__, __LINE__);                     \
    } while (0)
#else
#define QUENCH_ASSERT(condition, message)                                                                              \
    do {                                                                                                               \
    } while (0)
#endif
