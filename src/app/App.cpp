#include "AppState.h"

#include "core/Platform.h"

#include <quench/app/SdlFileSystem.h>
#include <quench/app/SdlLog.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Log.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>

namespace quench::app {

// ------------------------------------------------------------------ App

const AppDesc& App::desc() const { return m_state.desc; }
gpu::Device& App::device() { return *m_state.device; }
SDL_Window* App::window() const { return m_state.window.get(); }
Input& App::input() { return m_state.input; }
const Input& App::input() const { return m_state.input; }
int App::windowWidth() const { return m_state.windowWidth; }
int App::windowHeight() const { return m_state.windowHeight; }
std::uint32_t App::pixelWidth() const { return m_state.pixelWidth; }
std::uint32_t App::pixelHeight() const { return m_state.pixelHeight; }
float App::pixelDensity() const { return m_state.window ? SDL_GetWindowPixelDensity(m_state.window.get()) : 1.0f; }
gpu::TextureFormat App::swapchainFormat() const { return m_state.swapchainFormat; }
double App::time() const { return m_state.time; }
std::uint64_t App::frame() const { return m_state.frame; }
std::uint64_t App::ticks() const { return m_state.ticks; }
double App::alpha() const { return m_state.clock.alpha(); }
bool App::deterministic() const { return m_state.options.deterministic() && !m_state.perf; }
bool App::scripted() const { return !m_state.options.script.empty(); }
const FrameStats& App::stats() const { return m_state.stats; }

WindowRect App::safeArea() const {
    SDL_Rect rect{};
    if (!m_state.window || !SDL_GetWindowSafeArea(m_state.window.get(), &rect) || rect.w <= 0 || rect.h <= 0)
        return {0, 0, m_state.windowWidth, m_state.windowHeight};
    return {rect.x, rect.y, rect.w, rect.h};
}

void App::requestCapture(std::string path) { m_state.captureRequest = std::move(path); }

void App::setPresentMode(gpu::PresentMode mode) {
    if (m_state.device && m_state.window) m_state.device->setPresentMode(m_state.window.get(), mode);
}

void App::startTextInput() {
    if (m_state.window) SDL_StartTextInput(m_state.window.get());
}

void App::stopTextInput() {
    if (m_state.window) SDL_StopTextInput(m_state.window.get());
}

void App::quit(int exitCode) {
    m_state.quitRequested = true;
    m_state.exitCode = std::max(m_state.exitCode, exitCode);
}

namespace detail {
namespace {

// ImGui sizes for the display: the content scale (user setting on desktops), larger on touch screens.
float uiScale(SDL_Window* window) {
    float scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    if (scale <= 0.0f) scale = 1.0f;
#if QUENCH_PLATFORM_IOS || QUENCH_PLATFORM_ANDROID
    scale *= 1.5f;
#endif
    return scale;
}

void updateWindowSize(AppState& s) {
    SDL_GetWindowSize(s.window.get(), &s.windowWidth, &s.windowHeight);
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(s.window.get(), &w, &h);
    s.pixelWidth = std::uint32_t(std::max(0, w));
    s.pixelHeight = std::uint32_t(std::max(0, h));
}

SDL_AppResult fail(std::string_view what) {
    log::error("app", "{}", what);
    return SDL_APP_FAILURE;
}

int scenarioExitCode(const AppState& s) { return s.scenario && s.scenario->failed() > 0 ? 1 : 0; }

void updateGpuTime(AppState& s) {
    for (gpu::Fence& fence : s.fences) {
        if (!fence || !s.device->isSignaled(fence)) continue;
        if (const auto ms = s.device->gpuMilliseconds(fence)) s.stats.gpuMs = *ms;
        fence.reset();
    }
}

void runScenario(AppState& s) {
    if (!s.scenario || s.scenarioDone) return;
    const bool running = s.scenario->update(
        s.time, [&] { return s.inputPushedFrame == s.frame; },
        [&](std::string_view name) { return scenarioValue(s, name); },
        [&](const ScenarioCommand& command) { return scenarioCommand(s, command); });
    if (running) return;
    s.scenarioDone = true;
    log::info("scenario", "{}: {} passed, {} failed", s.scenario->name(), s.scenario->passed(), s.scenario->failed());
    s.app.quit(scenarioExitCode(s));
}

// Renders the frame. Returns false when the frame could not be recorded (logged). waitMs: the wait for a swapchain
// texture; submitCounter: when recording ended (presenting may block in the submit, on Vulkan for example).
bool render(AppState& s, double& waitMs, Uint64& submitCounter) {
    gpu::CommandBuffer commands = s.device->acquireCommandBuffer();
    if (!commands) return false;
    ImDrawData* drawData = ImGui::GetDrawData();
    s.imguiRenderer->prepare(commands, drawData);

    const Uint64 waitStart = SDL_GetPerformanceCounter();
    const gpu::TextureRef swapchain = commands.acquireSwapchainTexture(s.window.get());
    waitMs = double(SDL_GetPerformanceCounter() - waitStart) * 1000.0 / double(SDL_GetPerformanceFrequency());
    if (!swapchain) {
        submitCounter = SDL_GetPerformanceCounter();
        (void)commands.submit(); // minimized or hidden
        return true;
    }

    std::string capturePath = std::move(s.captureRequest);
    s.captureRequest.clear();
    if (capturePath.empty() && !s.options.capture.empty() && !s.capturedFromOptions &&
        s.time + 1e-9 >= s.options.captureAt) {
        capturePath = s.options.capture;
        s.capturedFromOptions = true;
    }

    // Offscreen frames (--frame-size, captures) are blitted to the swapchain; captures are read back after the
    // submit.
    gpu::Texture captureTexture;
    gpu::TextureRef target = swapchain;
    const bool fixedSize = s.options.frameWidth > 0;
    if (fixedSize || !capturePath.empty()) {
        const std::uint32_t width = fixedSize ? std::uint32_t(s.options.frameWidth) : swapchain.desc.width;
        const std::uint32_t height = fixedSize ? std::uint32_t(s.options.frameHeight) : swapchain.desc.height;
        gpu::Texture& texture = fixedSize ? s.frameTexture : captureTexture;
        if (!texture) {
            Result<gpu::Texture> created =
                s.device->createTexture({.format = s.swapchainFormat,
                                         .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::ColorTarget,
                                         .width = width,
                                         .height = height,
                                         .name = fixedSize ? "frame" : "capture"});
            if (!created) {
                log::error("app", "offscreen frame {}x{}: {}", width, height, created.error());
                return false;
            }
            texture = std::move(*created);
        }
        target = texture;
    }

    Frame frame{commands, target, s.swapchainFormat, target.desc.width, target.desc.height};
    s.game->render(s.app, frame);
    {
        const gpu::ColorTarget color{.texture = target, .load = gpu::LoadOp::Load};
        gpu::RenderPass pass = commands.beginRenderPass(color);
        if (pass) s.imguiRenderer->draw(pass, drawData, s.swapchainFormat, target.desc.width, target.desc.height);
    }
    const bool offscreen = target.native != swapchain.native;
    if (offscreen) {
        const bool sameSize = target.desc.width == swapchain.desc.width && target.desc.height == swapchain.desc.height;
        commands.blit({.source = {.texture = target},
                       .destination = {.texture = swapchain},
                       .filter = sameSize ? gpu::Filter::Nearest : gpu::Filter::Linear});
    }

    submitCounter = SDL_GetPerformanceCounter();
    Result<gpu::Fence> fence = commands.submitWithFence();
    if (!fence) {
        log::error("app", "submit: {}", fence.error());
        return false;
    }
    s.fences[s.nextFence] = std::move(*fence);
    s.nextFence = (s.nextFence + 1) % s.fences.size();

    if (!capturePath.empty()) {
        gpu::TextureRef source = target;
        Result<std::vector<std::byte>> pixels = s.device->download({.texture = source});
        Result<void> written =
            pixels ? writePng(capturePath, *pixels, source.desc.width, source.desc.height, s.swapchainFormat)
                   : Result<void>(Error{pixels.error()});
        if (written)
            log::info("app", "captured {} ({}x{}, frame {}, t={:.3f} s)", capturePath, source.desc.width,
                      source.desc.height, s.frame, s.time);
        else
            log::error("app", "capture {}: {}", capturePath, written.error());
        if (!written) s.app.quit(1);
    }
    return true;
}

void updateStats(AppState& s, double realDt, double cpuMs, int ticks) {
    s.statsWindowMs += realDt * 1000.0;
    ++s.statsWindowFrames;
    if (s.statsWindowMs >= 500.0) {
        s.stats.frameMs = s.statsWindowMs / s.statsWindowFrames;
        s.stats.fps = 1000.0 / s.stats.frameMs;
        s.statsWindowMs = 0.0;
        s.statsWindowFrames = 0;
    }
    s.stats.cpuMs = cpuMs;
    s.stats.ticks = ticks;
    if (s.perf) {
        s.perf->frameMs.push_back(realDt * 1000.0);
        s.perf->cpuMs.push_back(cpuMs);
        if (s.stats.gpuMs >= 0.0) s.perf->gpuMs.push_back(s.stats.gpuMs);
    }
}

} // namespace

LogSinkGuard::~LogSinkGuard() {
    if (id < 0) return;
    log::removeSink(id);
    log::setStderr(true);
}

ImGuiContextGuard::~ImGuiContextGuard() {
    if (!context) return;
    if (platform) ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context);
}

AppState::~AppState() {
    if (virtualPad) SDL_CloseJoystick(virtualPad);
    if (virtualPadId) SDL_DetachVirtualJoystick(virtualPadId);
}

SDL_AppResult init(AppState& s, int argc, char** argv) {
    std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0), argv + std::max(argc, 1));
    Result<Options> options = parseOptions(args);
    if (!options) {
        std::fprintf(stderr, "%s\n%s", options.error().c_str(), optionsHelp());
        return SDL_APP_FAILURE;
    }
    s.options = std::move(*options);

    std::vector<std::string_view> gameArgs(s.options.gameArgs.begin(), s.options.gameArgs.end());
    if (s.options.printHelp) {
        std::printf("%s", optionsHelp());
        gameArgs.push_back("--help");
    }
    Result<std::unique_ptr<Game>> game = createGame(s.desc, gameArgs);
    if (s.options.printVersion) {
        std::printf("%s %s\n", s.desc.name.c_str(), s.desc.version.c_str());
        return SDL_APP_SUCCESS;
    }
    if (s.options.printHelp) return SDL_APP_SUCCESS;
    if (!game) {
        std::fprintf(stderr, "%s\n", game.error().c_str());
        return SDL_APP_FAILURE;
    }
    s.game = std::move(*game);
    if (!s.game) return SDL_APP_SUCCESS;

    SDL_SetAppMetadata(s.desc.name.c_str(), s.desc.version.c_str(), nullptr);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail(std::format("SDL_Init: {}", SDL_GetError()));
    s.logSink.id = addSdlLogSink();
    log::info("app", "{} {} (Quench {}), SDL {}.{}.{}", s.desc.name, s.desc.version, QUENCH_VERSION,
              SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
              SDL_VERSIONNUM_MICRO(SDL_GetVersion()));

    Result<std::unique_ptr<fs::Backend>> files =
        makeSdlFileSystem({.org = s.desc.organization, .app = s.desc.name, .assetDir = s.desc.assetDir, .userDir = {}});
    if (!files) return fail(std::format("file system: {}", files.error()));
    fs::setBackend(std::move(*files));

    if (!s.options.script.empty()) {
        // A host path given on the command line (also in the iOS Simulator), not an asset. On a device the host's
        // files are out of reach: a relative path not found there is read from the user directory.
        std::string text;
        size_t size = 0;
        if (void* data = SDL_LoadFile(s.options.script.c_str(), &size)) {
            text.assign(static_cast<const char*>(data), size);
            SDL_free(data);
        } else {
            const std::string error = SDL_GetError();
            Result<std::string> user = std::filesystem::path(s.options.script).is_relative()
                                           ? fs::readUserText(s.options.script)
                                           : Result<std::string>(Error{error});
            if (!user) return fail(std::format("--script {}: {}", s.options.script, error));
            text = std::move(*user);
        }
        Result<Scenario> parsed = Scenario::parse(text, s.options.script);
        if (!parsed) return fail(parsed.error());
        s.scenario = std::move(*parsed);
        log::info("scenario", "{}: {} commands", s.options.script, s.scenario->commands().size());
    }

    const int width = s.options.windowWidth > 0 ? s.options.windowWidth : s.desc.width;
    const int height = s.options.windowHeight > 0 ? s.options.windowHeight : s.desc.height;
    s.window.reset(
        SDL_CreateWindow(s.desc.name.c_str(), width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if (!s.window) return fail(std::format("SDL_CreateWindow: {}", SDL_GetError()));

    Result<gpu::Device> device =
        gpu::Device::create({.driver = s.options.gpuDriver, .debug = s.options.gpuDebug, .preferLowPower = false});
    if (!device) return fail(std::format("GPU device: {}", device.error()));
    s.device.emplace(std::move(*device));
    const gpu::PresentMode presentMode = s.options.presentMode.value_or(s.desc.presentMode);
    if (Result<void> claimed = s.device->claimWindow(s.window.get(), presentMode); !claimed)
        return fail(std::format("GPU window: {}", claimed.error()));
    s.swapchainFormat = s.device->swapchainFormat(s.window.get());
    const gpu::Caps& caps = s.device->caps();
    log::info("app", "GPU: {} ({}), swapchain {}", caps.driver, caps.deviceName.empty() ? "?" : caps.deviceName,
              gpu::formatInfo(s.swapchainFormat).name);

    IMGUI_CHECKVERSION();
    s.imgui.context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window layouts are not saved (user files go through quench::fs)
    io.LogFilename = nullptr;
    const float scale = uiScale(s.window.get());
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    if (!ImGui_ImplSDL3_InitForOther(s.window.get())) return fail("ImGui SDL3 backend");
    s.imgui.platform = true;
    Result<ImGuiRenderer> renderer = ImGuiRenderer::create(*s.device);
    if (!renderer) return fail(std::format("ImGui renderer: {}", renderer.error()));
    s.imguiRenderer.emplace(std::move(*renderer));

    updateWindowSize(s);
    s.clock.setStep(s.desc.fixedStep);
    s.input.beginEvents();
    if (Result<void> started = s.game->init(s.app); !started)
        return fail(std::format("game init: {}", started.error()));
    s.gameStarted = true;
    s.lastCounter = SDL_GetPerformanceCounter();
    return SDL_APP_CONTINUE;
}

SDL_AppResult iterate(AppState& s) {
    const Uint64 now = SDL_GetPerformanceCounter();
    const double frequency = double(SDL_GetPerformanceFrequency());
    const double realDt = double(now - s.lastCounter) / frequency;
    s.lastCounter = now;

    const bool minimized = (SDL_GetWindowFlags(s.window.get()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
    if (s.background || minimized) {
        // No GPU work in the background (iOS forbids it); the clock does not advance meanwhile.
        s.input.endEvents(false, false);
        s.input.beginEvents();
        SDL_Delay(16);
        s.lastCounter = SDL_GetPerformanceCounter();
        return SDL_APP_CONTINUE;
    }

    updateWindowSize(s);
    updateGpuTime(s);
    const ImGuiIO& io = ImGui::GetIO();
    s.input.endEvents(io.WantCaptureKeyboard, io.WantCaptureMouse);

    runScenario(s);
    if (s.pendingInput.kind == PendingInput::Kind::None && s.inputPushedFrame != s.frame) s.textInputs.clear();

    const double step = s.clock.step();
    const bool fixedFrame = s.app.deterministic();
    const double frameDt = fixedFrame ? step : realDt;
    const int ticks = s.clock.advance(frameDt);
    for (int i = 0; i < ticks; ++i) {
        s.game->fixedStep(s.app, step);
        ++s.ticks;
    }
    s.time += frameDt;
    s.game->update(s.app, frameDt);

    ImGui_ImplSDL3_NewFrame();
    ImGuiIO& imguiIo = ImGui::GetIO();
    imguiIo.DeltaTime = float(std::max(frameDt, 1e-4));
    if (s.options.frameWidth > 0 && imguiIo.DisplaySize.x > 0.0f && imguiIo.DisplaySize.y > 0.0f)
        imguiIo.DisplayFramebufferScale = ImVec2(float(s.options.frameWidth) / imguiIo.DisplaySize.x,
                                                 float(s.options.frameHeight) / imguiIo.DisplaySize.y);
    ImGui::NewFrame();
    s.game->ui(s.app);
    ImGui::Render();

    double waitMs = 0.0;
    Uint64 submitCounter = now;
    if (!render(s, waitMs, submitCounter)) return SDL_APP_FAILURE;

    const double cpuMs = double(submitCounter - now) * 1000.0 / frequency - waitMs;
    updateStats(s, realDt, cpuMs, ticks);
    if (fixedFrame) {
        const double spent = double(SDL_GetPerformanceCounter() - now) / frequency;
        if (spent < step) SDL_DelayPrecise(Uint64((step - spent) * 1e9));
    }

    if (s.options.quitAfter >= 0.0 && s.time + 1e-9 >= s.options.quitAfter && !s.quitRequested) {
        log::info("app", "quit after {:.3f} s: {} frames, {} ticks", s.time, s.frame + 1, s.ticks);
        s.app.quit(scenarioExitCode(s));
    }
    ++s.frame;
    s.input.beginEvents();
    if (s.quitRequested) return s.exitCode == 0 ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    return SDL_APP_CONTINUE;
}

SDL_AppResult event(AppState& s, SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_QUIT: s.app.quit(scenarioExitCode(s)); return s.exitCode == 0 ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    case SDL_EVENT_TERMINATING: log::info("app", "terminating"); break;
    case SDL_EVENT_LOW_MEMORY: log::warning("app", "low memory"); break;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        s.background = true;
        log::info("app", "entering background");
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        if (!s.background) break; // also sent at startup on iOS
        s.background = false;
        s.clock.reset();
        s.lastCounter = SDL_GetPerformanceCounter();
        log::info("app", "back in foreground");
        break;
    default: break;
    }
    if (s.imgui.platform) ImGui_ImplSDL3_ProcessEvent(&e);
    s.input.handleEvent(e, float(std::max(1, s.windowWidth)), float(std::max(1, s.windowHeight)));
    if (s.gameStarted) s.game->event(s.app, e);
    return SDL_APP_CONTINUE;
}

void quit(AppState& s, SDL_AppResult result) {
    if (s.device) s.device->waitIdle();
    if (s.gameStarted) s.game->shutdown(s.app);
    s.gameStarted = false;
    s.game.reset();
    releaseScenarioInput(s);
    for (gpu::Fence& fence : s.fences) fence.reset();
    s.frameTexture.reset();
    if (s.imguiRenderer) s.imguiRenderer->shutdown();
    s.imguiRenderer.reset();
    if (s.imgui.platform) ImGui_ImplSDL3_Shutdown();
    s.imgui.platform = false;
    if (s.imgui.context) ImGui::DestroyContext(s.imgui.context);
    s.imgui.context = nullptr;
    if (s.device) {
        if (const std::int64_t live = s.device->liveResources(); live != 0)
            log::warning("app", "{} GPU resources still alive at exit", live);
        s.device->releaseWindow(s.window.get());
    }
    s.device.reset();
    s.window.reset();
    fs::setBackend(nullptr);
    if (result == SDL_APP_FAILURE) log::error("app", "exit with failure");
}

} // namespace detail
} // namespace quench::app
