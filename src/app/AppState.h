#pragma once

// The state behind quench::app::App, owned by the SDL main callbacks (Main.cpp). Members are destroyed in reverse
// order: the game first, then ImGui, the GPU device, the window, the file system backend and the log sink.

#include "Options.h"

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
    std::array<gpu::Fence, 3> fences;
    std::size_t nextFence = 0;

    // Captures and quitting.
    std::string captureRequest;
    bool capturedFromOptions = false;
    bool quitRequested = false;
    int exitCode = 0;

    // Scenario.
    std::optional<Scenario> scenario;
    bool scenarioDone = false;
    std::uint64_t inputPushedFrame = UINT64_MAX;
    PendingInput pendingInput;
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

// Capture.cpp: writes RGBA8 or BGRA8 pixels (tightly packed) as an opaque PNG. Relative paths go to the user
// directory through quench::fs; absolute paths are opened directly (hosts and the iOS Simulator).
Result<void> writePng(std::string_view path, std::span<const std::byte> pixels, std::uint32_t width,
                      std::uint32_t height, gpu::TextureFormat format);

} // namespace quench::app::detail
