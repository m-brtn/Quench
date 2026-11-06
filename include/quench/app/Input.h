#pragma once

// Input of one frame from SDL events: keyboard, mouse, the first connected gamepad and touch. Games read named
// actions bound to physical controls, or the raw state. State changes come only from events (also the ones a
// scenario pushes), so scripted input takes the same path as real input.

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

union SDL_Event;
struct SDL_Gamepad;

namespace quench::app {

enum class MouseButton : std::uint8_t { Left, Middle, Right, X1, X2, Count };

// The order of SDL_GamepadButton (South: A on Xbox, Cross on PlayStation).
enum class GamepadButton : std::uint8_t {
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Count
};

// The order of SDL_GamepadAxis. Sticks: -1..1, x right, y down. Triggers: 0..1.
enum class GamepadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

// A physical control an action reads.
struct Binding {
    enum class Kind : std::uint8_t { Key, Mouse, Gamepad, GamepadAxisPositive, GamepadAxisNegative };
    Kind kind = Kind::Key;
    int code = 0; // SDL_Scancode, MouseButton, GamepadButton or GamepadAxis

    static Binding key(int scancode) { return {Kind::Key, scancode}; }
    // SDL scancode name ("W", "Space", "Left Ctrl", "F1"); empty when unknown.
    static std::optional<Binding> key(std::string_view name);
    static Binding mouse(MouseButton button) { return {Kind::Mouse, int(button)}; }
    static Binding gamepad(GamepadButton button) { return {Kind::Gamepad, int(button)}; }
    static Binding gamepadAxis(GamepadAxis axis, bool positive) {
        return {positive ? Kind::GamepadAxisPositive : Kind::GamepadAxisNegative, int(axis)};
    }
};

using ActionId = std::uint32_t;

// A finger on the touch screen (SDL finger events), in window points. A trackpad's touches (macOS) are not fingers:
// their positions are on the pad, not in the window, and they already move the mouse.
struct Finger {
    std::uint64_t id = 0;
    glm::vec2 position{0.0f};
    glm::vec2 delta{0.0f}; // movement this frame
    bool pressed = false;  // touched down this frame
    bool released = false; // lifted this frame (removed next frame)
};

class Input {
public:
    Input();
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    // ---- actions
    ActionId addAction(std::string name, std::span<const Binding> bindings = {});
    ActionId addAction(std::string name, std::initializer_list<Binding> bindings) {
        return addAction(std::move(name), std::span<const Binding>(bindings.begin(), bindings.size()));
    }
    void bind(ActionId action, const Binding& binding);
    void clearBindings(ActionId action);
    std::optional<ActionId> findAction(std::string_view name) const;
    // The strongest of the action's controls, 0..1: keys and buttons 0 or 1, axis directions after the dead zone.
    float value(ActionId action) const;
    bool down(ActionId action) const { return value(action) >= kDownThreshold; }
    bool pressed(ActionId action) const;  // went down this frame
    bool released(ActionId action) const; // went up this frame
    // value(positive) - value(negative), -1..1.
    float axis(ActionId negative, ActionId positive) const { return value(positive) - value(negative); }

    // ---- raw state
    bool keyDown(int scancode) const;
    bool keyPressed(int scancode) const;
    glm::vec2 mousePosition() const { return m_mousePosition; } // window points
    glm::vec2 mouseDelta() const { return m_mouseDelta; }
    glm::vec2 wheel() const { return m_wheel; } // + y: away from the user
    bool mouseDown(MouseButton button) const { return m_mouse[std::size_t(button)].down; }
    bool mousePressed(MouseButton button) const { return m_mouse[std::size_t(button)].pressed; }
    // The mouse events of this frame were synthesized by SDL from a finger.
    bool mouseFromTouch() const { return m_mouseFromTouch; }
    std::span<const Finger> fingers() const { return m_fingers; }

    bool gamepadConnected() const { return m_gamepad != nullptr; }
    std::string gamepadName() const;
    float gamepadAxis(GamepadAxis axis) const { return m_axes[std::size_t(axis)]; } // after the dead zone
    bool gamepadDown(GamepadButton button) const { return m_buttons[std::size_t(button)].down; }
    bool gamepadPressed(GamepadButton button) const { return m_buttons[std::size_t(button)].pressed; }
    float deadZone = 0.15f; // stick deflections and trigger values below it read as 0; the rest is rescaled to 0..1

    // ImGui wants the keyboard / mouse this frame: key and mouse bindings read as up; the raw state stays available.
    bool uiCapturesKeyboard() const { return m_uiKeyboard; }
    bool uiCapturesMouse() const { return m_uiMouse; }

    // ---- driven by the engine: beginEvents() clears this frame's presses, deltas and lifted fingers, the events of
    // the next frame follow, endEvents() then updates the actions.
    void beginEvents();
    void handleEvent(const SDL_Event& event, float windowWidth, float windowHeight);
    void endEvents(bool uiKeyboard, bool uiMouse);
    // Only the gamepad with this SDL joystick id is opened (0: none); nullopt, the default: the first connected one.
    // Runs on scripted input open only the scenario's virtual gamepad.
    void restrictGamepad(std::optional<std::uint32_t> id);

    static constexpr float kDownThreshold = 0.5f;

private:
    struct State {
        bool down = false;
        bool pressed = false;
        bool released = false;
    };
    struct Action {
        std::string name;
        std::vector<Binding> bindings;
        float value = 0.0f;
        bool wasDown = false;
        bool pressed = false;
        bool released = false;
    };
    float bindingValue(const Binding& binding) const;
    static void press(State& state, bool down);
    void openFirstGamepad();
    void closeGamepad();

    std::vector<Action> m_actions;
    std::vector<State> m_keys; // per SDL scancode
    std::array<State, std::size_t(MouseButton::Count)> m_mouse{};
    std::array<State, std::size_t(GamepadButton::Count)> m_buttons{};
    std::array<float, std::size_t(GamepadAxis::Count)> m_axes{};
    std::array<float, std::size_t(GamepadAxis::Count)> m_rawAxes{};
    glm::vec2 m_mousePosition{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
    glm::vec2 m_wheel{0.0f};
    bool m_mouseFromTouch = false;
    std::vector<Finger> m_fingers;
    SDL_Gamepad* m_gamepad = nullptr;
    std::uint32_t m_gamepadId = 0;
    std::optional<std::uint32_t> m_onlyGamepad;
    bool m_uiKeyboard = false;
    bool m_uiMouse = false;
};

} // namespace quench::app
