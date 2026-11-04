#include "Test.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace quench::test {
namespace {

struct Case {
    const char* name;
    TestFn fn;
};

std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}

int g_failures = 0;
bool g_skipped = false;

} // namespace

Registrar::Registrar(const char* name, TestFn fn) { cases().push_back({name, fn}); }

void fail(const char* file, int line, const char* condition, const char* format, ...) {
    std::printf("  FAIL %s:%d: %s: ", file, line, condition);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
    ++g_failures;
}

void skip(const char* reason) {
    std::printf("  skipped: %s\n", reason);
    g_skipped = true;
}

} // namespace quench::test

int main(int argc, char** argv) {
    using namespace quench::test;
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const Case& c : cases()) std::printf("%s\n", c.name);
        return 0;
    }
    int ran = 0, skipped = 0;
    for (const Case& c : cases()) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
        const int before = g_failures;
        g_skipped = false;
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        const char* status = g_failures != before ? "[ FAIL ]" : g_skipped ? "[ SKIP ]" : "[  OK  ]";
        std::printf("%s %s\n", status, c.name);
        std::fflush(stdout);
        ++ran;
        skipped += g_skipped ? 1 : 0;
    }
    if (ran == 0) {
        std::printf("unknown test: %s\n", argc > 1 ? argv[1] : "(none registered)");
        return 2;
    }
    if (g_failures != 0) return 1;
    return skipped == ran ? kSkipExitCode : 0;
}
