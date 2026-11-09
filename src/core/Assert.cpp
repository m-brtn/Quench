#include <quench/core/Assert.h>

#include <quench/core/Log.h>

#include <cstdlib>

namespace quench {
namespace {

void defaultHandler(const AssertInfo& info) {
    log::error("assert", "{}:{}: {}: {}", info.file, info.line, info.condition, info.message);
#if defined(__clang__)
    __builtin_debugtrap();
#elif defined(_MSC_VER)
    __debugbreak();
#endif
    std::abort();
}

AssertHandler g_handler = defaultHandler;

} // namespace

AssertHandler setAssertHandler(AssertHandler handler) {
    const AssertHandler previous = g_handler;
    g_handler = handler ? handler : defaultHandler;
    return previous;
}

namespace detail {

void assertFailed(const char* condition, const char* message, const char* file, int line) {
    g_handler(AssertInfo{condition, message, file, line});
}

} // namespace detail

} // namespace quench
