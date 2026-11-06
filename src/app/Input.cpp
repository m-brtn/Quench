#include <quench/app/Input.h>

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace quench::app {

static_assert(int(GamepadButton::DpadRight) == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
static_assert(int(GamepadAxis::RightTrigger) == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

std::optional<Binding> Binding::key(std::string_view name) {
    const SDL_Scancode scancode = SDL_GetScancodeFromName(std::string(name).c_str());
    if (scancode == SDL_SCANCODE_UNKNOWN) return std::nullopt;
    return Binding::key(int(scancode));
}

Input::Input() : m_keys(SDL_SCANCODE_COUNT) {}

Input::~Input() {
    if (m_gamepad) SDL_CloseGamepad(m_gamepad);
}

ActionId Input::addAction(std::string name, std::span<const Binding> bindings) {
    QUENCH_ASSERT(!findAction(name), "action names are unique");
    m_actions.push_back({std::move(name), {bindings.begin(), bindings.end()}});
    return ActionId(m_actions.size() - 1);
}

void Input::bind(ActionId action, const Binding& binding) {
    QUENCH_ASSERT(action < m_actions.size(), "unknown action");
    m_actions[action].bindings.push_back(binding);
}

void Input::clearBindings(ActionId action) {
    QUENCH_ASSERT(action < m_actions.size(), "unknown action");
    m_actions[action].bindings.clear();
}

std::optional<ActionId> Input::findAction(std::string_view name) const {
    for (std::size_t i = 0; i < m_actions.size(); ++i)
        if (m_actions[i].name == name) return ActionId(i);
    return std::nullopt;
}

float Input::value(ActionId action) const {
    QUENCH_ASSERT(action < m_actions.size(), "unknown action");
    return m_actions[action].value;
}

bool Input::pressed(ActionId action) const {
    QUENCH_ASSERT(action < m_actions.size(), "unknown action");
    return m_actions[action].pressed;
}

bool Input::released(ActionId action) const {
    QUENCH_ASSERT(action < m_actions.size(), "unknown action");
    return m_actions[action].released;
}

bool Input::keyDown(int scancode) const {
    return scancode >= 0 && scancode < int(m_keys.size()) && m_keys[std::size_t(scancode)].down;
}

bool Input::keyPressed(int scancode) const {
    return scancode >= 0 && scancode < int(m_keys.size()) && m_keys[std::size_t(scancode)].pressed;
}

std::string Input::gamepadName() const {
    const char* name = m_gamepad ? SDL_GetGamepadName(m_gamepad) : nullptr;
    return name ? name : "";
}

void Input::press(State& state, bool down) {
    if (down && !state.down) state.pressed = true;
    if (!down && state.down) state.released = true;
    state.down = down;
}

void Input::restrictGamepad(std::optional<std::uint32_t> id) {
    m_onlyGamepad = id;
    if (m_gamepad && id && m_gamepadId != *id) closeGamepad();
    if (!m_gamepad) openFirstGamepad();
}

void Input::closeGamepad() {
    SDL_CloseGamepad(m_gamepad);
    m_gamepad = nullptr;
    m_gamepadId = 0;
    for (State& button : m_buttons) press(button, false);
    m_rawAxes.fill(0.0f);
}

void Input::openFirstGamepad() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count && !m_gamepad; ++i) {
        if (m_onlyGamepad && ids[i] != *m_onlyGamepad) continue;
        m_gamepad = SDL_OpenGamepad(ids[i]);
        if (m_gamepad) {
            m_gamepadId = ids[i];
            log::info("app", "gamepad: {}", gamepadName());
        }
    }
    SDL_free(ids);
}

void Input::beginEvents() {
    for (State& key : m_keys) key.pressed = key.released = false;
    for (State& button : m_mouse) button.pressed = button.released = false;
    for (State& button : m_buttons) button.pressed = button.released = false;
    m_mouseDelta = glm::vec2(0.0f);
    m_wheel = glm::vec2(0.0f);
    m_mouseFromTouch = false;
    std::erase_if(m_fingers, [](const Finger& f) { return f.released; });
    for (Finger& finger : m_fingers) {
        finger.delta = glm::vec2(0.0f);
        finger.pressed = false;
    }
}

void Input::handleEvent(const SDL_Event& e, float windowWidth, float windowHeight) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e.key.scancode < m_keys.size() && !e.key.repeat) press(m_keys[e.key.scancode], e.key.down);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        m_mousePosition = {e.motion.x, e.motion.y};
        m_mouseDelta += glm::vec2(e.motion.xrel, e.motion.yrel);
        m_mouseFromTouch |= e.motion.which == SDL_TOUCH_MOUSEID;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        m_mousePosition = {e.button.x, e.button.y};
        m_mouseFromTouch |= e.button.which == SDL_TOUCH_MOUSEID;
        if (e.button.button >= 1 && e.button.button <= int(MouseButton::Count))
            press(m_mouse[std::size_t(e.button.button - 1)], e.button.down);
        break;
    case SDL_EVENT_MOUSE_WHEEL: m_wheel += glm::vec2(e.wheel.x, e.wheel.y); break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        // SDL's macOS trackpad touches (and touches it makes from the mouse) come from the mouse's touch id.
        if (e.tfinger.touchID == SDL_MOUSE_TOUCHID) break;
        const glm::vec2 position(e.tfinger.x * windowWidth, e.tfinger.y * windowHeight);
        auto it = std::find_if(m_fingers.begin(), m_fingers.end(),
                               [&](const Finger& f) { return f.id == e.tfinger.fingerID && !f.released; });
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            if (it == m_fingers.end()) m_fingers.push_back({e.tfinger.fingerID, position, glm::vec2(0.0f), true});
        } else if (it != m_fingers.end()) {
            it->delta += position - it->position;
            it->position = position;
            if (e.type != SDL_EVENT_FINGER_MOTION) it->released = true;
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!m_gamepad) openFirstGamepad();
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (m_gamepad && e.gdevice.which == m_gamepadId) {
            log::info("app", "gamepad removed: {}", gamepadName());
            closeGamepad();
            openFirstGamepad();
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (e.gbutton.which == m_gamepadId && e.gbutton.button < m_buttons.size())
            press(m_buttons[e.gbutton.button], e.gbutton.down);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e.gaxis.which == m_gamepadId && e.gaxis.axis < m_rawAxes.size())
            m_rawAxes[e.gaxis.axis] = std::clamp(float(e.gaxis.value) / 32767.0f, -1.0f, 1.0f);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // SDL sends key-up events itself; mouse buttons released outside the window are not reported.
        for (State& button : m_mouse) press(button, false);
        break;
    default: break;
    }
}

float Input::bindingValue(const Binding& binding) const {
    switch (binding.kind) {
    case Binding::Kind::Key: return !m_uiKeyboard && keyDown(binding.code) ? 1.0f : 0.0f;
    case Binding::Kind::Mouse:
        return !m_uiMouse && binding.code < int(MouseButton::Count) && m_mouse[std::size_t(binding.code)].down ? 1.0f
                                                                                                               : 0.0f;
    case Binding::Kind::Gamepad:
        return binding.code < int(GamepadButton::Count) && m_buttons[std::size_t(binding.code)].down ? 1.0f : 0.0f;
    case Binding::Kind::GamepadAxisPositive:
        return binding.code < int(GamepadAxis::Count) ? std::max(0.0f, m_axes[std::size_t(binding.code)]) : 0.0f;
    case Binding::Kind::GamepadAxisNegative:
        return binding.code < int(GamepadAxis::Count) ? std::max(0.0f, -m_axes[std::size_t(binding.code)]) : 0.0f;
    }
    return 0.0f;
}

void Input::endEvents(bool uiKeyboard, bool uiMouse) {
    m_uiKeyboard = uiKeyboard;
    m_uiMouse = uiMouse;
    const auto rescale = [this](float magnitude) {
        return magnitude <= deadZone ? 0.0f : std::min(1.0f, (magnitude - deadZone) / (1.0f - deadZone));
    };
    // Sticks: a radial dead zone keeps diagonals; triggers: a linear one.
    for (const auto [x, y] :
         {std::pair(GamepadAxis::LeftX, GamepadAxis::LeftY), std::pair(GamepadAxis::RightX, GamepadAxis::RightY)}) {
        const glm::vec2 raw(m_rawAxes[std::size_t(x)], m_rawAxes[std::size_t(y)]);
        const float magnitude = std::sqrt(raw.x * raw.x + raw.y * raw.y);
        const glm::vec2 scaled = magnitude > 0.0f ? raw * (rescale(magnitude) / magnitude) : glm::vec2(0.0f);
        m_axes[std::size_t(x)] = scaled.x;
        m_axes[std::size_t(y)] = scaled.y;
    }
    for (const GamepadAxis trigger : {GamepadAxis::LeftTrigger, GamepadAxis::RightTrigger})
        m_axes[std::size_t(trigger)] = rescale(std::max(0.0f, m_rawAxes[std::size_t(trigger)]));
    for (Action& action : m_actions) {
        float value = 0.0f;
        for (const Binding& binding : action.bindings) value = std::max(value, bindingValue(binding));
        action.value = value;
        const bool isDown = value >= kDownThreshold;
        action.pressed = isDown && !action.wasDown;
        action.released = !isDown && action.wasDown;
        action.wasDown = isDown;
    }
}

} // namespace quench::app
