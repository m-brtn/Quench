// The app's scenario commands and values (include/quench/app/Scenario.h). Input commands push SDL events, so the
// engine, ImGui and the game see them exactly like real input, one frame later.

#include "AppState.h"
#include "ScenarioPoint.h"
#include "ThermalState.h"

#include <quench/audio/Analysis.h>
#include <quench/core/Log.h>
#include <quench/core/Parse.h>

#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace quench::app::detail {
namespace {

constexpr const char* kPointHelp = "window points, percent of the window like 50% 25%, or a point the game names";

std::optional<double> number(const std::vector<std::string>& args, std::size_t index) {
    if (index >= args.size()) return std::nullopt;
    return parseDouble(args[index]);
}

// The point a pointer command starts with; logs an unknown @NAME itself.
std::optional<ScenarioPoint> pointArgument(AppState& s, const ScenarioCommand& c) {
    const glm::vec2 window(float(std::max(1, s.windowWidth)), float(std::max(1, s.windowHeight)));
    std::optional<ScenarioPoint> point =
        parseScenarioPoint(c.args, 0, window, [&s](std::string_view name) { return s.game->scenarioPoint(name); });
    if (!point && !c.args.empty() && c.args[0].starts_with('@'))
        log::error("scenario", "line {}: the game has no point named '{}' now (Game::scenarioPoint)", c.line,
                   c.args[0].substr(1));
    return point;
}

void push(AppState& s, SDL_Event& e) {
    e.common.reserved = kScenarioEvent;
    SDL_PushEvent(&e);
    s.inputPushedFrame = s.frame;
}

Scenario::Exec pushKey(AppState& s, SDL_Scancode scancode, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(s.window.get());
    e.key.scancode = scancode;
    e.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
    e.key.down = down;
    push(s, e);
    return Scenario::Exec::Done;
}

void pushMouseMotion(AppState& s, float x, float y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = SDL_GetWindowID(s.window.get());
    const glm::vec2 previous = s.input.mousePosition();
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = x - previous.x;
    e.motion.yrel = y - previous.y;
    push(s, e);
}

void pushMouseButton(AppState& s, Uint8 button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(s.window.get());
    e.button.button = button;
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = s.input.mousePosition().x;
    e.button.y = s.input.mousePosition().y;
    push(s, e);
}

// Like SDL_SendTouch for the first finger of a touch device (SDL_HINT_TOUCH_MOUSE_EVENTS, on by default): a mouse
// event with SDL_TOUCH_MOUSEID before the finger event, so ImGui and Input::mouseFromTouch() see a scripted touch
// as they see one on a phone.
void pushMouseFromTouch(AppState& s, Uint32 type, float x, float y) {
    if (!SDL_GetHintBoolean(SDL_HINT_TOUCH_MOUSE_EVENTS, true)) return;
    const float w = float(std::max(1, s.windowWidth)), h = float(std::max(1, s.windowHeight));
    const float px = std::clamp(x * w, 0.0f, w - 1.0f), py = std::clamp(y * h, 0.0f, h - 1.0f);
    SDL_Event e{};
    if (type != SDL_EVENT_FINGER_UP) {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = SDL_GetWindowID(s.window.get());
        e.motion.which = SDL_TOUCH_MOUSEID;
        e.motion.state = type == SDL_EVENT_FINGER_MOTION ? SDL_BUTTON_LMASK : 0;
        e.motion.x = px;
        e.motion.y = py;
        e.motion.xrel = px - s.touchMouse.x;
        e.motion.yrel = py - s.touchMouse.y;
        push(s, e);
        s.touchMouse = {px, py};
    }
    if (type == SDL_EVENT_FINGER_MOTION) return;
    e = {};
    e.type = type == SDL_EVENT_FINGER_DOWN ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = SDL_GetWindowID(s.window.get());
    e.button.which = SDL_TOUCH_MOUSEID;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = type == SDL_EVENT_FINGER_DOWN;
    e.button.clicks = 1;
    e.button.x = s.touchMouse.x;
    e.button.y = s.touchMouse.y;
    push(s, e);
}

void pushFinger(AppState& s, Uint32 type, float x, float y, float dx, float dy) {
    pushMouseFromTouch(s, type, x, y);
    SDL_Event e{};
    e.type = type;
    e.tfinger.touchID = 1;
    e.tfinger.fingerID = 1;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.dx = dx;
    e.tfinger.dy = dy;
    e.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0f : 1.0f;
    e.tfinger.windowID = SDL_GetWindowID(s.window.get());
    push(s, e);
}

// Sends what a multi-frame command still owes; Busy while frames remain.
Scenario::Exec continuePending(AppState& s) {
    PendingInput& p = s.pendingInput;
    switch (p.kind) {
    case PendingInput::Kind::None: return Scenario::Exec::Done;
    case PendingInput::Kind::KeyUp: pushKey(s, p.scancode, false); break;
    case PendingInput::Kind::MouseUp: pushMouseButton(s, Uint8(p.button), false); break;
    case PendingInput::Kind::GamepadUp:
        if (s.virtualPad) SDL_SetJoystickVirtualButton(s.virtualPad, p.button, false);
        s.inputPushedFrame = s.frame;
        break;
    case PendingInput::Kind::TouchMove:
        if (p.steps > 0) {
            p.x += p.dx;
            p.y += p.dy;
            pushFinger(s, SDL_EVENT_FINGER_MOTION, p.x, p.y, p.dx, p.dy);
            --p.steps;
            return Scenario::Exec::Busy;
        }
        pushFinger(s, SDL_EVENT_FINGER_UP, p.x, p.y, 0.0f, 0.0f);
        break;
    }
    p = {};
    return Scenario::Exec::Done;
}

Scenario::Exec gamepadCommand(AppState& s, const ScenarioCommand& c) {
    const std::string& what = c.args.empty() ? std::string() : c.args[0];
    if (what == "attach") {
        if (s.virtualPadId) return Scenario::Exec::Done;
        // Scenario runs often have no keyboard focus; SDL drops gamepad input of unfocused apps by default.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Quench scenario gamepad";
        s.virtualPadId = SDL_AttachVirtualJoystick(&desc);
        s.virtualPad = s.virtualPadId ? SDL_OpenJoystick(s.virtualPadId) : nullptr;
        if (!s.virtualPad) {
            log::error("scenario", "virtual gamepad: {}", SDL_GetError());
            return Scenario::Exec::Failed;
        }
        // SDL takes the first report of an axis as its rest position; triggers rest at the bottom of the range.
        for (int axis = SDL_GAMEPAD_AXIS_LEFTX; axis <= SDL_GAMEPAD_AXIS_RIGHTY; ++axis)
            SDL_SetJoystickVirtualAxis(s.virtualPad, axis, 0);
        SDL_SetJoystickVirtualAxis(s.virtualPad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        SDL_SetJoystickVirtualAxis(s.virtualPad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        // The virtual pad is the only gamepad Input and ImGui use, whatever else is connected.
        s.input.restrictGamepad(s.virtualPadId);
        s.virtualGamepad = SDL_OpenGamepad(s.virtualPadId);
        if (s.virtualGamepad && s.imgui.platform)
            ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, &s.virtualGamepad, 1);
        s.inputPushedFrame = s.frame;
        return Scenario::Exec::Done;
    }
    if (what == "detach") {
        releaseScenarioInput(s);
        s.inputPushedFrame = s.frame;
        return Scenario::Exec::Done;
    }
    if (!s.virtualPad) {
        log::error("scenario", "line {}: `gamepad attach` first", c.line);
        return Scenario::Exec::Failed;
    }
    if (what == "button" && c.args.size() == 3) {
        const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(c.args[1].c_str());
        const std::string& action = c.args[2];
        if (button == SDL_GAMEPAD_BUTTON_INVALID || (action != "down" && action != "up" && action != "tap")) {
            log::error("scenario",
                       "line {}: gamepad button NAME down|up|tap (SDL names: a, b, x, y, start, "
                       "leftshoulder, dpup, ...)",
                       c.line);
            return Scenario::Exec::Failed;
        }
        SDL_SetJoystickVirtualButton(s.virtualPad, button, action != "up");
        s.inputPushedFrame = s.frame;
        if (action != "tap") return Scenario::Exec::Done;
        s.pendingInput = {.kind = PendingInput::Kind::GamepadUp, .button = int(button)};
        return Scenario::Exec::Busy;
    }
    if (what == "axis" && c.args.size() == 3) {
        const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(c.args[1].c_str());
        const auto value = number(c.args, 2);
        if (axis == SDL_GAMEPAD_AXIS_INVALID || !value) {
            log::error("scenario",
                       "line {}: gamepad axis NAME VALUE (leftx, lefty, rightx, righty: -1..1; "
                       "lefttrigger, righttrigger: 0..1)",
                       c.line);
            return Scenario::Exec::Failed;
        }
        const bool trigger = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        const double v = std::clamp(*value, trigger ? 0.0 : -1.0, 1.0);
        // A trigger spans the whole joystick range: minimum released, maximum fully pressed.
        const double raw = trigger ? SDL_JOYSTICK_AXIS_MIN + (SDL_JOYSTICK_AXIS_MAX - SDL_JOYSTICK_AXIS_MIN) * v
                                   : SDL_JOYSTICK_AXIS_MAX * v;
        SDL_SetJoystickVirtualAxis(s.virtualPad, axis, Sint16(std::lround(raw)));
        s.inputPushedFrame = s.frame;
        return Scenario::Exec::Done;
    }
    log::error("scenario", "line {}: gamepad attach|detach|button NAME down|up|tap|axis NAME VALUE", c.line);
    return Scenario::Exec::Failed;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return -1.0;
    std::sort(values.begin(), values.end());
    const double index = p * double(values.size() - 1);
    const std::size_t lo = std::size_t(std::floor(index));
    const std::size_t hi = std::min(values.size() - 1, lo + 1);
    return values[lo] + (values[hi] - values[lo]) * (index - double(lo));
}

double average(const std::vector<double>& values) {
    if (values.empty()) return -1.0;
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum / double(values.size());
}

} // namespace

void startPerf(AppState& s, std::string label) {
    s.perf = PerfRegion{.label = std::move(label),
                        .serial = ++s.perfSerial,
                        .thermalStart = s.stats.thermal,
                        .thermalMax = s.stats.thermal,
                        .reactionStart = thermalReaction(s),
                        .reactionMax = thermalReaction(s),
                        .processCpuStart = processCpuTime()};
}

PerfReport finishPerf(AppState& s) {
    const PerfRegion& r = *s.perf;
    const ThermalState thermalEnd = s.stats.thermal;
    const ThermalState thermalMax = std::max(r.thermalMax, thermalEnd);
    auto& v = s.perfValues;
    v.clear();
    v["perf_frames"] = double(r.frameMs.size());
    v["perf_frame_avg"] = average(r.frameMs);
    v["perf_frame_p10"] = percentile(r.frameMs, 0.10);
    v["perf_frame_p50"] = percentile(r.frameMs, 0.50);
    v["perf_frame_p95"] = percentile(r.frameMs, 0.95);
    v["perf_frame_p99"] = percentile(r.frameMs, 0.99);
    v["perf_frame_max"] = r.frameMs.empty() ? -1.0 : *std::max_element(r.frameMs.begin(), r.frameMs.end());
    v["perf_cpu_avg"] = average(r.cpuMs);
    v["perf_cpu_p50"] = percentile(r.cpuMs, 0.50);
    v["perf_cpu_p95"] = percentile(r.cpuMs, 0.95);
    v["perf_cpu_p99"] = percentile(r.cpuMs, 0.99);
    v["perf_wait_p50"] = percentile(r.waitMs, 0.50);
    v["perf_wait_p95"] = percentile(r.waitMs, 0.95);
    // Whether presents can be paced: the mode in use (vsync where the window does not support the one asked for)
    // and the display's rate.
    const gpu::PresentMode presentMode = s.device->presentMode(s.window.get());
    const SDL_DisplayMode* display = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s.window.get()));
    v["perf_present_mode"] = s.offscreen ? -1.0 : double(int(presentMode));
    v["perf_display_hz"] = display ? double(display->refresh_rate) : 0.0;
    const char* presentName = s.offscreen                                  ? "offscreen"
                              : presentMode == gpu::PresentMode::Immediate ? "immediate"
                              : presentMode == gpu::PresentMode::Mailbox   ? "mailbox"
                                                                           : "vsync";
    v["perf_gpu_avg"] = average(r.gpuMs);
    // The fastest GPU frames are the least disturbed by other work on the machine.
    v["perf_gpu_min"] = r.gpuMs.empty() ? -1.0 : *std::min_element(r.gpuMs.begin(), r.gpuMs.end());
    v["perf_gpu_p10"] = percentile(r.gpuMs, 0.10);
    v["perf_gpu_p50"] = percentile(r.gpuMs, 0.50);
    v["perf_gpu_p95"] = percentile(r.gpuMs, 0.95);
    // Frames still in flight at the end are not timed; any other timed frame without a sample is lost.
    std::size_t inFlight = 0;
    for (std::size_t i = 0; i < s.fences.size(); ++i)
        if (s.fences[i] && s.fencePerf[i] == r.serial) ++inFlight;
    v["perf_gpu_samples"] = double(r.gpuMs.size());
    v["perf_gpu_lost"] = r.timedFrames == 0 ? 0.0 : double(r.timedFrames) - double(r.gpuMs.size() + inFlight);
    // The process's CPU time per frame and the share of it on performance cores: the frame's CPU times depend on
    // where the scheduler ran the threads (on the M4 Pro about 4 times longer on efficiency cores at the same work).
    const std::optional<ProcessCpuTime> processEnd = r.processCpuStart ? processCpuTime() : std::nullopt;
    const double processMs = processEnd ? processEnd->totalMs - r.processCpuStart->totalMs : 0.0;
    v["perf_process_cpu"] = processEnd && !r.cpuMs.empty() ? processMs / double(r.cpuMs.size()) : -1.0;
    v["perf_cpu_performance_share"] =
        processEnd && processMs > 0.0
            ? 100.0 * (processEnd->performanceMs - r.processCpuStart->performanceMs) / processMs
            : -1.0;
    v["perf_thermal_start"] = double(r.thermalStart);
    v["perf_thermal_end"] = double(thermalEnd);
    v["perf_thermal_max"] = double(thermalMax);
    // "thermal fair", or "thermal nominal -> serious (max critical)" where it changed.
    std::string thermal = thermalStateName(r.thermalStart);
    if (thermalEnd != r.thermalStart) thermal += std::format(" -> {}", thermalStateName(thermalEnd));
    if (thermalMax > r.thermalStart && thermalMax > thermalEnd)
        thermal += std::format(" (max {})", thermalStateName(thermalMax));
    // The reaction's levels the same way, where it was on: "; thermal reaction serious (at most 60 Hz, ...)".
    const ThermalState reactionEnd = thermalReaction(s);
    const ThermalState reactionMax = std::max(r.reactionMax, reactionEnd);
    v["perf_thermal_reaction_start"] = double(r.reactionStart);
    v["perf_thermal_reaction_end"] = double(reactionEnd);
    v["perf_thermal_reaction_max"] = double(reactionMax);
    if (reactionMax != ThermalState::Unknown) {
        thermal += std::format("; thermal reaction {}", thermalStateName(r.reactionStart));
        if (reactionEnd != r.reactionStart) thermal += std::format(" -> {}", thermalStateName(reactionEnd));
        if (reactionMax > r.reactionStart && reactionMax > reactionEnd)
            thermal += std::format(" (max {})", thermalStateName(reactionMax));
        thermal += std::format(" ({} now)", thermalLimitsText(s.thermalGovernor.limits()));
    }
    std::string details = s.game->perfDetails(s.app);
    const std::string process = v["perf_process_cpu"] < 0.0
                                    ? std::string()
                                    : std::format(", process {:.3f} per frame ({:.0f}% on performance cores)",
                                                  v["perf_process_cpu"], v["perf_cpu_performance_share"]);
    log::info(
        "perf",
        "{}: {} frames; frame ms avg {:.3f} p10 {:.3f} p50 {:.3f} p95 {:.3f} p99 {:.3f} max {:.3f}; cpu ms avg "
        "{:.3f} p50 {:.3f} p95 {:.3f} p99 {:.3f}{}; wait ms p50 {:.3f} p95 {:.3f} ({}, display {:.0f} Hz); gpu ms avg "
        "{:.3f} min {:.3f} p10 {:.3f} p50 {:.3f} p95 {:.3f} ({} samples); thermal {}{}{}",
        r.label, r.frameMs.size(), v["perf_frame_avg"], v["perf_frame_p10"], v["perf_frame_p50"], v["perf_frame_p95"],
        v["perf_frame_p99"], v["perf_frame_max"], v["perf_cpu_avg"], v["perf_cpu_p50"], v["perf_cpu_p95"],
        v["perf_cpu_p99"], process, v["perf_wait_p50"], v["perf_wait_p95"], presentName, v["perf_display_hz"],
        v["perf_gpu_avg"], v["perf_gpu_min"], v["perf_gpu_p10"], v["perf_gpu_p50"], v["perf_gpu_p95"], r.gpuMs.size(),
        thermal, details.empty() ? "" : "; ", details);
    PerfReport report{.label = r.label,
                      .frames = r.frameMs.size(),
                      .frameAvg = v["perf_frame_avg"],
                      .frameP50 = v["perf_frame_p50"],
                      .frameP99 = v["perf_frame_p99"],
                      .frameMax = v["perf_frame_max"],
                      .cpuAvg = v["perf_cpu_avg"],
                      .cpuP50 = v["perf_cpu_p50"],
                      .cpuP99 = v["perf_cpu_p99"],
                      .gpuAvg = v["perf_gpu_avg"],
                      .gpuP50 = v["perf_gpu_p50"],
                      .gpuSamples = r.gpuMs.size(),
                      .passP50 = {},
                      .details = std::move(details),
                      .thermalStart = r.thermalStart,
                      .thermalEnd = thermalEnd,
                      .thermalMax = thermalMax,
                      .reactionStart = r.reactionStart,
                      .reactionEnd = reactionEnd,
                      .reactionMax = reactionMax,
                      .processCpu = v["perf_process_cpu"],
                      .cpuPerformanceShare = v["perf_cpu_performance_share"]};
    if (!r.passMs.empty()) {
        std::string passes;
        for (const auto& [name, ms] : r.passMs) {
            const double p50 = percentile(ms, 0.50);
            v["perf_pass_" + name + "_p50"] = p50;
            report.passP50.emplace_back(name, p50);
            passes += std::format("{}{} {:.3f}", passes.empty() ? "" : ", ", name, p50);
        }
        v["perf_pass_sum_p50"] = percentile(r.passSumMs, 0.50);
        v["perf_pass_span_p50"] = percentile(r.passSpanMs, 0.50);
        v["perf_pass_share_p50"] = percentile(r.passShare, 0.50);
        log::info("perf",
                  "{}: GPU busy ms p50 per timed range: {}; top-level ranges: sum {:.3f}, span {:.3f} = {:.1f}% of "
                  "the frame's GPU time",
                  r.label, passes, v["perf_pass_sum_p50"], v["perf_pass_span_p50"], v["perf_pass_share_p50"] * 100.0);
    }
    s.perf.reset();
    return report;
}

namespace {

// audio_measure SECONDS [MIN_HZ MAX_HZ]: levels and the strongest frequency of the newest SECONDS of the mix.
Scenario::Exec audioMeasure(AppState& s, const ScenarioCommand& c) {
    const auto seconds = number(c.args, 0);
    const auto minHz = c.args.size() > 1 ? number(c.args, 1) : std::optional<double>(20.0);
    const auto maxHz = c.args.size() > 2 ? number(c.args, 2) : std::optional<double>(20000.0);
    if (!seconds || *seconds <= 0.0 || *seconds > kAudioMonitorSeconds || !minHz || !maxHz || *minHz >= *maxHz ||
        c.args.size() > 3) {
        log::error("scenario", "line {}: audio_measure SECONDS [MIN_HZ MAX_HZ] (SECONDS up to {})", c.line,
                   kAudioMonitorSeconds);
        return Scenario::Exec::Failed;
    }
    const std::uint32_t rate = s.audio->sampleRate();
    const std::vector<float> mix = s.audio->monitored(*seconds);
    const audio::Levels levels = audio::measureLevels(mix, audio::kMixChannels);
    const audio::Peak peak = audio::dominantFrequency(mix, audio::kMixChannels, rate, float(*minHz), float(*maxHz));
    auto& v = s.audioValues;
    v["audio_rms"] = levels.rmsMono();
    v["audio_rms_l"] = levels.rms[0];
    v["audio_rms_r"] = levels.rms[1];
    v["audio_peak"] = std::max(levels.peak[0], levels.peak[1]);
    v["audio_freq"] = peak.frequency;
    v["audio_freq_level"] = peak.level;
    v["audio_measured"] = double(levels.frames) / rate;
    log::info("scenario",
              "audio_measure {:.3f} s ({:.3f} s mixed): rms {:.4f} (L {:.4f} R {:.4f}), peak {:.4f}; strongest "
              "{:.1f} Hz at {:.4f} ({:.0f} .. {:.0f} Hz)",
              *seconds, v["audio_measured"], v["audio_rms"], levels.rms[0], levels.rms[1], v["audio_peak"],
              peak.frequency, peak.level, *minHz, *maxHz);
    return Scenario::Exec::Done;
}

} // namespace

void releaseScenarioInput(AppState& s) {
    if (s.virtualGamepad) {
        if (s.imgui.platform) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
        SDL_CloseGamepad(s.virtualGamepad);
        s.virtualGamepad = nullptr;
    }
    if (s.virtualPadId) s.input.restrictGamepad(0);
    if (s.virtualPad) SDL_CloseJoystick(s.virtualPad);
    if (s.virtualPadId) SDL_DetachVirtualJoystick(s.virtualPadId);
    s.virtualPad = nullptr;
    s.virtualPadId = 0;
}

std::optional<double> scenarioValue(AppState& s, std::string_view name) {
    if (name == "time") return s.time;
    if (name == "frame") return double(s.frame);
    if (name == "ticks") return double(s.ticks);
    if (name == "fps") return s.stats.fps;
    if (name == "frame_ms") return s.stats.frameMs;
    if (name == "cpu_ms") return s.stats.cpuMs;
    if (name == "gpu_ms") return s.stats.gpuMs;
    if (name == "window_w") return double(s.windowWidth);
    if (name == "window_h") return double(s.windowHeight);
    if (name == "pixel_w") return double(s.pixelWidth);
    if (name == "pixel_h") return double(s.pixelHeight);
    if (name == "gamepad") return s.input.gamepadConnected() ? 1.0 : 0.0;
    if (name == "fingers") return double(s.input.fingers().size());
    if (name == "hdr") return s.output.hdr() ? 1.0 : 0.0;
    if (name == "hdr_supported") return s.hdrSupported ? 1.0 : 0.0;
    if (name == "hdr_headroom") return double(s.output.headroom);
    if (name == "display_headroom") return double(s.displayHeadroom);
    if (name == "display_headroom_potential") return double(s.displayPotentialHeadroom);
    if (name == "capture_peak") return s.capturePeak;
    if (name == "memory_mb") {
        const std::optional<std::uint64_t> bytes = s.app.memoryFootprint();
        return bytes ? double(*bytes) / double(1u << 20) : -1.0;
    }
    if (name == "gpu_resources") return double(s.device->liveResources());
    if (name.starts_with("shader_")) {
        const gpu::ShaderReloadStatus reload = s.device->shaderReloadStatus();
        if (name == "shader_reloads") return double(reload.reloads);
        if (name == "shader_reload_failures") return double(reload.failures);
        if (name == "shader_reload_pipelines") return double(reload.pipelines);
    }
    if (name == "thermal_state") return double(s.stats.thermal);
    if (name == "max_frame_rate") return s.maxFrameRate;
    if (name == "frame_rate_limit") return s.frameRateLimit;
    if (name.starts_with("thermal_")) {
        const ThermalGovernor& governor = s.thermalGovernor;
        if (name == "thermal_reaction") return double(thermalReaction(s));
        if (name == "thermal_frame_rate_cap") return governor.limits().maxFrameRate;
        if (name == "thermal_render_scale") return double(governor.limits().renderScaleFactor);
        if (name == "thermal_reduce_effects") return governor.reduceEffects() ? 1.0 : 0.0;
        if (name == "thermal_reaction_changes") return double(governor.changes());
    }
    if (const auto it = s.perfValues.find(name); it != s.perfValues.end()) return it->second;
    if (name.starts_with("audio_")) {
        if (const auto it = s.audioValues.find(name); it != s.audioValues.end()) return it->second;
        const audio::AudioStats stats = s.audio->stats();
        if (name == "audio_voices") return double(stats.voices);
        if (name == "audio_hrtf_voices") return double(stats.hrtfVoices);
        if (name == "audio_sounds") return double(stats.sounds);
        if (name == "audio_pending") return double(stats.pending);
        if (name == "audio_streams") return double(stats.streams);
        if (name == "audio_played") return double(stats.played);
        if (name == "audio_stolen") return double(stats.stolen);
        if (name == "audio_rejected") return double(stats.rejected);
        if (name == "audio_failed") return double(stats.failed);
        if (name == "audio_underruns") return double(stats.underruns);
        if (name == "audio_clipped") return double(stats.clipped);
        if (name == "audio_device") return s.audioDevice ? 1.0 : 0.0;
    }
    if (name.starts_with("action:")) {
        const auto action = s.input.findAction(name.substr(7));
        if (action) return double(s.input.value(*action));
        return std::nullopt;
    }
    return s.game->scenarioValue(name);
}

Scenario::Exec scenarioCommand(AppState& s, const ScenarioCommand& c) {
    using Exec = Scenario::Exec;
    if (s.pendingInput.kind != PendingInput::Kind::None) return continuePending(s);
    const std::string& op = c.op;
    const std::string first = c.args.empty() ? std::string() : c.args[0];

    if (op == "down" || op == "up" || op == "tap") {
        const SDL_Scancode scancode = SDL_GetScancodeFromName(first.c_str());
        if (scancode == SDL_SCANCODE_UNKNOWN) {
            log::error("scenario", "line {}: unknown key '{}' (SDL scancode names: W, Space, Left Ctrl, F1, ...)",
                       c.line, first);
            return Exec::Failed;
        }
        pushKey(s, scancode, op != "up");
        if (op != "tap") return Exec::Done;
        s.pendingInput = {.kind = PendingInput::Kind::KeyUp, .scancode = scancode};
        return Exec::Busy;
    }
    if (op == "text") {
        auto text = std::make_unique<char[]>(first.size() + 1);
        std::memcpy(text.get(), first.c_str(), first.size() + 1);
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = SDL_GetWindowID(s.window.get());
        e.text.text = text.get();
        s.textInputs.push_back(std::move(text));
        push(s, e);
        return Exec::Done;
    }
    if (op == "mouse" || op == "click") {
        const std::optional<ScenarioPoint> point = pointArgument(s, c);
        if (!point || c.args.size() > point->tokens + (op == "click" ? 1 : 0)) {
            if (point || !first.starts_with('@'))
                log::error("scenario", "line {}: {} X Y|@NAME{} ({})", c.line, op,
                           op == "click" ? " [left|right|middle]" : "", kPointHelp);
            return Exec::Failed;
        }
        const float x = point->position.x, y = point->position.y;
        pushMouseMotion(s, x, y);
        if (op == "mouse") return Exec::Done;
        const std::string button = c.args.size() > point->tokens ? c.args[point->tokens] : "left";
        const Uint8 sdlButton = button == "right"    ? SDL_BUTTON_RIGHT
                                : button == "middle" ? SDL_BUTTON_MIDDLE
                                                     : SDL_BUTTON_LEFT;
        s.pendingInput = {.kind = PendingInput::Kind::MouseUp, .button = sdlButton};
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.windowID = SDL_GetWindowID(s.window.get());
        e.button.button = sdlButton;
        e.button.down = true;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
        push(s, e);
        return Exec::Busy;
    }
    if (op == "wheel") {
        const auto dy = number(c.args, 0);
        if (!dy) {
            log::error("scenario", "line {}: wheel DY", c.line);
            return Exec::Failed;
        }
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = SDL_GetWindowID(s.window.get());
        e.wheel.y = float(*dy);
        e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        e.wheel.mouse_x = s.input.mousePosition().x;
        e.wheel.mouse_y = s.input.mousePosition().y;
        push(s, e);
        return Exec::Done;
    }
    if (op == "touch") {
        // touch X Y|@NAME [DX DY STEPS]: a tap without the drag arguments.
        const std::optional<ScenarioPoint> point = pointArgument(s, c);
        const float w = float(std::max(1, s.windowWidth)), h = float(std::max(1, s.windowHeight));
        const std::size_t d = point ? point->tokens : 0;
        const auto dx = d + 1 < c.args.size() ? parseScenarioCoordinate(c.args[d], w) : std::nullopt;
        const auto dy = d + 1 < c.args.size() ? parseScenarioCoordinate(c.args[d + 1], h) : std::nullopt;
        const auto steps = number(c.args, d + 2);
        const bool drag = c.args.size() == d + 3 && dx && dy && steps && *steps >= 1.0;
        if (!point || !(c.args.size() == d || drag)) {
            if (point || !first.starts_with('@'))
                log::error("scenario", "line {}: touch X Y|@NAME [DX DY STEPS] ({})", c.line, kPointHelp);
            return Exec::Failed;
        }
        PendingInput p{.kind = PendingInput::Kind::TouchMove, .x = point->position.x / w, .y = point->position.y / h};
        if (drag) {
            p.steps = int(*steps);
            p.dx = *dx / w / float(p.steps);
            p.dy = *dy / h / float(p.steps);
        }
        pushFinger(s, SDL_EVENT_FINGER_DOWN, p.x, p.y, 0.0f, 0.0f);
        s.pendingInput = p;
        return Exec::Busy;
    }
    if (op == "gamepad") return gamepadCommand(s, c);
    if (op == "resize") {
        const auto w = number(c.args, 0), h = number(c.args, 1);
        if (!w || !h || *w < 1 || *h < 1) {
            log::error("scenario", "line {}: resize W H (points)", c.line);
            return Exec::Failed;
        }
        SDL_SetWindowSize(s.window.get(), int(*w), int(*h));
        return Exec::Done;
    }
    if (op == "hdr") {
        const std::optional<HdrMode> mode = parseHdrMode(first);
        if (!mode) {
            log::error("scenario", "line {}: hdr off|on|auto", c.line);
            return Exec::Failed;
        }
        s.app.setHdrMode(*mode);
        return Exec::Done;
    }
    if (op == "capture") {
        if (first.empty()) {
            log::error("scenario", "line {}: capture PATH", c.line);
            return Exec::Failed;
        }
        // Done once the frame is written, so that the next command (a change of the scene, quit) comes after it.
        if (s.scenarioCapturing) {
            if (!s.captureRequest.empty()) return Exec::Busy;
            s.scenarioCapturing = false;
            return Exec::Done;
        }
        s.app.requestCapture(first);
        s.scenarioCapturing = true;
        return Exec::Busy;
    }
    if (op == "perf_begin") {
        if (s.offscreen)
            log::warning("app", "perf region offscreen: frames are not presented; --onscreen to measure as played");
        startPerf(s, first.empty() ? std::string("perf") : first);
        return Exec::Done;
    }
    if (op == "perf_end") {
        if (!s.perf) {
            log::error("scenario", "line {}: perf_end without perf_begin", c.line);
            return Exec::Failed;
        }
        finishPerf(s);
        return Exec::Done;
    }
    if (op == "audio_measure") return audioMeasure(s, c);
    if (op == "max_frame_rate") {
        const auto hz = number(c.args, 0);
        if (c.args.size() != 1 || !hz || *hz < 0.0 || *hz > 1000.0) {
            log::error("scenario", "line {}: max_frame_rate HZ (0 to 1000; 0: the display's rate)", c.line);
            return Exec::Failed;
        }
        s.app.setMaxFrameRate(*hz);
        return Exec::Done;
    }
    if (op == "thermal_state") {
        // A fake state in place of the system's until `thermal_state system`; read again at once, so the thermal
        // reaction follows before this frame's Game::update.
        const std::optional<ThermalState> state = parseThermalState(first);
        if (c.args.size() != 1 || (!state && first != "system")) {
            log::error("scenario", "line {}: thermal_state nominal|fair|serious|critical|unknown|system", c.line);
            return Exec::Failed;
        }
        s.thermalOverride = state;
        if (!state) log::info("app", "thermal state: the system's again");
        updateThermalState(s);
        return Exec::Done;
    }

    switch (s.game->scenarioCommand(s.app, c)) {
    case ScenarioExec::Done: return Exec::Done;
    case ScenarioExec::Busy: return Exec::Busy;
    case ScenarioExec::Failed: return Exec::Failed;
    case ScenarioExec::Unknown: break;
    }
    log::error("scenario", "line {}: unknown command '{}'", c.line, op);
    return Exec::Failed;
}

} // namespace quench::app::detail
