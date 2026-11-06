#include "Test.h"

#include <quench/app/Input.h>

#include <SDL3/SDL.h>

#include <cmath>

using namespace quench;
using app::Binding;
using app::Input;

namespace {

SDL_Event key(SDL_Scancode scancode, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = scancode;
    e.key.down = down;
    return e;
}

SDL_Event finger(Uint32 type, SDL_FingerID id, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.fingerID = id;
    e.tfinger.x = x;
    e.tfinger.y = y;
    return e;
}

// One frame: the engine's order is beginEvents, the events, endEvents.
template <class... Events>
void frame(Input& input, bool uiKeyboard, const Events&... events) {
    input.beginEvents();
    (input.handleEvent(events, 1000.0f, 500.0f), ...);
    input.endEvents(uiKeyboard, false);
}

} // namespace

QUENCH_TEST(input_action_press_release) {
    Input input;
    const auto jump = input.addAction("jump", {Binding::key(SDL_SCANCODE_SPACE), *Binding::key("J")});
    CHECK(input.findAction("jump") == jump, "find");
    CHECK(!input.findAction("run"), "unknown action");
    CHECK(!Binding::key("No Such Key"), "unknown key name");

    frame(input, false, key(SDL_SCANCODE_SPACE, true));
    CHECK(input.down(jump) && input.pressed(jump) && !input.released(jump), "pressed");
    frame(input, false);
    CHECK(input.down(jump) && !input.pressed(jump), "held");
    frame(input, false, key(SDL_SCANCODE_J, true), key(SDL_SCANCODE_SPACE, false));
    CHECK(input.down(jump) && !input.pressed(jump) && !input.released(jump), "another binding keeps it down");
    frame(input, false, key(SDL_SCANCODE_J, false));
    CHECK(!input.down(jump) && input.released(jump), "released");
    CHECK(input.keyPressed(SDL_SCANCODE_J) == false, "raw press cleared");
}

QUENCH_TEST(input_ui_capture_masks_keys) {
    Input input;
    const auto fire = input.addAction("fire", {Binding::key(SDL_SCANCODE_F)});
    frame(input, true, key(SDL_SCANCODE_F, true));
    CHECK(!input.down(fire), "ImGui has the keyboard");
    CHECK(input.keyDown(SDL_SCANCODE_F), "the raw state is still there");
    frame(input, false);
    CHECK(input.down(fire) && input.pressed(fire), "pressed once ImGui lets go");
}

QUENCH_TEST(input_axis_from_keys) {
    Input input;
    const auto left = input.addAction("left", {Binding::key(SDL_SCANCODE_A)});
    const auto right = input.addAction("right", {Binding::key(SDL_SCANCODE_D)});
    frame(input, false, key(SDL_SCANCODE_D, true));
    CHECK(input.axis(left, right) == 1.0f, "%f", double(input.axis(left, right)));
    frame(input, false, key(SDL_SCANCODE_A, true));
    CHECK(input.axis(left, right) == 0.0f, "both cancel");
}

QUENCH_TEST(input_fingers) {
    Input input;
    frame(input, false, finger(SDL_EVENT_FINGER_DOWN, 7, 0.5f, 0.5f));
    REQUIRE(input.fingers().size() == 1, "%zu", input.fingers().size());
    CHECK(input.fingers()[0].pressed && input.fingers()[0].position.x == 500.0f, "down at %f",
          double(input.fingers()[0].position.x));
    frame(input, false, finger(SDL_EVENT_FINGER_MOTION, 7, 0.6f, 0.5f), finger(SDL_EVENT_FINGER_MOTION, 7, 0.7f, 0.4f));
    REQUIRE(input.fingers().size() == 1, "%zu", input.fingers().size());
    CHECK(std::abs(input.fingers()[0].delta.x - 200.0f) < 1e-3f && std::abs(input.fingers()[0].delta.y + 50.0f) < 1e-3f,
          "delta %f %f", double(input.fingers()[0].delta.x), double(input.fingers()[0].delta.y));
    CHECK(!input.fingers()[0].pressed, "pressed only in the first frame");
    frame(input, false, finger(SDL_EVENT_FINGER_UP, 7, 0.7f, 0.4f));
    REQUIRE(input.fingers().size() == 1, "lifted fingers stay for one frame");
    CHECK(input.fingers()[0].released, "released");
    frame(input, false);
    CHECK(input.fingers().empty(), "%zu", input.fingers().size());

    // A trackpad's touches (positions on the pad) are not fingers.
    SDL_Event pad = finger(SDL_EVENT_FINGER_DOWN, 8, 0.5f, 0.5f);
    pad.tfinger.touchID = SDL_MOUSE_TOUCHID;
    frame(input, false, pad);
    CHECK(input.fingers().empty(), "trackpad touch as a finger: %zu", input.fingers().size());
}

QUENCH_TEST(input_mouse) {
    Input input;
    const auto shoot = input.addAction("shoot", {Binding::mouse(app::MouseButton::Left)});
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 10.0f;
    motion.motion.y = 20.0f;
    motion.motion.xrel = 3.0f;
    SDL_Event button{};
    button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    button.button.button = SDL_BUTTON_LEFT;
    button.button.down = true;
    button.button.x = 10.0f;
    button.button.y = 20.0f;
    frame(input, false, motion, button);
    CHECK(input.down(shoot) && input.mousePressed(app::MouseButton::Left), "left button");
    CHECK(input.mousePosition().x == 10.0f && input.mouseDelta().x == 3.0f, "position and delta");
    CHECK(!input.mouseFromTouch(), "a real mouse");
    frame(input, false);
    CHECK(input.mouseDelta().x == 0.0f, "delta per frame");
}
