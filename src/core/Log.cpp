#include <quench/core/Log.h>

#include <quench/core/Profile.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace quench::log {
namespace {

struct State {
    std::mutex mutex;
    std::vector<std::pair<int, Sink>> sinks;
    int nextId = 1;
    bool stderrEnabled = true;
    std::atomic<int> level{int(kDebugEnabled ? Level::Debug : Level::Info)};
};

// Constructed on first use, so logging from other static initializers is safe.
State& state() {
    static State s;
    return s;
}

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

int addSink(Sink sink) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    const int id = s.nextId++;
    s.sinks.emplace_back(id, std::move(sink));
    return id;
}

void removeSink(int id) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.sinks, [id](const auto& entry) { return entry.first == id; });
}

void setStderr(bool enabled) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.stderrEnabled = enabled;
}

void setLevel(Level level) { state().level.store(int(level)); }

bool enabled(Level level) { return int(level) <= state().level.load(std::memory_order_relaxed); }

const char* levelName(Level level) {
    switch (level) {
    case Level::Error: return "error";
    case Level::Warning: return "warning";
    case Level::Info: return "info";
    case Level::Debug: return "debug";
    }
    return "?";
}

void write(Level level, std::string_view category, std::string_view message) {
    if (profile::connected()) profile::message(std::format("[{}] {}: {}", levelName(level), category, message));
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.stderrEnabled) {
        const std::string line = std::format("[{}] {}: {}\n", levelName(level), category, message);
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
    for (const auto& [id, sink] : s.sinks) sink(level, category, message);
}

Throttle::Throttle(double intervalSeconds) : m_intervalNs(std::int64_t(intervalSeconds * 1e9)) {}

bool Throttle::allow() {
    const std::int64_t now = nowNs();
    std::int64_t next = m_nextNs.load(std::memory_order_relaxed);
    if (now >= next && m_nextNs.compare_exchange_strong(next, now + m_intervalNs)) return true;
    m_suppressed.fetch_add(1, std::memory_order_relaxed);
    return false;
}

} // namespace quench::log
