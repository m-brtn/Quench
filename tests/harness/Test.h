#pragma once

// Minimal test harness. A test executable holds QUENCH_TEST cases; `<exe> --list` prints their names, `<exe> <name>`
// runs one, `<exe>` runs all. CTest registers every case as its own test (tests/DiscoverTests.cmake).

namespace quench::test {

using TestFn = void (*)();

struct Registrar {
    Registrar(const char* name, TestFn fn);
};

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
void fail(const char* file, int line, const char* condition, const char* format, ...);

// Marks the running case as skipped (CTest reports it as "Skipped", exit code kSkipExitCode).
void skip(const char* reason);

inline constexpr int kSkipExitCode = 77;

} // namespace quench::test

#define QUENCH_TEST(name)                                                                                              \
    static void name();                                                                                                \
    static const ::quench::test::Registrar name##_registrar(#name, name);                                              \
    static void name()

// QUENCH_SKIP("reason"): skips the rest of the case when it cannot run in this build.
#define QUENCH_SKIP(reason)                                                                                            \
    do {                                                                                                               \
        ::quench::test::skip(reason);                                                                                  \
        return;                                                                                                        \
    } while (0)

// CHECK(condition, "printf format", args...): records a failure and continues.
#define CHECK(condition, ...)                                                                                          \
    do {                                                                                                               \
        if (!(condition)) ::quench::test::fail(__FILE__, __LINE__, #condition, __VA_ARGS__);                           \
    } while (0)
