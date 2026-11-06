#pragma once

// The command line flags every game understands (docs/app.md). Arguments the engine does not know are passed to
// createGame() in order.

#include <quench/app/App.h> // HdrMode
#include <quench/core/Result.h>
#include <quench/gpu/Device.h>
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
    std::optional<bool> offscreen;          // --offscreen / --onscreen; empty: offscreen for automated runs on desktops
    std::optional<gpu::Backend> gpuBackend; // empty: AppDesc
    std::string gpuDriver;
    bool gpuDebug = false;
    bool gpuPassTiming = false; // record per-pass GPU times every frame (FrameStats::gpuPasses)
    std::optional<gpu::PresentMode> presentMode;
    std::optional<HdrMode> hdr;
    float hdrHeadroom = 0.0f;  // > 0: the headroom of HDR output instead of the display's
    std::string orientation;   // SDL_HINT_ORIENTATIONS value (phones and tablets); empty: the app's orientations
    int workerThreads = -1;    // job system workers; -1: JobSystemDesc's default
    double assetLatency = 0.0; // AssetManagerSettings::simulatedLatency
    bool hotReload = false;
    bool noAudio = false;                   // no audio device: the mix runs offline and is discarded
    std::optional<std::string> audioDevice; // DeviceDesc::name
    std::string audioCapture;               // WAV path: the mix runs offline and is written at the end
    std::string userDir;                    // the user directory instead of SDL_GetPrefPath's (tests)
    bool printVersion = false;
    bool printHelp = false;
    std::vector<std::string> gameArgs;

    // Scenario and capture runs step the app time by exactly one fixed step per frame (outside perf regions).
    bool deterministic() const { return !script.empty() || !capture.empty(); }
    // Runs nobody watches: a scenario, a capture or a fixed run time.
    bool automated() const { return deterministic() || quitAfter >= 0.0; }
};

// `args` without the program name. Accepts "--flag value" and "--flag=value".
Result<Options> parseOptions(std::span<const std::string_view> args);
const char* optionsHelp();
std::optional<HdrMode> parseHdrMode(std::string_view text); // off, on, auto

} // namespace quench::app::detail
