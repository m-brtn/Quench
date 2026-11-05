#include "Test.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if QUENCH_TEST_SDL_MAIN
// The device test app: SDL's UIKit delegate calls main as SDL_main once the application has started, so cases can
// open windows. SDL does not end the process when SDL_main returns.
#include <SDL3/SDL_main.h>
#endif

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

const char* tmpDir() {
#ifdef QUENCH_TEST_TMP_DIR
    return QUENCH_TEST_TMP_DIR;
#else
    static const std::string dir = [] {
        std::error_code ec;
        const std::filesystem::path path = std::filesystem::temp_directory_path(ec) / "quench_tests";
        std::filesystem::create_directories(path, ec);
        return path.string();
    }();
    return dir.c_str();
#endif
}

} // namespace quench::test

int main(int argc, char** argv) {
    using namespace quench::test;
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        for (const Case& c : cases()) std::printf("%s\n", c.name);
        return 0;
    }
    int ran = 0, skipped = 0, failed = 0;
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
        skipped += g_skipped && g_failures == before ? 1 : 0;
        failed += g_failures != before ? 1 : 0;
    }
    int result = 0;
    if (ran == 0) {
        std::printf("unknown test: %s\n", argc > 1 ? argv[1] : "(none registered)");
        result = 2;
    } else {
        if (ran > 1)
            std::printf("%d cases: %d passed, %d failed, %d skipped\n", ran, ran - failed - skipped, failed, skipped);
        result = failed != 0 ? 1 : skipped == ran ? kSkipExitCode : 0;
    }
#if QUENCH_TEST_SDL_MAIN
    std::fflush(stdout);
    std::exit(result);
#endif
    return result;
}
