#include <quench/app/SdlLog.h>

#include <quench/core/Log.h>

#include <SDL3/SDL.h>

namespace quench::app {
namespace {

SDL_LogPriority priority(log::Level level) {
    switch (level) {
    case log::Level::Error: return SDL_LOG_PRIORITY_ERROR;
    case log::Level::Warning: return SDL_LOG_PRIORITY_WARN;
    case log::Level::Info: return SDL_LOG_PRIORITY_INFO;
    case log::Level::Debug: return SDL_LOG_PRIORITY_DEBUG;
    }
    return SDL_LOG_PRIORITY_INFO;
}

} // namespace

int addSdlLogSink() {
    SDL_SetLogPriority(SDL_LOG_CATEGORY_CUSTOM, SDL_LOG_PRIORITY_DEBUG);
    log::setStderr(false);
    return log::addSink([](log::Level level, std::string_view category, std::string_view message) {
        SDL_LogMessage(SDL_LOG_CATEGORY_CUSTOM, priority(level), "%.*s: %.*s", int(category.size()), category.data(),
                       int(message.size()), message.data());
    });
}

} // namespace quench::app
