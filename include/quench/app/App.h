#pragma once

// The app shell: the engine owns main (SDL main callbacks, src/app/Main.cpp) and drives a Game. A game executable
// defines quench::app::createGame() and is built with quench_add_game() (cmake/QuenchGame.cmake).
//
// Per frame: events -> input -> scenario -> fixed simulation ticks -> update -> audio -> ImGui -> render -> ImGui on
// top -> present. Rendering is skipped while the app is in the background (iOS) or the window is minimized.

#include <quench/app/ImGuiRenderer.h> // imguiTexture()
#include <quench/assets/AssetManager.h>
#include <quench/audio/AudioSystem.h>
#include <quench/audio/Device.h>
#include <quench/core/Jobs.h>
#include <quench/core/Result.h>
#include <quench/gpu/Device.h>

#include <glm/vec2.hpp>

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

// HDR display output (gpu::SwapchainComposition::HdrExtendedLinear). Off: SDR. On: HDR wherever the GPU backend
// can composite the window so, also on a display without HDR range. Auto: HDR while the window's display can show HDR
// range (potential headroom above 1). Without backend support the output stays SDR (one log line).
enum class HdrMode : std::uint8_t { Off, Auto, On };

const char* hdrModeName(HdrMode mode);

struct AppDesc {
    std::string name = "Quench";         // window title; the user directory is SDL_GetPrefPath(organization, name)
    std::string organization = "Quench"; // empty: the user directory is named after `name` alone
    std::string version = "0.0.0";
    std::string assetDir = "assets"; // below SDL_GetBasePath(): next to the executable, or in the app bundle
    int width = 1280;                // window size in points (desktop; mobile windows fill the screen)
    int height = 720;
    double fixedStep = 1.0 / 120.0; // simulation tick
    // Default: SDL GPU, or the QUENCH_GPU_BACKEND environment variable (sdl, metal); --gpu-backend overrides it.
    gpu::Backend gpuBackend = gpu::Backend::Default;
    gpu::PresentMode presentMode = gpu::PresentMode::Vsync;
    HdrMode hdr = HdrMode::Off;    // --hdr overrides it
    audio::AudioSettings audio;    // the mixer: sample rate, voices, buses
    audio::DeviceDesc audioDevice; // the output and the iOS audio session category; --audio-device overrides the name
};

// The target of one frame. The game records its passes into `commands`; the engine draws ImGui on top and presents.
struct Frame {
    gpu::CommandBuffer& commands;
    gpu::TextureRef target; // color target: the swapchain texture, or an offscreen texture on capture frames
    gpu::TextureFormat format = gpu::TextureFormat::Invalid;
    std::uint32_t width = 0; // pixels
    std::uint32_t height = 0;
    // What the values written to `target` mean (SDR, or HDR extended linear with its headroom); as App::output().
    gpu::DisplayOutput output{};
};

// The operating system's thermal state (Apple platforms: NSProcessInfo's thermalState; Unknown elsewhere). Fair and
// above: the system lowers clocks to cool down, so frame times grow without a change in the work.
enum class ThermalState { Unknown = -1, Nominal = 0, Fair = 1, Serious = 2, Critical = 3 };
const char* thermalStateName(ThermalState state); // "unknown", "nominal", "fair", "serious", "critical"

struct FrameStats {
    double frameMs = 0.0; // real time between frames, averaged over 0.5 s
    double fps = 0.0;
    double cpuMs = 0.0;  // last frame: main thread work until the submit, without the wait for a swapchain texture
    double gpuMs = -1.0; // last measured GPU time of a frame; -1 where unavailable (!gpu::Caps::gpuTiming)
    int ticks = 0;       // simulation ticks in the last frame
    ThermalState thermal = ThermalState::Unknown; // read every 0.5 s, with frameMs; a change is logged
    // GPU time per timed range of passes of the last measured frame, in the order they began (gpu::Caps::gpuPassTiming;
    // empty without it, and unless App::requestPassTiming, a perf region or --gpu-pass-timing turned the ranges on a
    // few frames before): the app's "game" (around Game::render, with the renderer's ranges inside), "ui" and "blit".
    std::vector<gpu::GpuTiming> gpuPasses;
};

// A perf region's numbers (App::endPerf; milliseconds, -1 where there were no samples).
struct PerfReport {
    std::string label;
    std::size_t frames = 0;
    double frameAvg = -1.0, frameP50 = -1.0, frameP99 = -1.0, frameMax = -1.0;
    double cpuAvg = -1.0, cpuP50 = -1.0, cpuP99 = -1.0;
    double gpuAvg = -1.0, gpuP50 = -1.0;
    std::size_t gpuSamples = 0;
    // GPU busy time p50 per timed range (FrameStats::gpuPasses), in the order first seen; empty without pass timing.
    std::vector<std::pair<std::string, double>> passP50;
    std::string details; // Game::perfDetails at the end
    // The thermal state at the start and the end of the region and the highest one read in it.
    ThermalState thermalStart = ThermalState::Unknown, thermalEnd = ThermalState::Unknown,
                 thermalMax = ThermalState::Unknown;
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
    // Named pointer positions for `mouse @NAME`, `click @NAME` and `touch @NAME`, in window points (a UI item, a
    // world point projected into the window), so a script does not depend on the window size; nullopt: unknown.
    virtual std::optional<glm::vec2> scenarioPoint(std::string_view name) const {
        (void)name;
        return std::nullopt;
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
    // The engine's thread pool (--worker-threads), created before Game::init and destroyed after Game::shutdown and the
    // game's destructor.
    JobSystem& jobs();
    // The engine's asset cache on that job system (--asset-latency, --hot-reload), updated every frame before
    // Game::update; created and destroyed like jobs(). Asset types are registered by the modules that use them
    // (render::SceneAssets: textures and models).
    assets::AssetManager& assets();
    // The engine's mixer (AppDesc::audio), updated every frame after Game::update; created after assets() and
    // destroyed before it, after the game's destructor (AssetRef<SoundAsset> must be gone by then).
    audio::AudioSystem& audio();
    // The audio output; null when the mix runs offline on the frame clock instead: --no-audio, --audio-capture, or
    // no device could be opened. It stops in the background and starts again in the foreground.
    audio::Device* audioDevice();
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
    // FrameStats::gpuPasses is filled while something reads it: call this in every frame that shows the per-pass
    // times (a frame panel). Also on during a scenario's perf region and with --gpu-pass-timing; otherwise the
    // ranges are not recorded (gpu::Device::setPassTiming), which saves CPU time per frame.
    void requestPassTiming();
    // A frame-time region as a scenario's perf_begin .. perf_end, started by the game (a benchmark mode): the frames
    // until endPerf() run in real time, then the perf line is logged and the perf_* scenario values are set. A new
    // region (from either side) ends the running one without a report. endPerf() without a region: nullopt.
    void beginPerf(std::string label);
    bool perfRunning() const;
    std::optional<PerfReport> endPerf();
    // The part of the window not covered by notches, rounded corners or system bars, in points.
    WindowRect safeArea() const;

    // Writes the next frame (with ImGui) as PNG. Relative paths go to the user directory, absolute ones are used as
    // given (host paths in the iOS Simulator).
    void requestCapture(std::string path);
    void setPresentMode(gpu::PresentMode mode);
    // At most `hz` frames per second on the screen (0: the display's rate; --max-frame-rate overrides it at the
    // start). Frames are paced to whole display refreshes: 60 on a 120 Hz display renders every second refresh (the GPU
    // works half as often, which lowers heat and battery use). Offscreen frames are not limited.
    void setMaxFrameRate(double hz);
    double maxFrameRate() const;

    // HDR output. A new mode takes effect before the next frame; so do changes of the window's display (moved to
    // another display, HDR switched on or off: SDL_EVENT_WINDOW_HDR_STATE_CHANGED) and of its headroom, which the
    // engine reads every frame where the platform has a query for it (Apple: UIScreen, NSScreen).
    void setHdrMode(HdrMode mode);
    HdrMode hdrMode() const;
    // The output of the coming frames: the swapchain composition, and in HDR the headroom (the display's current
    // one, or --hdr-headroom) and the paper white value.
    const gpu::DisplayOutput& output() const;
    bool hdrSupported() const; // the GPU backend can composite this window in HDR (extended linear)
    // HDR range of the window's display (1: none), also in SDR. Current: what it shows now, values above it are
    // clipped; it changes with the brightness and while HDR content is on screen. Potential: the most it can show;
    // HdrMode::Auto turns HDR on when it is above 1.
    float displayHeadroom() const;
    float displayPotentialHeadroom() const;
    void startTextInput();
    void stopTextInput();
    // Ends the app after this frame. A non-zero code makes the process fail.
    void quit(int exitCode = 0);

    // The command line the app was started with, without the program name (engine flags included).
    std::span<const std::string> arguments() const;
    // Whether relaunch() can start the app again: desktops; not phones, tablets or the web.
    bool canRelaunch() const;
    // Whether the app runs on a phone or tablet (iOS, also in the Simulator, and Android): a passively cooled device
    // that lowers its clocks under sustained GPU load (FrameStats::thermal), for a game's quality defaults. Effects
    // still fall back by gpu::Caps.
    bool mobile() const;
    // Starts the program again as a new process with `args` (without the program name) and ends this one after the
    // frame, as quit(0); an error, and the app goes on, where it cannot or the process does not start.
    Result<void> relaunch(std::span<const std::string> args);
    // Physical memory of the process in bytes (Apple platforms: the footprint the system charges it, as Xcode's
    // memory gauge shows); nullopt where unknown. Scenario value memory_mb.
    std::optional<std::uint64_t> memoryFootprint() const;

private:
    friend struct detail::AppState;
    explicit App(detail::AppState& state) : m_state(state) {}
    detail::AppState& m_state;
};

} // namespace quench::app
