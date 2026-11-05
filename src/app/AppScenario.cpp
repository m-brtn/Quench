// The app's scenario commands and values (include/quench/app/Scenario.h). Input commands push SDL events, so the
// engine, ImGui and the game see them exactly like real input, one frame later.

#include "AppState.h"

#include <quench/core/Log.h>
#include <quench/core/Parse.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace quench::app::detail {
namespace {

std::optional<double> number(const std::vector<std::string>& args, std::size_t index) {
    if (index >= args.size()) return std::nullopt;
    return parseDouble(args[index]);
}

void push(AppState& s, SDL_Event& e) {
    SDL_PushEvent(&e);
    s.inputPushedFrame = s.frame;
}

Scenario::Exec pushKey(AppState& s, SDL_Scancode scancode, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(s.window.get());
    e.key.scancode = scancode;
    e.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
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

void pushFinger(AppState& s, Uint32 type, float x, float y, float dx, float dy) {
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

void finishPerf(AppState& s) {
    const PerfRegion& r = *s.perf;
    auto& v = s.perfValues;
    v.clear();
    v["perf_frames"] = double(r.frameMs.size());
    v["perf_frame_avg"] = average(r.frameMs);
    v["perf_frame_p50"] = percentile(r.frameMs, 0.50);
    v["perf_frame_p95"] = percentile(r.frameMs, 0.95);
    v["perf_frame_p99"] = percentile(r.frameMs, 0.99);
    v["perf_frame_max"] = r.frameMs.empty() ? -1.0 : *std::max_element(r.frameMs.begin(), r.frameMs.end());
    v["perf_cpu_avg"] = average(r.cpuMs);
    v["perf_cpu_p50"] = percentile(r.cpuMs, 0.50);
    v["perf_cpu_p95"] = percentile(r.cpuMs, 0.95);
    v["perf_cpu_p99"] = percentile(r.cpuMs, 0.99);
    v["perf_gpu_avg"] = average(r.gpuMs);
    v["perf_gpu_p50"] = percentile(r.gpuMs, 0.50);
    v["perf_gpu_p95"] = percentile(r.gpuMs, 0.95);
    log::info("perf",
              "{}: {} frames; frame ms avg {:.3f} p50 {:.3f} p95 {:.3f} p99 {:.3f} max {:.3f}; cpu ms avg {:.3f} "
              "p50 {:.3f} p95 {:.3f} p99 {:.3f}; gpu ms avg {:.3f} p50 {:.3f} p95 {:.3f} ({} samples)",
              r.label, r.frameMs.size(), v["perf_frame_avg"], v["perf_frame_p50"], v["perf_frame_p95"],
              v["perf_frame_p99"], v["perf_frame_max"], v["perf_cpu_avg"], v["perf_cpu_p50"], v["perf_cpu_p95"],
              v["perf_cpu_p99"], v["perf_gpu_avg"], v["perf_gpu_p50"], v["perf_gpu_p95"], r.gpuMs.size());
    s.perf.reset();
}

} // namespace

void releaseScenarioInput(AppState& s) {
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
    if (const auto it = s.perfValues.find(name); it != s.perfValues.end()) return it->second;
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
        const auto x = number(c.args, 0), y = number(c.args, 1);
        if (!x || !y) {
            log::error("scenario", "line {}: {} X Y (window points)", c.line, op);
            return Exec::Failed;
        }
        pushMouseMotion(s, float(*x), float(*y));
        if (op == "mouse") return Exec::Done;
        const std::string button = c.args.size() > 2 ? c.args[2] : "left";
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
        e.button.x = float(*x);
        e.button.y = float(*y);
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
        // touch X Y [DX DY STEPS]: points; a tap without the drag arguments.
        const auto x = number(c.args, 0), y = number(c.args, 1);
        const auto dx = number(c.args, 2), dy = number(c.args, 3), steps = number(c.args, 4);
        const bool drag = c.args.size() == 5 && dx && dy && steps && *steps >= 1.0;
        if (!x || !y || !(c.args.size() == 2 || drag)) {
            log::error("scenario", "line {}: touch X Y [DX DY STEPS] (window points)", c.line);
            return Exec::Failed;
        }
        const float w = float(std::max(1, s.windowWidth)), h = float(std::max(1, s.windowHeight));
        PendingInput p{.kind = PendingInput::Kind::TouchMove, .x = float(*x) / w, .y = float(*y) / h};
        if (drag) {
            p.steps = int(*steps);
            p.dx = float(*dx) / w / float(p.steps);
            p.dy = float(*dy) / h / float(p.steps);
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
    if (op == "capture") {
        if (first.empty()) {
            log::error("scenario", "line {}: capture PATH", c.line);
            return Exec::Failed;
        }
        s.app.requestCapture(first);
        return Exec::Done;
    }
    if (op == "perf_begin") {
        s.perf = PerfRegion{.label = first.empty() ? std::string("perf") : first};
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
