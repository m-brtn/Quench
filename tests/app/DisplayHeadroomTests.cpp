#include "Test.h"

#include "app/DisplayHeadroom.h"
#include "core/Platform.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace quench;
using app::detail::DisplayHeadroom;
using app::detail::HeadroomLog;
using app::detail::queryDisplayHeadroom;
using app::detail::resolveDisplayHeadroom;

// On iOS a window needs the app lifecycle: the device test app has it (SDL_main), executables spawned by simctl do not.
#if QUENCH_PLATFORM_MACOS || QUENCH_PLATFORM_WINDOWS || QUENCH_PLATFORM_LINUX || defined(QUENCH_TEST_SDL_MAIN)
#define QUENCH_TEST_WINDOWS 1
#else
#define QUENCH_TEST_WINDOWS 0
#endif

QUENCH_TEST(display_headroom_resolve) {
    DisplayHeadroom h = resolveDisplayHeadroom(std::nullopt, 8.0f);
    CHECK(h.current == 8.0f && h.potential == 8.0f, "SDL's value for both: %f %f", double(h.current),
          double(h.potential));
    h = resolveDisplayHeadroom(DisplayHeadroom{.current = 1.2f, .potential = 8.0f}, 1.2f);
    CHECK(h.current == 1.2f && h.potential == 8.0f, "the platform's values: %f %f", double(h.current),
          double(h.potential));
    h = resolveDisplayHeadroom(DisplayHeadroom{.current = 0.5f, .potential = 0.0f}, 1.0f);
    CHECK(h.current == 1.0f && h.potential == 1.0f, "at least 1: %f %f", double(h.current), double(h.potential));
    h = resolveDisplayHeadroom(DisplayHeadroom{.current = 4.0f, .potential = 2.0f}, 1.0f);
    CHECK(h.current == 4.0f && h.potential == 4.0f, "potential at least current: %f", double(h.potential));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    h = resolveDisplayHeadroom(DisplayHeadroom{.current = nan, .potential = nan}, 1.0f);
    CHECK(h.current == 1.0f && h.potential == 1.0f, "not finite: %f %f", double(h.current), double(h.potential));
}

QUENCH_TEST(display_headroom_log_rate) {
    HeadroomLog log;
    log.logged(1.2f, 0.0f);
    CHECK(!log.due(1.2f, 5.0), "unchanged");
    CHECK(!log.due(1.205f, 5.0), "within 1 percent");
    log.logged(1.2f, 9.5);
    CHECK(!log.due(2.0f, 10.0), "a change within the interval of the last line");
    CHECK(log.due(2.0f, 10.5), "a change after the interval");
    CHECK(!log.due(4.0f, 11.0), "a ramp: not more than one line per interval");
    CHECK(!log.due(8.0f, 11.4), "still within the interval");
    CHECK(log.due(8.0f, 11.5), "the value the ramp settled on, one interval after the last line");
    CHECK(!log.due(8.0f, 20.0), "settled");
    HeadroomLog fresh;
    CHECK(fresh.due(1.0f, 0.0), "the first value is due");
}

// The platform query on a real window: Apple platforms have one (UIScreen, NSScreen); elsewhere SDL's property is used.
QUENCH_TEST(display_headroom_query) {
    CHECK(!queryDisplayHeadroom(nullptr), "no window");
#if !QUENCH_TEST_WINDOWS
    QUENCH_SKIP("windows need the app lifecycle on this platform");
#else
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) QUENCH_SKIP("no video subsystem");
    SDL_Window* window = SDL_CreateWindow("quench_app_tests", 160, 90, 0);
    REQUIRE(window, "SDL_CreateWindow: %s", SDL_GetError());
    SDL_PumpEvents();
    const std::optional<DisplayHeadroom> h = queryDisplayHeadroom(window);
    constexpr int kCalls = 1000; // the app queries once per frame: print what that costs
    const Uint64 start = SDL_GetTicksNS();
    for (int i = 0; i < kCalls; ++i) (void)queryDisplayHeadroom(window);
    std::printf("  %.2f us per query\n", double(SDL_GetTicksNS() - start) / kCalls / 1000.0);
    const float sdl = SDL_GetFloatProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, -1.0f);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
#if QUENCH_PLATFORM_APPLE
    REQUIRE(h.has_value(), "no headroom from the window's screen");
    std::printf("  current %.3f, potential %.3f; SDL %.3f\n", double(h->current), double(h->potential), double(sdl));
    CHECK(std::isfinite(h->current) && h->current >= 1.0f, "current %f", double(h->current));
    CHECK(std::isfinite(h->potential) && h->potential >= h->current, "potential %f", double(h->potential));
#else
    (void)sdl;
    CHECK(!h, "no platform query here");
#endif
#endif
}
