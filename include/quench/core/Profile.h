#pragma once

// Profiling with Tracy (docs/performance.md, "Profiling with Tracy"). With QUENCH_TRACY off, the default, every macro
// compiles to nothing (its arguments are not evaluated), the functions do nothing and no Tracy header is included.
// With it on (the `profile` preset), the engine links Tracy's client in on-demand mode: zones, plots and messages are
// recorded only while a Tracy server is connected (tracy-capture or the Tracy GUI); otherwise each costs a check of
// one atomic flag.
//
//   QUENCH_PROFILE_ZONE("game.ai");               a zone to the end of the scope, named by a string literal
//   QUENCH_PROFILE_ZONE_DYNAMIC(name);            the same with a name known at run time (std::string_view, copied)
//   QUENCH_PROFILE_ZONE_TEXT(text);               text on the scope's zone (std::string_view, copied)
//   QUENCH_PROFILE_FRAME();                       the end of a frame (the app marks its own frames)
//   QUENCH_PROFILE_PLOT("game.enemies", value);   a value over time, named by a string literal
//   QUENCH_PROFILE_MESSAGE(text);                 a message on the timeline (std::string_view, copied)
//   QUENCH_PROFILE_THREAD_NAME(name);             names the calling thread (const char*, copied)
//
// One zone macro per scope: they declare the same variable. Engine zones are named "<module>.<what>" ("app.update",
// "assets.decode"); a job's zone has the name given to JobSystem::run ("job" without one).

#include <cstdint>
#include <span>
#include <string_view>

#if QUENCH_TRACY
#include <tracy/Tracy.hpp>

#include <cstring>
#endif

namespace quench::profile {

// A zone opened and closed by calls, for ranges that are not a C++ scope (a command buffer's timing ranges, Jolt's
// profile scopes). Zones nest on the thread that opens them; endZone() takes what beginZone() returned.
struct Zone {
    std::uint32_t id = 0;
    std::int32_t active = 0;
    std::uint64_t connection = 0;
};

// GPU zones from timestamps a GPU backend reads itself (the native Metal backend's timing ranges): an interval in
// which a range kept the GPU busy. A range has one or more (its passes overlap other ranges' on tile-based GPUs).
struct GpuZone {
    std::string_view name;
    std::int64_t start = 0; // GPU ticks
    std::int64_t end = 0;
    std::uint32_t depth = 0; // 0: outermost; a zone is inside the last earlier one of a smaller depth
};

#if QUENCH_TRACY
inline constexpr bool kEnabled = true;

// True while a Tracy server is connected.
bool connected();
Zone beginZone(std::string_view name);
void endZone(const Zone& zone);
void message(std::string_view text);
void setThreadName(const char* name);
// A GPU timeline, once per device: gpuNow is a GPU timestamp taken just before the call, periodNs the nanoseconds per
// tick. Returns its id, -1 without Tracy.
int gpuContext(std::string_view name, std::int64_t gpuNow, float periodNs);
// One command buffer's zones, in any order, once the GPU has finished it. Nothing while no server is connected.
void gpuZones(int context, std::span<const GpuZone> zones);
#else
inline constexpr bool kEnabled = false;

inline bool connected() { return false; }
inline Zone beginZone(std::string_view) { return {}; }
inline void endZone(const Zone&) {}
inline void message(std::string_view) {}
inline void setThreadName(const char*) {}
inline int gpuContext(std::string_view, std::int64_t, float) { return -1; }
inline void gpuZones(int, std::span<const GpuZone>) {}
#endif

} // namespace quench::profile

#if QUENCH_TRACY
#define QUENCH_PROFILE_ZONE(name) ZoneScopedN(name)
#define QUENCH_PROFILE_ZONE_DYNAMIC(name)                                                                              \
    const std::string_view quench_profile_zone_name_ = (name);                                                         \
    tracy::ScopedZone ___tracy_scoped_zone(TracyLine, TracyFile, std::strlen(TracyFile), TracyFunction,                \
                                           std::strlen(TracyFunction), quench_profile_zone_name_.data(),               \
                                           quench_profile_zone_name_.size(), 0u)
#define QUENCH_PROFILE_ZONE_TEXT(text)                                                                                 \
    do {                                                                                                               \
        const std::string_view quench_profile_text_ = (text);                                                          \
        ZoneText(quench_profile_text_.data(), quench_profile_text_.size());                                            \
    } while (false)
#define QUENCH_PROFILE_FRAME() FrameMark
#define QUENCH_PROFILE_PLOT(name, value) TracyPlot(name, static_cast<double>(value))
#define QUENCH_PROFILE_MESSAGE(text) ::quench::profile::message(text)
#define QUENCH_PROFILE_THREAD_NAME(name) ::quench::profile::setThreadName(name)
#else
#define QUENCH_PROFILE_ZONE(name) static_cast<void>(0)
#define QUENCH_PROFILE_ZONE_DYNAMIC(name) static_cast<void>(0)
#define QUENCH_PROFILE_ZONE_TEXT(text) static_cast<void>(0)
#define QUENCH_PROFILE_FRAME() static_cast<void>(0)
#define QUENCH_PROFILE_PLOT(name, value) static_cast<void>(0)
#define QUENCH_PROFILE_MESSAGE(text) static_cast<void>(0)
#define QUENCH_PROFILE_THREAD_NAME(name) static_cast<void>(0)
#endif
