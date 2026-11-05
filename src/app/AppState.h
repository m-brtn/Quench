#pragma once

// The state behind quench::app::App, owned by the SDL main callbacks (Main.cpp). Members are destroyed in reverse
// order: the game first, then ImGui, the GPU device, the window, the file system backend and the log sink.

#include "DisplayHeadroom.h"
#include "Options.h"
#include "UiComposite.h"

#include <quench/app/App.h>
#include <quench/app/ImGuiRenderer.h>
#include <quench/app/Input.h>
#include <quench/app/Scenario.h>
#include <quench/core/FileSystem.h>
#include <quench/core/FixedStepClock.h>

#include <SDL3/SDL.h>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImGuiContext;

namespace quench::app::detail {

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

// Frame-time statistics of a scenario's perf_begin .. perf_end region.
struct PerfRegion {
    std::string label;
    std::vector<double> frameMs;
    std::vector<double> cpuMs;
    std::vector<double> gpuMs;
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

    LogSinkGuard logSink;
    std::unique_ptr<SDL_Window, WindowDeleter> window;
    std::optional<gpu::Device> device;
    ImGuiContextGuard imgui;
    std::optional<ImGuiRenderer> imguiRenderer;
    Input input;
    SDL_Joystick* virtualPad = nullptr;
    SDL_JoystickID virtualPadId = 0;
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

    // Statistics.
    FrameStats stats;
    double statsWindowMs = 0.0;
    int statsWindowFrames = 0;
    gpu::Texture frameTexture; // --frame-size
    UiComposite uiComposite;   // HDR output
    std::array<gpu::Fence, 3> fences;
    std::size_t nextFence = 0;

    // Captures and quitting.
    std::string captureRequest;
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
    std::map<std::string, double, std::less<>> perfValues; // of the last finished region: perf_frame_avg, ...

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

// Capture.cpp: writes RGBA8 or BGRA8 pixels, or the Rgba16Float pixels of an HDR extended-linear frame (tightly
// packed), as an opaque 8-bit sRGB PNG. HDR values are divided by paperWhite and clipped at 1: the PNG shows the
// frame as an SDR display would at paper white, highlights clipped. Returns the brightest channel in multiples of
// paper white (8-bit formats: at most 1). Relative paths go to the user directory through quench::fs; absolute paths
// are opened directly (hosts and the iOS Simulator).
Result<double> writePng(std::string_view path, std::span<const std::byte> pixels, std::uint32_t width,
                        std::uint32_t height, gpu::TextureFormat format, float paperWhite = 1.0f);

// App.cpp: applies the HDR mode and the window's display state to the swapchain and to `output`.
void updateOutput(AppState& state);

} // namespace quench::app::detail
