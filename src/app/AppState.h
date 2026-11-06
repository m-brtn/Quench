#pragma once

// The state behind quench::app::App, owned by the SDL main callbacks (Main.cpp). Members are destroyed in reverse
// order: the game first, then ImGui, the GPU device, the window, the audio device, the mixer, the asset manager, the
// job system, the file system backend and the log sink.

#include "DisplayHeadroom.h"
#include "Options.h"
#include "UiComposite.h"

#include <quench/app/App.h>
#include <quench/app/ImGuiRenderer.h>
#include <quench/app/Input.h>
#include <quench/app/Scenario.h>
#include <quench/assets/AssetManager.h>
#include <quench/core/FileSystem.h>
#include <quench/core/FixedStepClock.h>
#include <quench/core/Jobs.h>

#include <SDL3/SDL.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImGuiContext;

namespace quench::app::detail {

// Scenario runs keep this much of the mix for audio_measure.
inline constexpr double kAudioMonitorSeconds = 4.0;

// SDL_CommonEvent::reserved of the events a scenario pushes, which SDL leaves alone: runs that take only scripted
// input drop device events without it (and gamepad events of other gamepads than the scenario's virtual one).
inline constexpr Uint32 kScenarioEvent = 0x51534345;

struct LogSinkGuard {
    int id = -1;
    ~LogSinkGuard();
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
};

struct ImGuiContextGuard {
    ImGuiContext* context = nullptr;
    bool platform = false;
    ~ImGuiContextGuard();
};

// The CPU time of the process's threads, all of it and the part on performance cores (processCpuTime).
struct ProcessCpuTime {
    double totalMs = 0.0;
    double performanceMs = 0.0;
};

// Frame-time statistics of a scenario's perf_begin .. perf_end region.
struct PerfRegion {
    std::string label;
    std::uint64_t serial = 0; // the frames' fences carry it (AppState::fencePerf)
    // A frame's time runs from its start to the next frame's start, so it is added at the next frame's start.
    std::vector<double> frameMs;
    std::vector<double> cpuMs;
    // The wait for the swapchain texture (offscreen: for the oldest frame's fence), left out of cpuMs: paced presents
    // and a GPU-bound frame wait here.
    std::vector<double> waitMs;
    // One sample per frame of the region, read when the frame's fence has signalled (later than the frame itself);
    // timedFrames: the region's frames on a device that times them.
    std::vector<double> gpuMs;
    std::size_t timedFrames = 0;
    // GPU busy time per timed range (FrameStats::gpuPasses), by name in the order first seen; per frame the sum of the
    // top-level ranges, the time they span together and its share of the frame's GPU time.
    std::vector<std::pair<std::string, std::vector<double>>> passMs;
    std::vector<double> passSumMs;
    std::vector<double> passSpanMs;
    std::vector<double> passShare;
    ThermalState thermalStart = ThermalState::Unknown;
    ThermalState thermalMax = ThermalState::Unknown;
    std::optional<ProcessCpuTime> processCpuStart;
};

// Multi-frame scenario input (tap, click, touch): what the next frames still have to send.
struct PendingInput {
    enum class Kind { None, KeyUp, MouseUp, GamepadUp, TouchMove } kind = Kind::None;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
    int button = 0;                                 // SDL mouse button or SDL_GamepadButton
    float x = 0.0f, y = 0.0f, dx = 0.0f, dy = 0.0f; // touch: normalized position and step
    int steps = 0;
};

struct AppState {
    AppState() : app(*this) {}
    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;

    App app;
    AppDesc desc;
    Options options;
    std::string program;                // argv[0], for App::relaunch
    std::vector<std::string> arguments; // the rest of the command line

    LogSinkGuard logSink;
    std::unique_ptr<JobSystem> jobs;
    std::unique_ptr<assets::AssetManager> assets;
    std::unique_ptr<audio::AudioSystem> audio;
    std::unique_ptr<audio::Device> audioDevice; // null: offline mixing on the frame clock
    std::vector<float> audioCapture;            // --audio-capture: the whole mix
    std::unique_ptr<SDL_Window, WindowDeleter> window;
    std::optional<gpu::Device> device;
    ImGuiContextGuard imgui;
    std::optional<ImGuiRenderer> imguiRenderer;
    Input input;
    SDL_Joystick* virtualPad = nullptr;
    SDL_JoystickID virtualPadId = 0;
    SDL_Gamepad* virtualGamepad = nullptr; // the virtual pad as ImGui's only gamepad (scripted input)
    // No window on the screen (--offscreen; automated runs on desktops): the window stays hidden and unclaimed, frames
    // go only to frameTexture, at --frame-size or the window size in points.
    bool offscreen = false;
    gpu::SwapchainComposition offscreenComposition = gpu::SwapchainComposition::Sdr;
    // Only the script's events reach ImGui, Input and the game (--script, offscreen runs).
    bool scriptedInput = false;
    std::unique_ptr<Game> game;
    bool gameStarted = false;

    // Window and timing.
    int windowWidth = 0, windowHeight = 0;
    std::uint32_t pixelWidth = 0, pixelHeight = 0;
    gpu::TextureFormat swapchainFormat = gpu::TextureFormat::Invalid;
    // HDR output: the mode, what the frames get, the window's display (DisplayHeadroom.h).
    HdrMode hdrMode = HdrMode::Off;
    gpu::DisplayOutput output;
    bool outputDirty = true; // mode, display or headroom changed: updateOutput() before the next frame
    bool hdrSupported = false;
    float displayHeadroom = 1.0f;
    float displayPotentialHeadroom = 1.0f;
    HeadroomLog headroomLog;
    bool background = false;
    FixedStepClock clock;
    double time = 0.0;
    std::uint64_t frame = 0;
    std::uint64_t ticks = 0;
    Uint64 lastCounter = 0;
    double maxFrameRate = 0.0; // App::setMaxFrameRate; 0: the display's rate

    // Statistics.
    FrameStats stats;
    double statsWindowMs = 0.0;
    int statsWindowFrames = 0;
    gpu::Texture frameTexture; // --frame-size
    UiComposite uiComposite;   // HDR output
    std::array<gpu::Fence, 3> fences;
    std::array<std::uint64_t, 3> fencePerf{}; // the serial of the perf region each fence's frame belongs to; 0: none
    std::size_t nextFence = 0;

    // Captures and quitting.
    std::string captureRequest;
    bool scenarioCapturing = false; // the scenario's capture command waits for its frame
    bool capturedFromOptions = false;
    double capturePeak = -1.0; // brightest channel of the last capture, in multiples of paper white; < 0: none yet
    bool quitRequested = false;
    int exitCode = 0;

    // Scenario.
    std::optional<Scenario> scenario;
    bool scenarioDone = false;
    std::uint64_t inputPushedFrame = UINT64_MAX;
    PendingInput pendingInput;
    glm::vec2 touchMouse{0.0f};                      // the mouse position SDL would synthesize from the scripted finger
    std::vector<std::unique_ptr<char[]>> textInputs; // TEXT_INPUT payloads stay alive until dispatched
    std::optional<PerfRegion> perf;
    std::uint64_t perfSerial = 0;
    std::uint64_t lastFramePerf = 0;  // the perf region of the last frame, whose time the next frame's start ends
    bool passTimingRequested = false; // App::requestPassTiming in this frame
    std::map<std::string, double, std::less<>> perfValues;  // of the last finished region: perf_frame_avg, ...
    std::map<std::string, double, std::less<>> audioValues; // of the last audio_measure: audio_rms, ...

    ~AppState();
};

SDL_AppResult init(AppState& state, int argc, char** argv);
SDL_AppResult iterate(AppState& state);
SDL_AppResult event(AppState& state, SDL_Event& event);
void quit(AppState& state, SDL_AppResult result);

// AppScenario.cpp
std::optional<double> scenarioValue(AppState& state, std::string_view name);
Scenario::Exec scenarioCommand(AppState& state, const ScenarioCommand& command);
void releaseScenarioInput(AppState& state);
// Starts a perf region (replacing a running one); ends it: logs its perf line, sets the perf_* values and returns its
// numbers.
void startPerf(AppState& state, std::string label);
PerfReport finishPerf(AppState& state);

// Capture.cpp: writes RGBA8 or BGRA8 pixels, or the Rgba16Float pixels of an HDR extended-linear frame (tightly
// packed), as an opaque 8-bit sRGB PNG. HDR values are divided by paperWhite and clipped at 1: the PNG shows the
// frame as an SDR display would at paper white, highlights clipped. Returns the brightest channel in multiples of
// paper white (8-bit formats: at most 1). Relative paths go to the user directory through quench::fs; absolute paths
// are opened directly (hosts and the iOS Simulator).
Result<double> writePng(std::string_view path, std::span<const std::byte> pixels, std::uint32_t width,
                        std::uint32_t height, gpu::TextureFormat format, float paperWhite = 1.0f);
// A capture file: relative paths into the user directory through quench::fs, absolute paths as given.
Result<void> writeCaptureFile(std::string_view path, std::span<const std::uint8_t> bytes);

// App.cpp: applies the HDR mode and the window's display state to the swapchain and to `output`.
void updateOutput(AppState& state);

// App.cpp: the CPU time of the process's threads so far (Apple platforms; elsewhere none).
std::optional<ProcessCpuTime> processCpuTime();

} // namespace quench::app::detail
