#include "Test.h"

#include <quench/core/Log.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace quench;

namespace {

struct Captured {
    log::Level level;
    std::string category, message;
};

struct Capture {
    Capture() {
        log::setStderr(false);
        id = log::addSink([this](log::Level level, std::string_view category, std::string_view message) {
            std::lock_guard lock(mutex);
            lines.push_back({level, std::string(category), std::string(message)});
        });
    }
    ~Capture() {
        log::removeSink(id);
        log::setStderr(true);
    }
    std::mutex mutex;
    std::vector<Captured> lines;
    int id = 0;
};

} // namespace

QUENCH_TEST(log_sink_receives_formatted) {
    Capture c;
    log::warning("assets", "missing {} ({} bytes)", "rock.glb", 42);
    CHECK(c.lines.size() == 1, "%zu lines", c.lines.size());
    if (c.lines.size() != 1) return;
    CHECK(c.lines[0].level == log::Level::Warning, "level");
    CHECK(c.lines[0].category == "assets", "category '%s'", c.lines[0].category.c_str());
    CHECK(c.lines[0].message == "missing rock.glb (42 bytes)", "message '%s'", c.lines[0].message.c_str());
}

QUENCH_TEST(log_level_filter) {
    Capture c;
    log::setLevel(log::Level::Warning);
    CHECK(!log::enabled(log::Level::Info) && log::enabled(log::Level::Error), "enabled()");
    log::info("app", "dropped");
    log::error("app", "kept");
    log::setLevel(log::Level::Debug);
    log::debug("app", "debug line");
    CHECK(c.lines.size() == (log::kDebugEnabled ? 2u : 1u), "%zu lines", c.lines.size());
    CHECK(!c.lines.empty() && c.lines[0].message == "kept", "first line");
}

QUENCH_TEST(log_remove_sink) {
    std::vector<std::string> got;
    log::setStderr(false);
    const int id = log::addSink([&](log::Level, std::string_view, std::string_view m) { got.emplace_back(m); });
    log::info("app", "one");
    log::removeSink(id);
    log::info("app", "two");
    log::setStderr(true);
    CHECK(got.size() == 1 && got[0] == "one", "%zu lines after removal", got.size());
}

QUENCH_TEST(log_throttle) {
    log::Throttle throttle(3600.0);
    CHECK(throttle.allow(), "first message refused");
    int refused = 0;
    for (int i = 0; i < 3; ++i) refused += throttle.allow() ? 0 : 1;
    CHECK(refused == 3, "%d refused", refused);
    CHECK(throttle.takeSuppressed() == 3, "suppressed count");
    CHECK(throttle.takeSuppressed() == 0, "count not reset");
}

QUENCH_TEST(log_threads) {
    Capture c;
    constexpr int kThreads = 4, kPerThread = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) log::info("jobs", "thread {} line {}", t, i);
        });
    for (std::thread& t : threads) t.join();
    CHECK(c.lines.size() == std::size_t(kThreads * kPerThread), "%zu lines", c.lines.size());
}
