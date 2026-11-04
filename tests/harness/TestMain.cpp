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

} // namespace quench::test

int main(int argc, char** argv) {
    using namespace quench::test;
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const Case& c : cases()) std::printf("%s\n", c.name);
        return 0;
    }
    int ran = 0;
    for (const Case& c : cases()) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        std::printf("%s %s\n", g_failures == before ? "[  OK  ]" : "[ FAIL ]", c.name);
        std::fflush(stdout);
        ++ran;
    }
    if (ran == 0) {
        std::printf("unknown test: %s\n", argc > 1 ? argv[1] : "(none registered)");
        return 2;
    }
    return g_failures == 0 ? 0 : 1;
}
