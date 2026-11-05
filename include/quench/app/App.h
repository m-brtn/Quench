#pragma once

// The app shell: the engine owns main (SDL main callbacks, src/app/Main.cpp) and drives a Game. A game executable
// defines quench::app::createGame() and is built with quench_add_game() (cmake/QuenchGame.cmake).
//
// Per frame: events -> input -> scenario -> fixed simulation ticks -> update -> ImGui -> render -> ImGui on top ->
// present. Rendering is skipped while the app is in the background (iOS) or the window is minimized.

#include <quench/app/ImGuiRenderer.h> // imguiTexture()
#include <quench/core/Result.h>
#include <quench/gpu/Device.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace quench::app {

class App;
class Input;
struct ScenarioCommand;

struct AppDesc {
    std::string name = "Quench";         // window title; the user directory is SDL_GetPrefPath(organization, name)
    std::string organization = "Quench"; // empty: the user directory is named after `name` alone
    std::string version = "0.0.0";
    std::string assetDir = "assets"; // below SDL_GetBasePath(): next to the executable, or in the app bundle
    int width = 1280;                // window size in points (desktop; mobile windows fill the screen)
    int height = 720;
    double fixedStep = 1.0 / 120.0; // simulation tick
    gpu::PresentMode presentMode = gpu::PresentMode::Vsync;
};

// The target of one frame. The game records its passes into `commands`; the engine draws ImGui on top and presents.
struct Frame {
    gpu::CommandBuffer& commands;
    gpu::TextureRef target; // color target: the swapchain texture, or an offscreen texture on capture frames
    gpu::TextureFormat format = gpu::TextureFormat::Invalid;
    std::uint32_t width = 0; // pixels
    std::uint32_t height = 0;
};

struct FrameStats {
    double frameMs = 0.0; // real time between frames, averaged over 0.5 s
    double fps = 0.0;
    double cpuMs = 0.0;  // last frame: main thread work until the submit, without the wait for a swapchain texture
    double gpuMs = -1.0; // last measured GPU time of a frame; -1 where unavailable (!gpu::Caps::gpuTiming)
    int ticks = 0;       // simulation ticks in the last frame
};

struct WindowRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class ScenarioExec { Done, Busy, Failed, Unknown };

class Game {
public:
    virtual ~Game() = default;

    // After the window, the GPU device and ImGui exist. An error ends the app with a failure exit code.
    virtual Result<void> init(App& app) = 0;
    // A simulation tick of AppDesc::fixedStep seconds; none, one or several per frame.
    virtual void fixedStep(App& app, double dt) {
        (void)app;
        (void)dt;
    }
    // Once per frame after the ticks; App::alpha() interpolates between the last two ticks.
    virtual void update(App& app, double frameDt) {
        (void)app;
        (void)frameDt;
    }
    // ImGui windows of this frame.
    virtual void ui(App& app) { (void)app; }
    // Records the frame. ImGui is drawn on top of frame.target afterwards.
    virtual void render(App& app, Frame& frame) = 0;
    // Every SDL event, after the engine has handled it.
    virtual void event(App& app, const SDL_Event& event) {
        (void)app;
        (void)event;
    }
    // Before the GPU device is destroyed: release GPU resources here at the latest.
    virtual void shutdown(App& app) { (void)app; }

    // Scenario support (--script): values for `until` and `expect`, and the commands the engine does not know.
    virtual std::optional<double> scenarioValue(std::string_view name) const {
        (void)name;
        return std::nullopt;
    }
    virtual ScenarioExec scenarioCommand(App& app, const ScenarioCommand& command) {
        (void)app;
        (void)command;
        return ScenarioExec::Unknown;
    }
    // Game fields appended to the log line of a scenario perf region at perf_end, e.g. "draws=812 render=1920x1080".
    virtual std::string perfDetails(App& app) {
        (void)app;
        return {};
    }
};

// Defined by the game, once per executable. Called before SDL is initialized, with the command line arguments the
// engine did not consume; the game fills `desc` and returns itself, or an error for an argument it does not know.
// A null game without an error ends the program successfully (for a game flag that only prints, like --paths).
Result<std::unique_ptr<Game>> createGame(AppDesc& desc, std::span<const std::string_view> args);

namespace detail {
struct AppState;
}

// The engine services a game uses. Valid from Game::init until Game::shutdown; main thread only.
class App {
public:
    const AppDesc& desc() const;
    gpu::Device& device();
    SDL_Window* window() const;
    Input& input();
    const Input& input() const;

    int windowWidth() const; // points
    int windowHeight() const;
    std::uint32_t pixelWidth() const; // the swapchain size
    std::uint32_t pixelHeight() const;
    float pixelDensity() const; // pixels per point
    gpu::TextureFormat swapchainFormat() const;

    double time() const;         // app time in seconds (see deterministic())
    std::uint64_t frame() const; // frames since the start
    std::uint64_t ticks() const; // simulation ticks since the start
    double alpha() const;        // fraction of a tick since the last one, for interpolation
    // Scenario and capture runs (--script, --capture): every frame advances the app time by exactly one fixed step
    // and runs one tick, so results do not depend on the display rate. Such frames are never shorter than the step in
    // real time (background work such as streaming keeps its pace), but may be longer: app time then runs slower than
    // real time. False inside a scenario's perf_begin .. perf_end region, which runs in real time like normal play.
    bool deterministic() const;
    bool scripted() const; // a --script scenario runs (games may start from defaults and keep user files untouched)
    const FrameStats& stats() const;
    // The part of the window not covered by notches, rounded corners or system bars, in points.
    WindowRect safeArea() const;

    // Writes the next frame (with ImGui) as PNG. Relative paths go to the user directory, absolute ones are used as
    // given (host paths in the iOS Simulator).
    void requestCapture(std::string path);
    void setPresentMode(gpu::PresentMode mode);
    void startTextInput();
    void stopTextInput();
    // Ends the app after this frame. A non-zero code makes the process fail.
    void quit(int exitCode = 0);

private:
    friend struct detail::AppState;
    explicit App(detail::AppState& state) : m_state(state) {}
    detail::AppState& m_state;
};

} // namespace quench::app
