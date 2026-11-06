#include "Test.h"

#include "app/ThermalState.h"
#include "core/Platform.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

using namespace quench;
using app::ThermalState;
using app::thermalStateName;
using app::detail::queryThermalState;

QUENCH_TEST(thermal_state_names) {
    CHECK(std::strcmp(thermalStateName(ThermalState::Unknown), "unknown") == 0, "unknown");
    CHECK(std::strcmp(thermalStateName(ThermalState::Nominal), "nominal") == 0, "nominal");
    CHECK(std::strcmp(thermalStateName(ThermalState::Fair), "fair") == 0, "fair");
    CHECK(std::strcmp(thermalStateName(ThermalState::Serious), "serious") == 0, "serious");
    CHECK(std::strcmp(thermalStateName(ThermalState::Critical), "critical") == 0, "critical");
    CHECK(ThermalState::Unknown < ThermalState::Nominal && ThermalState::Serious < ThermalState::Critical,
          "ordered by heat, unknown lowest (a perf region's highest state)");
}

// Apple platforms have a query (NSProcessInfo); elsewhere the state is unknown.
QUENCH_TEST(thermal_state_query) {
    const ThermalState state = queryThermalState();
    constexpr int kCalls = 1000; // the app queries every 0.5 s: print what a query costs
    const Uint64 start = SDL_GetTicksNS();
    for (int i = 0; i < kCalls; ++i) (void)queryThermalState();
    std::printf("  %s, %.3f us per query\n", thermalStateName(state),
                double(SDL_GetTicksNS() - start) / kCalls / 1000.0);
#if QUENCH_PLATFORM_APPLE
    CHECK(state != ThermalState::Unknown, "no thermal state from the system");
#else
    CHECK(state == ThermalState::Unknown, "no platform query here: %s", thermalStateName(state));
#endif
}
