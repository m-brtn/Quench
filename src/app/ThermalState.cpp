#include "ThermalState.h"

#include "core/Platform.h"

#if QUENCH_PLATFORM_APPLE
extern "C" int quench_app_thermal_state(void);
#endif

namespace quench::app {

const char* thermalStateName(ThermalState state) {
    switch (state) {
    case ThermalState::Unknown: return "unknown";
    case ThermalState::Nominal: return "nominal";
    case ThermalState::Fair: return "fair";
    case ThermalState::Serious: return "serious";
    case ThermalState::Critical: return "critical";
    }
    return "unknown";
}

namespace detail {

ThermalState queryThermalState() {
#if QUENCH_PLATFORM_APPLE
    const int state = quench_app_thermal_state();
    if (state >= 0 && state <= int(ThermalState::Critical)) return ThermalState(state);
#endif
    return ThermalState::Unknown;
}

} // namespace detail

} // namespace quench::app
