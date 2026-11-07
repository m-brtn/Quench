#pragma once

// The operating system's thermal state (ThermalGovernor.h, app::ThermalState), read by the app every 0.5 s. Where the
// platform has a query (ThermalStateApple.m: NSProcessInfo), its value; elsewhere Unknown.

#include <quench/app/ThermalGovernor.h>

#include <optional>
#include <string_view>

namespace quench::app::detail {

ThermalState queryThermalState();
// thermalStateName's names back to states: --thermal-state, the scenario command thermal_state.
std::optional<ThermalState> parseThermalState(std::string_view name);

} // namespace quench::app::detail
