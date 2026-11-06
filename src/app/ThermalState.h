#pragma once

// The operating system's thermal state (App.h, app::ThermalState), read by the app every 0.5 s. Where the platform has
// a query (ThermalStateApple.m: NSProcessInfo), its value; elsewhere Unknown.

#include <quench/app/App.h>

namespace quench::app::detail {

ThermalState queryThermalState();

} // namespace quench::app::detail
