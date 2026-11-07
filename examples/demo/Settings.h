#pragma once

// The demo's settings: the last section and the main options, kept as versioned JSON in the user directory through
// quench::fs (settings.json; --settings-file NAME). A file that cannot be read or parsed, or one of another version,
// gives the defaults and a log line. Scripted runs (--script) ignore the file unless --settings-file names one, so
// checks start from the defaults and leave the user's file alone; --no-settings ignores it in any run.

#include "GraphicsPanel.h"

#include <quench/core/Result.h>

#include <string>
#include <string_view>

namespace demo {

enum class TouchControls { Auto, On, Off }; // Auto: on where the device has a touch screen

struct Settings {
    static constexpr int kVersion = 1;

    std::string section;  // id of the last section; empty: the first
    bool showUi = true;   // the windows (H, Start)
    float uiScale = 1.0f; // on top of the display's scale
    TouchControls touch = TouchControls::Auto;
    std::string hdr;     // "off", "on", "auto"; empty: the app's default
    std::string present; // "vsync", "immediate", "mailbox"; empty: the app's default
    float tourSeconds = 8.0f;
    // Adapt to heat: the engine's thermal reaction (App::thermalGovernor) with its defaults ("adapt_to_heat":
    // optional, off where a file has none).
    bool adaptToHeat = false;
    // The Graphics panel's options, for every section ("graphics": optional, so files without it still load).
    GraphicsOverrides graphics;

    friend bool operator==(const Settings&, const Settings&) = default;
};

// Errors name the problem (a parse error with its offset, a missing or other version); the caller logs it and keeps the
// defaults.
quench::Result<Settings> parseSettings(const std::string& text);
std::string writeSettings(const Settings& settings);
const char* touchControlsName(TouchControls touch);

} // namespace demo
