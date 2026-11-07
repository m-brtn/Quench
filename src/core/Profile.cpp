// Built with QUENCH_TRACY only (CMakeLists.txt).
#include <quench/core/Profile.h>

#include <tracy/TracyC.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace quench::profile {
namespace {

static_assert(sizeof(Zone) == sizeof(TracyCZoneCtx), "profile::Zone holds a TracyCZoneCtx");

constexpr char kFile[] = __FILE__;

std::atomic<std::uint16_t> g_nextQuery{0};

std::uint64_t sourceLocation(const char* function, std::string_view name) {
    return ___tracy_alloc_srcloc_name(__LINE__, kFile, sizeof(kFile) - 1, function, std::strlen(function), name.data(),
                                      name.size(), 0);
}

} // namespace

bool connected() { return TracyIsConnected; }

Zone beginZone(std::string_view name) {
    Zone zone;
    if (!TracyIsConnected) return zone;
    const TracyCZoneCtx ctx = ___tracy_emit_zone_begin_alloc(sourceLocation("zone", name), 1);
    std::memcpy(&zone, &ctx, sizeof(zone));
    return zone;
}

void endZone(const Zone& zone) {
    if (!zone.active) return;
    TracyCZoneCtx ctx;
    std::memcpy(&ctx, &zone, sizeof(ctx));
    ___tracy_emit_zone_end(ctx);
}

void message(std::string_view text) {
    if (TracyIsConnected) TracyMessage(text.data(), text.size());
}

void setThreadName(const char* name) { tracy::SetThreadName(name); }

int gpuContext(std::string_view name, std::int64_t gpuNow, float periodNs) {
    const auto context = std::uint8_t(tracy::NextGpuContextId());
    ___tracy_emit_gpu_new_context_serial({.gpuTime = gpuNow,
                                          .period = periodNs,
                                          .context = context,
                                          .flags = 0,
                                          .type = std::uint8_t(tracy::GpuContextType::Metal)});
    ___tracy_emit_gpu_context_name_serial(
        {.context = context, .name = name.data(), .len = std::uint16_t(std::min<std::size_t>(name.size(), 0xffff))});
    return context;
}

void gpuZones(int context, std::span<const GpuZone> zones) {
    if (context < 0 || zones.empty() || !TracyIsConnected) return;
    const auto ctx = std::uint8_t(context);
    // Each begin and end has its own query id, which its GPU time refers to (the server matches them by id).
    const auto timestamp = [ctx](std::int64_t ticks) {
        const std::uint16_t query = g_nextQuery.fetch_add(1, std::memory_order_relaxed);
        return std::pair{query, ___tracy_gpu_time_data{.gpuTime = ticks, .queryId = query, .context = ctx}};
    };
    struct Open {
        std::uint32_t depth;
        std::int64_t end;
    };
    std::vector<Open> open;
    const auto close = [&] {
        const auto [query, time] = timestamp(open.back().end);
        ___tracy_emit_gpu_zone_end_serial({.queryId = query, .context = ctx});
        ___tracy_emit_gpu_time_serial(time);
        open.pop_back();
    };
    // The server nests a context's zones in the order they begin and end.
    std::vector<GpuZone> sorted(zones.begin(), zones.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const GpuZone& a, const GpuZone& b) {
        return a.start != b.start ? a.start < b.start : a.depth < b.depth;
    });
    for (const GpuZone& zone : sorted) {
        while (!open.empty() && (open.back().depth >= zone.depth || open.back().end <= zone.start)) close();
        const auto [query, time] = timestamp(zone.start);
        ___tracy_emit_gpu_zone_begin_alloc_serial(
            {.srcloc = sourceLocation("gpu", zone.name), .queryId = query, .context = ctx});
        ___tracy_emit_gpu_time_serial(time);
        open.push_back({zone.depth, zone.end});
    }
    while (!open.empty()) close();
}

} // namespace quench::profile

#if QUENCH_TRACY_MEMORY
// Every allocation through operator new and delete, recorded while a server is connected (QUENCH_TRACY_MEMORY).
void* operator new(std::size_t size) {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (!p) throw std::bad_alloc();
    TracyAlloc(p, size);
    return p;
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p) TracyAlloc(p, size);
    return p;
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return operator new(size, tag); }
void operator delete(void* p) noexcept {
    if (!p) return;
    TracyFree(p);
    std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }
#endif
