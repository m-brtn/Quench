#include "Test.h"

#include <quench/app/SdlLog.h>
#include <quench/core/Log.h>

#include <SDL3/SDL.h>

#include <string>
#include <vector>

using namespace quench;

namespace {

struct Captured {
    int category;
    SDL_LogPriority priority;
    std::string message;
};

void SDLCALL capture(void* userdata, int category, SDL_LogPriority priority, const char* message) {
    static_cast<std::vector<Captured>*>(userdata)->push_back({category, priority, message});
}

} // namespace

QUENCH_TEST(sdl_log_sink) {
    SDL_LogOutputFunction previous = nullptr;
    void* previousData = nullptr;
    SDL_GetLogOutputFunction(&previous, &previousData);
    std::vector<Captured> captured;
    SDL_SetLogOutputFunction(capture, &captured);

    const int sink = app::addSdlLogSink();
    log::warning("test", "value {}", 42);
    log::debug("test", "detail");
    log::removeSink(sink);
    log::setStderr(true);
    SDL_SetLogOutputFunction(previous, previousData);

    REQUIRE(!captured.empty(), "nothing reached SDL_Log");
    CHECK(captured[0].category == SDL_LOG_CATEGORY_CUSTOM, "category %d", captured[0].category);
    CHECK(captured[0].priority == SDL_LOG_PRIORITY_WARN, "priority %d", int(captured[0].priority));
    CHECK(captured[0].message == "test: value 42", "message '%s'", captured[0].message.c_str());
    if (log::kDebugEnabled) {
        CHECK(captured.size() == 2 && captured[1].priority == SDL_LOG_PRIORITY_DEBUG, "debug message missing");
    } else {
        CHECK(captured.size() == 1, "%zu messages", captured.size());
    }
}
