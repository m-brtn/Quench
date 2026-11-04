#pragma once

// Engine log: one line per message with a level and a category per module ("gpu", "assets", "physics", "audio",
// "app", ...). Messages go to every sink; stderr is a sink by default, and the app adds SDL_Log (the Xcode console
// and the device log on iOS). Thread-safe. Debug messages are compiled out when NDEBUG is defined.

#include <atomic>
#include <cstdint>
#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace quench::log {

enum class Level { Error, Warning, Info, Debug };

#ifdef NDEBUG
inline constexpr bool kDebugEnabled = false;
#else
inline constexpr bool kDebugEnabled = true;
#endif

using Sink = std::function<void(Level level, std::string_view category, std::string_view message)>;

// Adds a sink and returns its id for removeSink(). Sinks are called under the log's lock: a sink must not log.
int addSink(Sink sink);
void removeSink(int id);
void setStderr(bool enabled);

// Messages above this level are dropped before they are formatted. Default: Debug in debug builds, Info otherwise.
void setLevel(Level level);
bool enabled(Level level);

const char* levelName(Level level);

void write(Level level, std::string_view category, std::string_view message);

template <class... Args>
void error(std::string_view category, std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::Error)) write(Level::Error, category, std::format(format, std::forward<Args>(args)...));
}

template <class... Args>
void warning(std::string_view category, std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::Warning)) write(Level::Warning, category, std::format(format, std::forward<Args>(args)...));
}

template <class... Args>
void info(std::string_view category, std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, category, std::format(format, std::forward<Args>(args)...));
}

template <class... Args>
void debug(std::string_view category, std::format_string<Args...> format, Args&&... args) {
    if constexpr (kDebugEnabled) {
        if (enabled(Level::Debug)) write(Level::Debug, category, std::format(format, std::forward<Args>(args)...));
    }
}

// Lets a message that could repeat every frame through at most once per interval; thread-safe.
//   static log::Throttle throttle(1.0);
//   if (throttle.allow()) log::warning("gpu", "budget exceeded ({} suppressed)", throttle.takeSuppressed());
class Throttle {
public:
    explicit Throttle(double intervalSeconds);

    bool allow();
    // Messages refused since the last call; resets the count.
    int takeSuppressed() { return m_suppressed.exchange(0); }

private:
    std::int64_t m_intervalNs;
    std::atomic<std::int64_t> m_nextNs{0};
    std::atomic<int> m_suppressed{0};
};

} // namespace quench::log
