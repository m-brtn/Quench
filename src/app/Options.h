#pragma once

// The command line flags every game understands (docs/app.md). Arguments the engine does not know are passed to
// createGame() in order.

#include <quench/core/Result.h>
#include <quench/gpu/Types.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::app::detail {

struct Options {
    double quitAfter = -1.0; // app seconds; < 0: run until closed
    std::string capture;     // PNG path
    double captureAt = 3.0;  // app seconds
    std::string script;      // scenario path
    int windowWidth = 0;     // 0: AppDesc
    int windowHeight = 0;
    int frameWidth = 0; // > 0: every frame is rendered offscreen at this size and scaled to the window
    int frameHeight = 0;
    std::string gpuDriver;
    bool gpuDebug = false;
    std::optional<gpu::PresentMode> presentMode;
    bool printVersion = false;
    bool printHelp = false;
    std::vector<std::string> gameArgs;

    // Scenario and capture runs step the app time by exactly one fixed step per frame.
    bool deterministic() const { return !script.empty() || !capture.empty(); }
};

// `args` without the program name. Accepts "--flag value" and "--flag=value".
Result<Options> parseOptions(std::span<const std::string_view> args);
const char* optionsHelp();

} // namespace quench::app::detail
