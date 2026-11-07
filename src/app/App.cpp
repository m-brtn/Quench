#include "AppState.h"
#include "ThermalState.h"

#include "core/Platform.h"

#include <quench/app/SdlFileSystem.h>
#include <quench/app/SdlLog.h>
#include <quench/audio/Wav.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Log.h>

#include <SDL3/SDL_process.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#if QUENCH_PLATFORM_APPLE
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif
#if QUENCH_PLATFORM_MACOS
extern "C" void quench_app_prohibit_activation(void); // ActivationApple.m
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>

// The asset overlay (docs/assets.md, "Hot reload on a device"): in development builds (without NDEBUG) unless the
// QUENCH_ASSET_OVERLAY CMake option sets it.
#ifndef QUENCH_ASSET_OVERLAY
#ifdef NDEBUG
#define QUENCH_ASSET_OVERLAY 0
#else
#define QUENCH_ASSET_OVERLAY 1
#endif
#endif

namespace quench::app {
#if QUENCH_ASSET_OVERLAY
namespace {
constexpr const char* kAssetOverlayDir = "asset_overlay";
}
#endif

// ------------------------------------------------------------------ App

const AppDesc& App::desc() const { return m_state.desc; }
gpu::Device& App::device() { return *m_state.device; }
JobSystem& App::jobs() { return *m_state.jobs; }
assets::AssetManager& App::assets() { return *m_state.assets; }
audio::AudioSystem& App::audio() { return *m_state.audio; }
audio::Device* App::audioDevice() { return m_state.audioDevice.get(); }
SDL_Window* App::window() const { return m_state.window.get(); }
Input& App::input() { return m_state.input; }
const Input& App::input() const { return m_state.input; }
int App::windowWidth() const { return m_state.windowWidth; }
int App::windowHeight() const { return m_state.windowHeight; }
std::uint32_t App::pixelWidth() const { return m_state.pixelWidth; }
std::uint32_t App::pixelHeight() const { return m_state.pixelHeight; }
float App::pixelDensity() const {
    return m_state.window && !m_state.offscreen ? SDL_GetWindowPixelDensity(m_state.window.get()) : 1.0f;
}
gpu::TextureFormat App::swapchainFormat() const { return m_state.swapchainFormat; }
double App::time() const { return m_state.time; }
std::uint64_t App::frame() const { return m_state.frame; }
std::uint64_t App::ticks() const { return m_state.ticks; }
double App::alpha() const { return m_state.clock.alpha(); }
bool App::deterministic() const { return m_state.options.deterministic() && !m_state.perf; }
bool App::scripted() const { return !m_state.options.script.empty(); }
const FrameStats& App::stats() const { return m_state.stats; }
void App::requestPassTiming() { m_state.passTimingRequested = true; }

void App::beginPerf(std::string label) { detail::startPerf(m_state, std::move(label)); }

bool App::perfRunning() const { return m_state.perf.has_value(); }

std::optional<PerfReport> App::endPerf() {
    if (!m_state.perf) return std::nullopt;
    return detail::finishPerf(m_state);
}

WindowRect App::safeArea() const {
    SDL_Rect rect{};
    if (!m_state.window || !SDL_GetWindowSafeArea(m_state.window.get(), &rect) || rect.w <= 0 || rect.h <= 0)
        return {0, 0, m_state.windowWidth, m_state.windowHeight};
    return {rect.x, rect.y, rect.w, rect.h};
}

void App::requestCapture(std::string path) { m_state.captureRequest = std::move(path); }

void App::setPresentMode(gpu::PresentMode mode) {
    if (m_state.device && m_state.window && !m_state.offscreen)
        m_state.device->setPresentMode(m_state.window.get(), mode);
}

void App::setMaxFrameRate(double hz) {
    hz = std::isfinite(hz) ? std::clamp(hz, 0.0, 1000.0) : 0.0;
    if (hz == m_state.maxFrameRate) return;
    m_state.maxFrameRate = hz;
    if (hz > 0.0)
        log::info("app", "max frame rate {} Hz", hz);
    else
        log::info("app", "max frame rate: the display's");
    detail::applyFrameRateLimit(m_state);
}

double App::maxFrameRate() const { return m_state.maxFrameRate; }
ThermalGovernor& App::thermalGovernor() { return m_state.thermalGovernor; }
const ThermalGovernor& App::thermalGovernor() const { return m_state.thermalGovernor; }
double App::frameRateLimit() const { return m_state.frameRateLimit; }

void App::setHdrMode(HdrMode mode) {
    m_state.hdrMode = mode;
    m_state.outputDirty = true;
}

HdrMode App::hdrMode() const { return m_state.hdrMode; }
const gpu::DisplayOutput& App::output() const { return m_state.output; }
bool App::hdrSupported() const { return m_state.hdrSupported; }
float App::displayHeadroom() const { return m_state.displayHeadroom; }
float App::displayPotentialHeadroom() const { return m_state.displayPotentialHeadroom; }

const char* hdrModeName(HdrMode mode) {
    switch (mode) {
    case HdrMode::Off: return "off";
    case HdrMode::Auto: return "auto";
    case HdrMode::On: return "on";
    }
    return "?";
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

std::span<const std::string> App::arguments() const { return m_state.arguments; }

bool App::canRelaunch() const {
    return (QUENCH_PLATFORM_MACOS || QUENCH_PLATFORM_WINDOWS || QUENCH_PLATFORM_LINUX) && !m_state.program.empty();
}

bool App::mobile() const { return QUENCH_PLATFORM_IOS || QUENCH_PLATFORM_ANDROID; }

Result<void> App::relaunch(std::span<const std::string> args) {
    if (!canRelaunch()) return Error{"relaunch: not possible on this platform"};
    std::vector<const char*> argv{m_state.program.c_str()};
    for (const std::string& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!process) return Error{std::format("relaunch {}: {}", m_state.program, SDL_GetError())};
    SDL_DestroyProcess(process); // only the handle: the new process runs on
    log::info("app", "relaunched {} with {} arguments; quitting", m_state.program, args.size());
    quit(0);
    return {};
}

std::optional<std::uint64_t> App::memoryFootprint() const {
#if QUENCH_PLATFORM_APPLE
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return std::nullopt;
    return std::uint64_t(info.phys_footprint);
#else
    return std::nullopt;
#endif
}

namespace detail {

std::optional<ProcessCpuTime> processCpuTime() {
#if QUENCH_PLATFORM_APPLE
    // Times in Mach absolute time units, as the kernel's thread timers count them.
    task_power_info_v2_data_t info{};
    mach_msg_type_number_t count = TASK_POWER_INFO_V2_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO_V2, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return std::nullopt;
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.denom == 0) return std::nullopt;
    const auto ms = [&](std::uint64_t t) { return double(t) * timebase.numer / timebase.denom / 1e6; };
    return ProcessCpuTime{.totalMs = ms(info.cpu_energy.total_user + info.cpu_energy.total_system),
                          .performanceMs = ms(info.task_ptime)};
#else
    return std::nullopt;
#endif
}

void applyFrameRateLimit(AppState& s) {
    const double hz = s.thermalGovernor.frameRateLimit(s.maxFrameRate);
    if (hz == s.frameRateLimit) return;
    s.frameRateLimit = hz;
    // Desktops: SDL's main loop waits between the callbacks. iOS calls them from a display link on every refresh, which
    // the hint does not change: iterate() lets the early ones pass without a frame.
    if (!s.offscreen) SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, std::format("{}", hz).c_str());
    if (hz != s.maxFrameRate) log::info("app", "frame rate limit {} Hz (thermal reaction)", hz);
}

void updateThermalState(AppState& s) {
    const ThermalState state = s.thermalOverride ? *s.thermalOverride : queryThermalState();
    if (state == s.stats.thermal) return;
    log::info("app", "thermal state {} -> {}{}", thermalStateName(s.stats.thermal), thermalStateName(state),
              s.thermalOverride ? " (override)" : "");
    s.stats.thermal = state;
}

ThermalState thermalReaction(const AppState& s) {
    return s.thermalGovernor.enabled() ? s.thermalGovernor.level() : ThermalState::Unknown;
}

namespace {

// ImGui sizes for the display: the content scale (user setting on desktops; 1 offscreen, where no display is
// involved), larger on touch screens.
float uiScale(const AppState& s) {
    float scale = s.offscreen ? 1.0f : SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(s.window.get()));
    if (scale <= 0.0f) scale = 1.0f;
#if QUENCH_PLATFORM_IOS || QUENCH_PLATFORM_ANDROID
    scale *= 1.5f;
#endif
    return scale;
}

// Offscreen the pixel size is the frame's: --frame-size, else the window size in points.
void updateWindowSize(AppState& s) {
    SDL_GetWindowSize(s.window.get(), &s.windowWidth, &s.windowHeight);
    int w = 0, h = 0;
    if (s.offscreen) {
        w = s.options.frameWidth > 0 ? s.options.frameWidth : s.windowWidth;
        h = s.options.frameWidth > 0 ? s.options.frameHeight : s.windowHeight;
    } else {
        SDL_GetWindowSizeInPixels(s.window.get(), &w, &h);
    }
    s.pixelWidth = std::uint32_t(std::max(0, w));
    s.pixelHeight = std::uint32_t(std::max(0, h));
}

// The format a window's swapchain has with each composition (Metal, Vulkan): offscreen frames get the same.
gpu::TextureFormat offscreenFormat(gpu::SwapchainComposition composition) {
    switch (composition) {
    case gpu::SwapchainComposition::Sdr: return gpu::TextureFormat::Bgra8Unorm;
    case gpu::SwapchainComposition::SdrLinear: return gpu::TextureFormat::Bgra8UnormSrgb;
    case gpu::SwapchainComposition::HdrExtendedLinear: return gpu::TextureFormat::Rgba16Float;
    case gpu::SwapchainComposition::Hdr10: return gpu::TextureFormat::Rgb10a2Unorm;
    }
    return gpu::TextureFormat::Bgra8Unorm;
}

// Keyboard, text, mouse, joystick, gamepad, touch, pinch, drop and pen events: what runs on scripted input ignore
// unless the scenario sent it.
bool deviceInput(const SDL_Event& e) {
    return (e.type >= SDL_EVENT_KEY_DOWN && e.type < SDL_EVENT_CLIPBOARD_UPDATE) ||
           (e.type >= SDL_EVENT_DROP_FILE && e.type < SDL_EVENT_AUDIO_DEVICE_ADDED) ||
           (e.type >= SDL_EVENT_PEN_PROXIMITY_IN && e.type < SDL_EVENT_CAMERA_DEVICE_ADDED);
}

bool fromScenario(const AppState& s, const SDL_Event& e) {
    if (e.type >= SDL_EVENT_JOYSTICK_AXIS_MOTION && e.type < SDL_EVENT_GAMEPAD_AXIS_MOTION)
        return s.virtualPadId != 0 && e.jdevice.which == s.virtualPadId;
    if (e.type >= SDL_EVENT_GAMEPAD_AXIS_MOTION && e.type < SDL_EVENT_FINGER_DOWN)
        return s.virtualPadId != 0 && e.gdevice.which == s.virtualPadId;
    return e.common.reserved == kScenarioEvent;
}

SDL_AppResult fail(std::string_view what) {
    log::error("app", "{}", what);
    return SDL_APP_FAILURE;
}

int scenarioExitCode(const AppState& s) { return s.scenario && s.scenario->failed() > 0 ? 1 : 0; }

void addGpuSample(PerfRegion& perf, double gpuMs, const std::vector<gpu::GpuTiming>& passes) {
    perf.gpuMs.push_back(gpuMs);
    double sum = 0.0, start = 1e30, end = 0.0;
    for (const gpu::GpuTiming& t : passes) {
        auto it = std::find_if(perf.passMs.begin(), perf.passMs.end(),
                               [&t](const auto& entry) { return entry.first == t.name; });
        if (it == perf.passMs.end()) it = perf.passMs.insert(it, {t.name, {}});
        it->second.push_back(t.milliseconds);
        if (t.depth > 0) continue;
        sum += t.milliseconds;
        start = std::min(start, t.startMilliseconds);
        end = std::max(end, t.endMilliseconds);
    }
    if (end > 0.0) {
        perf.passSumMs.push_back(sum);
        perf.passSpanMs.push_back(end - start);
        if (gpuMs > 0.0) perf.passShare.push_back((end - start) / gpuMs);
    }
}

// Reads the signalled fence s.fences[index] (its frame's GPU time into the stats, and into the perf region the frame
// was in) and releases it.
void readGpuTime(AppState& s, std::size_t index) {
    gpu::Fence& fence = s.fences[index];
    if (const auto ms = s.device->gpuMilliseconds(fence)) {
        s.stats.gpuMs = *ms;
        s.stats.gpuPasses = s.device->gpuTimings(fence);
        if (s.perf && s.fencePerf[index] == s.perf->serial) addGpuSample(*s.perf, *ms, s.stats.gpuPasses);
    }
    fence.reset();
}

void updateGpuTime(AppState& s) {
    for (std::size_t k = 0; k < s.fences.size(); ++k) {
        const std::size_t index = (s.nextFence + k) % s.fences.size(); // the oldest first
        if (s.fences[index] && s.device->isSignaled(s.fences[index])) readGpuTime(s, index);
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
    s.device->setPassTiming(s.options.gpuPassTiming || s.perf || s.passTimingRequested);
    s.passTimingRequested = false;
    gpu::CommandBuffer commands = s.device->acquireCommandBuffer();
    if (!commands) return false;
    ImDrawData* drawData = ImGui::GetDrawData();
    s.imguiRenderer->prepare(commands, drawData);

    // Offscreen no swapchain holds the frames back: at most as many in flight as there are fences.
    const Uint64 waitStart = SDL_GetPerformanceCounter();
    gpu::TextureRef swapchain;
    if (s.offscreen) {
        // Read here: this frame's fence takes its place, and a GPU-bound run would never time a frame otherwise.
        if (s.fences[s.nextFence]) {
            s.device->wait(s.fences[s.nextFence]);
            readGpuTime(s, s.nextFence);
        }
    } else {
        swapchain = commands.acquireSwapchainTexture(s.window.get());
    }
    waitMs = double(SDL_GetPerformanceCounter() - waitStart) * 1000.0 / double(SDL_GetPerformanceFrequency());
    if (!swapchain && !s.offscreen) {
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
    // submit. Without a window on the screen the frame texture is all there is.
    gpu::Texture captureTexture;
    gpu::TextureRef target = swapchain;
    const bool fixedSize = s.options.frameWidth > 0 || s.offscreen;
    if (fixedSize || !capturePath.empty()) {
        const std::uint32_t width = s.offscreen ? s.pixelWidth
                                    : fixedSize ? std::uint32_t(s.options.frameWidth)
                                                : swapchain.desc.width;
        const std::uint32_t height = s.offscreen ? s.pixelHeight
                                     : fixedSize ? std::uint32_t(s.options.frameHeight)
                                                 : swapchain.desc.height;
        gpu::Texture& texture = fixedSize ? s.frameTexture : captureTexture;
        if (texture && (texture.desc().width != width || texture.desc().height != height)) texture.reset();
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

    // HDR: the game renders into the composite's frame texture, which goes into the target with ImGui on top.
    gpu::TextureRef gameTarget = target;
    if (s.output.hdr()) {
        Result<gpu::TextureRef> frameTarget =
            s.uiComposite.frame(*s.device, s.swapchainFormat, target.desc.width, target.desc.height);
        if (!frameTarget) {
            log::error("app", "HDR frame: {}", frameTarget.error());
            return false;
        }
        gameTarget = *frameTarget;
    }
    Frame frame{commands, gameTarget, s.swapchainFormat, gameTarget.desc.width, gameTarget.desc.height, s.output};
    commands.beginTiming("game");
    s.game->render(s.app, frame);
    commands.endTiming();
    commands.beginTiming("ui");
    if (s.output.hdr()) {
        s.uiComposite.composite(commands, *s.imguiRenderer, drawData, target, s.output.paperWhite);
    } else {
        const gpu::ColorTarget color{.texture = target, .load = gpu::LoadOp::Load};
        gpu::RenderPass pass = commands.beginRenderPass(color);
        if (pass) s.imguiRenderer->draw(pass, drawData, s.swapchainFormat, target.desc.width, target.desc.height);
    }
    commands.endTiming();
    if (swapchain && target.native != swapchain.native) {
        const bool sameSize = target.desc.width == swapchain.desc.width && target.desc.height == swapchain.desc.height;
        commands.beginTiming("blit");
        commands.blit({.source = {.texture = target},
                       .destination = {.texture = swapchain},
                       .filter = sameSize ? gpu::Filter::Nearest : gpu::Filter::Linear});
        commands.endTiming();
    }

    submitCounter = SDL_GetPerformanceCounter();
    Result<gpu::Fence> fence = commands.submitWithFence();
    if (!fence) {
        log::error("app", "submit: {}", fence.error());
        return false;
    }
    s.fences[s.nextFence] = std::move(*fence);
    s.fencePerf[s.nextFence] = s.perf ? s.perf->serial : 0;
    if (s.perf && s.device->caps().gpuTiming) ++s.perf->timedFrames;
    s.nextFence = (s.nextFence + 1) % s.fences.size();

    if (!capturePath.empty()) {
        gpu::TextureRef source = target;
        Result<std::vector<std::byte>> pixels = s.device->download({.texture = source});
        Result<double> written = pixels ? writePng(capturePath, *pixels, source.desc.width, source.desc.height,
                                                   s.swapchainFormat, s.output.paperWhite)
                                        : Result<double>(Error{pixels.error()});
        if (written) {
            s.capturePeak = *written;
            if (s.output.hdr())
                log::info("app", "captured {} ({}x{}, frame {}, t={:.3f} s; HDR peak {:.2f} x paper white, clipped)",
                          capturePath, source.desc.width, source.desc.height, s.frame, s.time, *written);
            else
                log::info("app", "captured {} ({}x{}, frame {}, t={:.3f} s)", capturePath, source.desc.width,
                          source.desc.height, s.frame, s.time);
        } else {
            log::error("app", "capture {}: {}", capturePath, written.error());
            s.app.quit(1);
        }
    }
    return true;
}

void updateStats(AppState& s, double realDt, double cpuMs, double waitMs, int ticks) {
    s.statsWindowMs += realDt * 1000.0;
    ++s.statsWindowFrames;
    if (s.statsWindowMs >= 500.0) {
        s.stats.frameMs = s.statsWindowMs / s.statsWindowFrames;
        s.stats.fps = 1000.0 / s.stats.frameMs;
        s.statsWindowMs = 0.0;
        s.statsWindowFrames = 0;
        updateThermalState(s);
    }
    s.stats.cpuMs = cpuMs;
    s.stats.ticks = ticks;
    s.lastFramePerf = s.perf ? s.perf->serial : 0;
    if (s.perf) {
        s.perf->thermalMax = std::max(s.perf->thermalMax, s.stats.thermal);
        s.perf->reactionMax = std::max(s.perf->reactionMax, thermalReaction(s));
        s.perf->cpuMs.push_back(cpuMs);
        s.perf->waitMs.push_back(waitMs);
    }
}

// The mixer and its output: a device, or offline mixing on the frame clock (--no-audio, --audio-capture, or no
// device could be opened).
void startAudio(AppState& s) {
    s.audio = std::make_unique<audio::AudioSystem>(*s.jobs, s.assets.get(), s.desc.audio);
    // Only scenarios that measure the mix keep it: the copy costs the frame thread several microseconds.
    if (s.scenario &&
        std::ranges::any_of(s.scenario->commands(), [](const ScenarioCommand& c) { return c.op == "audio_measure"; }))
        s.audio->setMonitor(kAudioMonitorSeconds);
    // Offscreen frames are not paced, so the mix follows the frame clock there too unless a device is named.
    std::string offline = s.options.noAudio                       ? "--no-audio"
                          : !s.options.audioCapture.empty()       ? "--audio-capture"
                          : s.offscreen && !s.options.audioDevice ? "offscreen"
                                                                  : "";
    if (offline.empty()) {
        audio::DeviceDesc desc = s.desc.audioDevice;
        if (s.options.audioDevice) desc.name = *s.options.audioDevice;
        Result<std::unique_ptr<audio::Device>> device = audio::Device::open(*s.audio, desc);
        if (device) {
            s.audioDevice = std::move(*device);
        } else {
            log::warning("audio", "no audio output ({}): the mix runs offline", device.error());
            offline = "no device";
        }
    }
    if (!offline.empty()) {
        s.audio->setOffline(true);
        log::info("audio", "offline mixing on the frame clock ({}), {} Hz", offline, s.audio->sampleRate());
    }
}

void updateAudio(AppState& s, double frameDt) {
    s.audio->update();
    if (s.audioDevice)
        s.audioDevice->update();
    else
        s.audio->mixOffline(frameDt, s.options.audioCapture.empty() ? nullptr : &s.audioCapture);
}

void stopAudio(AppState& s) {
    s.audioDevice.reset();
    if (!s.options.audioCapture.empty() && s.audio) {
        const fs::Bytes wav = audio::encodeWav(s.audioCapture, audio::kMixChannels, s.audio->sampleRate());
        if (Result<void> written = writeCaptureFile(s.options.audioCapture, wav); written)
            log::info("audio", "captured {} ({:.3f} s, {} Hz stereo)", s.options.audioCapture,
                      double(s.audioCapture.size() / audio::kMixChannels) / s.audio->sampleRate(),
                      s.audio->sampleRate());
        else
            log::error("audio", "--audio-capture {}: {}", s.options.audioCapture, written.error());
    }
    s.audioCapture = {};
}

} // namespace

namespace {

void trackDisplayHeadroom(AppState& s) {
    if (s.offscreen) return;
    const std::optional<DisplayHeadroom> platform = queryDisplayHeadroom(s.window.get());
    if (!platform) return;
    const DisplayHeadroom headroom = resolveDisplayHeadroom(platform, 1.0f);
    if (headroom.current != s.displayHeadroom || headroom.potential != s.displayPotentialHeadroom) s.outputDirty = true;
}

void logDisplayHeadroom(AppState& s) {
    if (!s.headroomLog.due(s.displayHeadroom, double(SDL_GetTicksNS()) * 1e-9)) return;
    log::info("app", "display headroom {:.2f} (potential {:.2f}); output headroom {:.2f}", s.displayHeadroom,
              s.displayPotentialHeadroom, s.output.headroom);
}

} // namespace

void updateOutput(AppState& s) {
    s.outputDirty = false;
    SDL_Window* window = s.window.get();
    // SDL sets both when the window's display changes or reports another HDR state
    // (SDL_EVENT_WINDOW_HDR_STATE_CHANGED); SDR white is 1 on Apple platforms. Where the platform has a query, its
    // headroom replaces SDL's and is read every frame (trackDisplayHeadroom).
    // Offscreen there is no display: an SDR one (headroom 1, paper white 1), and HDR extended linear frames are
    // offscreen textures every backend renders.
    float sdrWhite = 1.0f;
    if (s.offscreen) {
        s.displayHeadroom = s.displayPotentialHeadroom = 1.0f;
        s.hdrSupported = true;
    } else {
        const SDL_PropertiesID props = SDL_GetWindowProperties(window);
        const DisplayHeadroom headroom = resolveDisplayHeadroom(
            queryDisplayHeadroom(window), SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f));
        s.displayHeadroom = headroom.current;
        s.displayPotentialHeadroom = headroom.potential;
        sdrWhite = std::max(1e-3f, SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f));
        s.hdrSupported = s.device->supportsComposition(window, gpu::SwapchainComposition::HdrExtendedLinear);
    }

    const bool wantHdr = s.hdrMode == HdrMode::On || (s.hdrMode == HdrMode::Auto && s.displayPotentialHeadroom > 1.0f);
    gpu::SwapchainComposition composition = gpu::SwapchainComposition::Sdr;
    if (wantHdr && !s.hdrSupported)
        log::warning("app", "HDR output ({}): {} cannot composite this window in HDR extended linear: SDR",
                     hdrModeName(s.hdrMode), s.device->caps().driver);
    else if (wantHdr)
        composition = gpu::SwapchainComposition::HdrExtendedLinear;
    if (s.offscreen && (composition != s.offscreenComposition || s.swapchainFormat == gpu::TextureFormat::Invalid)) {
        s.offscreenComposition = composition;
        s.swapchainFormat = offscreenFormat(composition);
        s.frameTexture.reset();
        s.uiComposite.reset();
    } else if (!s.offscreen && composition != s.device->composition(window)) {
        composition = s.device->setComposition(window, composition);
        s.swapchainFormat = s.device->swapchainFormat(window);
        s.frameTexture.reset();
        s.uiComposite.reset();
    }

    gpu::DisplayOutput output{.composition = composition};
    if (output.hdr()) {
        output.headroom = s.options.hdrHeadroom > 0.0f ? s.options.hdrHeadroom : s.displayHeadroom;
        output.paperWhite = sdrWhite;
    }
    // A headroom change alone gets the rate-limited line of logDisplayHeadroom().
    if (output.composition != s.output.composition || output.paperWhite != s.output.paperWhite || !s.gameStarted) {
        log::info("app",
                  "display output: {} ({}), headroom {:.2f}, paper white {:.2f}, {} {}; display "
                  "headroom {:.2f} (potential {:.2f}), HDR {}supported",
                  gpu::swapchainCompositionName(output.composition), hdrModeName(s.hdrMode), output.headroom,
                  output.paperWhite, s.offscreen ? "offscreen" : "swapchain", gpu::formatInfo(s.swapchainFormat).name,
                  s.displayHeadroom, s.displayPotentialHeadroom, s.hdrSupported ? "" : "not ");
        s.headroomLog.logged(s.displayHeadroom, double(SDL_GetTicksNS()) * 1e-9);
    }
    s.output = output;
}

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
    if (virtualGamepad) SDL_CloseGamepad(virtualGamepad);
    if (virtualPad) SDL_CloseJoystick(virtualPad);
    if (virtualPadId) SDL_DetachVirtualJoystick(virtualPadId);
}

SDL_AppResult init(AppState& s, int argc, char** argv) {
    std::vector<std::string_view> args(argv + (argc > 0 ? 1 : 0), argv + std::max(argc, 1));
    if (argc > 0 && argv[0]) s.program = argv[0];
    s.arguments.assign(args.begin(), args.end());
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

    // Runs nobody watches stay off the screen on desktops; on phones and tablets the screen is the app's.
    constexpr bool kDesktop = QUENCH_PLATFORM_MACOS || QUENCH_PLATFORM_WINDOWS || QUENCH_PLATFORM_LINUX;
    s.offscreen = s.options.offscreen.value_or(kDesktop && s.options.automated());
    s.scriptedInput = !s.options.script.empty() || s.offscreen;
    SDL_SetAppMetadata(s.desc.name.c_str(), s.desc.version.c_str(), nullptr);
    // macOS: SDL makes the process a regular app (a Dock icon) and activates it unless told otherwise.
    if (s.offscreen) SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
#if QUENCH_PLATFORM_MACOS
    if (s.offscreen) quench_app_prohibit_activation();
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail(std::format("SDL_Init: {}", SDL_GetError()));
    s.logSink.id = addSdlLogSink();
    log::info("app", "{} {} (Quench {}), SDL {}.{}.{}", s.desc.name, s.desc.version, QUENCH_VERSION,
              SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
              SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    s.thermalOverride = s.options.thermalState;
    s.stats.thermal = s.thermalOverride ? *s.thermalOverride : queryThermalState();
    log::info("app", "thermal state {}{}", thermalStateName(s.stats.thermal),
              s.thermalOverride ? " (override: --thermal-state)" : "");

    Result<std::unique_ptr<fs::Backend>> files = makeSdlFileSystem(
        {.org = s.desc.organization, .app = s.desc.name, .assetDir = s.desc.assetDir, .userDir = s.options.userDir});
    if (!files) return fail(std::format("file system: {}", files.error()));
#if QUENCH_ASSET_OVERLAY
    fs::setBackend(fs::makeOverlayBackend(std::move(*files), kAssetOverlayDir));
    log::info("app", "asset overlay: files in {}/ of the user directory replace bundled assets", kAssetOverlayDir);
#else
    fs::setBackend(std::move(*files));
#endif
    s.jobs = std::make_unique<JobSystem>(JobSystemDesc{.workerThreads = s.options.workerThreads});
    s.assets = std::make_unique<assets::AssetManager>(
        *s.jobs,
        assets::AssetManagerSettings{.simulatedLatency = s.options.assetLatency, .hotReload = s.options.hotReload});

    if (!s.options.script.empty()) {
        // A host path given on the command line (also in the iOS Simulator), not an asset. On a device the host's
        // files are out of reach: a relative path not found there is read from the user directory. Included files
        // are read the same way.
        const Scenario::LoadFn load = [](const std::string& path) -> Result<std::string> {
            size_t size = 0;
            if (void* data = SDL_LoadFile(path.c_str(), &size)) {
                std::string text(static_cast<const char*>(data), size);
                SDL_free(data);
                return text;
            }
            const std::string error = SDL_GetError();
            Result<std::string> user =
                std::filesystem::path(path).is_relative() ? fs::readUserText(path) : Result<std::string>(Error{error});
            if (!user) return Error{error};
            return user;
        };
        Result<std::string> text = load(s.options.script);
        if (!text) return fail(std::format("--script {}: {}", s.options.script, text.error()));
        Result<Scenario> parsed = Scenario::parse(*text, s.options.script, load);
        if (!parsed) return fail(parsed.error());
        s.scenario = std::move(*parsed);
        log::info("scenario", "{}: {} commands", s.options.script, s.scenario->commands().size());
    }
    startAudio(s);

    // Read when the window is created (phones and tablets; desktop windows ignore it).
    if (!s.options.orientation.empty()) SDL_SetHint(SDL_HINT_ORIENTATIONS, s.options.orientation.c_str());
    const int width = s.options.windowWidth > 0 ? s.options.windowWidth : s.desc.width;
    const int height = s.options.windowHeight > 0 ? s.options.windowHeight : s.desc.height;
    // Offscreen the window stays hidden: ImGui and the game still get its size, events and safe area.
    SDL_WindowFlags windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (s.offscreen) windowFlags |= SDL_WINDOW_HIDDEN;
    s.window.reset(SDL_CreateWindow(s.desc.name.c_str(), width, height, windowFlags));
    if (!s.window) return fail(std::format("SDL_CreateWindow: {}", SDL_GetError()));
    updateWindowSize(s);

    Result<gpu::Device> device = gpu::Device::create({.driver = s.options.gpuDriver,
                                                      .debug = s.options.gpuDebug,
                                                      .preferLowPower = false,
                                                      .backend = s.options.gpuBackend.value_or(s.desc.gpuBackend)});
    if (!device) return fail(std::format("GPU device: {}", device.error()));
    s.device.emplace(std::move(*device));
    if (s.options.hotReload) {
        if (Result<void> shaders = s.device->enableShaderHotReload(); !shaders)
            log::info("app", "no shader hot reload: {}", shaders.error());
    }
    if (!s.offscreen) {
        const gpu::PresentMode presentMode = s.options.presentMode.value_or(s.desc.presentMode);
        if (Result<void> claimed = s.device->claimWindow(s.window.get(), presentMode); !claimed)
            return fail(std::format("GPU window: {}", claimed.error()));
        s.swapchainFormat = s.device->swapchainFormat(s.window.get());
    }
    s.hdrMode = s.options.hdr.value_or(s.desc.hdr);
    updateOutput(s);
    const gpu::Caps& caps = s.device->caps();
    log::info("app", "GPU: {} backend, {} ({}), {} {}", gpu::backendName(caps.backend), caps.driver,
              caps.deviceName.empty() ? "?" : caps.deviceName, s.offscreen ? "offscreen frames" : "swapchain",
              gpu::formatInfo(s.swapchainFormat).name);
    if (s.offscreen)
        log::info("app", "offscreen: no window on the screen, frames {}x{}, no input from real devices", s.pixelWidth,
                  s.pixelHeight);
    else if (s.scriptedInput)
        log::info("app", "input from the script only");

    IMGUI_CHECKVERSION();
    s.imgui.context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window layouts are not saved (user files go through quench::fs)
    io.LogFilename = nullptr;
    const float scale = uiScale(s);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    if (!ImGui_ImplSDL3_InitForOther(s.window.get())) return fail("ImGui SDL3 backend");
    s.imgui.platform = true;
    // ImGui opens and polls gamepads itself; scripted runs give it only the scenario's virtual one (AppScenario.cpp).
    if (s.scriptedInput) {
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
        s.input.restrictGamepad(0);
    }
    Result<ImGuiRenderer> renderer = ImGuiRenderer::create(*s.device);
    if (!renderer) return fail(std::format("ImGui renderer: {}", renderer.error()));
    s.imguiRenderer.emplace(std::move(*renderer));

    updateWindowSize(s);
    s.clock.setStep(s.desc.fixedStep);
    s.input.beginEvents();
    if (Result<void> started = s.game->init(s.app); !started)
        return fail(std::format("game init: {}", started.error()));
    s.gameStarted = true;
    if (s.options.maxFrameRate) s.app.setMaxFrameRate(*s.options.maxFrameRate);
    s.lastCounter = SDL_GetPerformanceCounter();
    return SDL_APP_CONTINUE;
}

SDL_AppResult iterate(AppState& s) {
    const Uint64 now = SDL_GetPerformanceCounter();
    const double frequency = double(SDL_GetPerformanceFrequency());
    const double realDt = double(now - s.lastCounter) / frequency;
    // Under a frame rate limit a refresh that comes early passes without a frame (iOS calls on every refresh of its
    // display link: App::setMaxFrameRate). Three quarters of the interval, so that a refresh a little early by jitter
    // still renders: 60 Hz on a 120 Hz display renders every second refresh.
    if (s.frameRateLimit > 0.0 && !s.offscreen && realDt < 0.75 / s.frameRateLimit) return SDL_APP_CONTINUE;
    s.lastCounter = now;
    // The last frame's time, counted in its own region: the time before a region's first frame (a capture's encoding
    // just before perf_begin, for example) stays out of it, and its last frame is in.
    if (s.perf && s.lastFramePerf == s.perf->serial) s.perf->frameMs.push_back(realDt * 1000.0);
    s.lastFramePerf = 0;

    const bool minimized =
        !s.offscreen && (SDL_GetWindowFlags(s.window.get()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
    if (s.background || minimized) {
        // No GPU work in the background (iOS forbids it); the clock does not advance meanwhile. A device that still
        // plays (a minimized window) keeps its streams fed.
        s.input.endEvents(false, false);
        s.input.beginEvents();
        if (s.audioDevice && s.audioDevice->running()) updateAudio(s, 0.0);
        SDL_Delay(16);
        s.lastCounter = SDL_GetPerformanceCounter();
        return SDL_APP_CONTINUE;
    }

    updateWindowSize(s);
    if (!s.outputDirty) trackDisplayHeadroom(s);
    if (s.outputDirty) updateOutput(s);
    logDisplayHeadroom(s);
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
    s.thermalGovernor.update(s.stats.thermal, s.time);
    applyFrameRateLimit(s);
    s.assets->update();
    s.device->pollShaderHotReload();
    s.game->update(s.app, frameDt);
    updateAudio(s, frameDt);

    ImGui_ImplSDL3_NewFrame();
    ImGuiIO& imguiIo = ImGui::GetIO();
    imguiIo.DeltaTime = float(std::max(frameDt, 1e-4));
    if (s.offscreen && imguiIo.DisplaySize.x > 0.0f && imguiIo.DisplaySize.y > 0.0f)
        imguiIo.DisplayFramebufferScale =
            ImVec2(float(s.pixelWidth) / imguiIo.DisplaySize.x, float(s.pixelHeight) / imguiIo.DisplaySize.y);
    else if (s.options.frameWidth > 0 && imguiIo.DisplaySize.x > 0.0f && imguiIo.DisplaySize.y > 0.0f)
        imguiIo.DisplayFramebufferScale = ImVec2(float(s.options.frameWidth) / imguiIo.DisplaySize.x,
                                                 float(s.options.frameHeight) / imguiIo.DisplaySize.y);
    ImGui::NewFrame();
    s.game->ui(s.app);
    ImGui::Render();

    double waitMs = 0.0;
    Uint64 submitCounter = now;
    if (!render(s, waitMs, submitCounter)) return SDL_APP_FAILURE;

    const double cpuMs = double(submitCounter - now) * 1000.0 / frequency - waitMs;
    updateStats(s, realDt, cpuMs, waitMs, ticks);
    // Deterministic frames are held to real time on the screen. Offscreen they run as fast as they render, except
    // while loads are in flight or delayed by a simulated latency: loads take real time, and scripts wait for them
    // in app time.
    if (fixedFrame && (!s.offscreen || s.assets->settings().simulatedLatency > 0.0 || !s.assets->idle())) {
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
        if (s.audioDevice) s.audioDevice->suspend();
        log::info("app", "entering background");
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        if (!s.background) break; // also sent at startup on iOS
        s.background = false;
        s.clock.reset();
        s.lastCounter = SDL_GetPerformanceCounter();
        if (s.audioDevice) s.audioDevice->resume();
        log::info("app", "back in foreground");
        break;
    case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: s.outputDirty = true; break;
    default: break;
    }
    if (s.scriptedInput && deviceInput(e) && !fromScenario(s, e)) return SDL_APP_CONTINUE;
    if (s.imgui.platform) ImGui_ImplSDL3_ProcessEvent(&e);
    s.input.handleEvent(e, float(std::max(1, s.windowWidth)), float(std::max(1, s.windowHeight)));
    if (s.gameStarted) s.game->event(s.app, e);
    return SDL_APP_CONTINUE;
}

void quit(AppState& s, SDL_AppResult result) {
    if (s.device) s.device->waitIdle();
    // The device thread stops before the game goes: its generators may read game state.
    stopAudio(s);
    if (s.gameStarted) s.game->shutdown(s.app);
    s.gameStarted = false;
    s.game.reset();
    s.audio.reset();
    // Loads and jobs a game left behind may still read files: they finish before the file system backend goes.
    s.assets.reset();
    s.jobs.reset();
    releaseScenarioInput(s);
    for (gpu::Fence& fence : s.fences) fence.reset();
    s.frameTexture.reset();
    s.uiComposite.reset();
    if (s.imguiRenderer) s.imguiRenderer->shutdown();
    s.imguiRenderer.reset();
    if (s.imgui.platform) ImGui_ImplSDL3_Shutdown();
    s.imgui.platform = false;
    if (s.imgui.context) ImGui::DestroyContext(s.imgui.context);
    s.imgui.context = nullptr;
    if (s.device) {
        if (const std::int64_t live = s.device->liveResources(); live != 0)
            log::warning("app", "{} GPU resources still alive at exit", live);
        if (!s.offscreen) s.device->releaseWindow(s.window.get());
    }
    s.device.reset();
    s.window.reset();
    fs::setBackend(nullptr);
    if (result == SDL_APP_FAILURE) log::error("app", "exit with failure");
}

} // namespace detail
} // namespace quench::app
