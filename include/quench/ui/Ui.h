#pragma once

// A game's interface, the parts that take input (docs/ui.md): pressable areas and buttons, slots whose contents are
// dragged from one to another (an inventory), tips, a stick and buttons under the thumbs on a touch screen, and the
// way around all of them with a gamepad's D-pad or the arrow keys. Immediate mode: a widget is a call in Game::ui
// that draws itself on the frame's Canvas and says what happened to it; the Ui object keeps what lasts between
// frames (what is held, dragged, focused; the timers of what pops in).
//
// Input comes from the app's Input: the mouse (a first finger arrives as the mouse), every finger for the touch
// stick and buttons, the gamepad and the keyboard for the focus. A press is a release over the widget that was
// pressed, or the confirm button on the focused one.

#include <quench/ui/Canvas.h>

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace quench::app {
class App;
}

namespace quench::ui {

// The look the widgets draw themselves in; a game sets its own.
struct Theme {
    FontId font = 0;
    Color accent = rgb(0x1ad9cc); // what can be pressed, what is chosen
    Color ink = rgb(0x0b0d12);    // outlines and the text on the accent
    Color panel = rgb(0x141821, 0.92f);
    Color well = rgb(0x000000, 0.55f);
    Color text = rgb(0xf2f4f8);
    Color textDim = rgb(0x9aa3b2);
    Color warn = rgb(0xff5a3c);
    float radius = 8.0f;   // design points
    float inkWidth = 2.5f; // design points
    float textSize = 16.0f;
};

// What happened to a widget this frame.
struct Press {
    bool hovered = false; // the pointer is over it
    bool held = false;    // it was pressed and the button is still down
    bool pressed = false; // it was let go over it, or confirmed with the focus on it
    bool focused = false; // the gamepad's or the keyboard's focus is on it
};

struct SlotEvent {
    Press press;
    bool dragging = false; // its content is being dragged away (draw the slot empty)
    int droppedFrom = -1;  // this frame the content of that slot was dropped here
};

class Ui {
public:
    Ui() = default;
    explicit Ui(const Theme& theme) : m_theme(theme) {}

    Theme& theme() { return m_theme; }

    // Once per frame, in Game::ui: before and after the widgets. `dt`: the frame's time, for what animates.
    void begin(const app::App& app, Canvas& canvas, float dt);
    void end();
    Canvas& canvas() { return *m_canvas; }

    // The pointer was over a widget, or held one, in the last frame: the game leaves that click alone.
    bool pointerOverUi() const { return m_overUiLast; }
    // The gamepad or the keys moved the focus more recently than the pointer moved.
    bool focusVisible() const { return m_navigation && m_focusVisible; }
    // D-pad and arrow keys move a focus between the widgets, South / Return presses the focused one. Off by
    // default: a game turns it on while a screen of widgets is open (it takes those buttons).
    void setNavigation(bool on);
    void focus(std::string_view id);

    // ---- widgets
    // An area that takes presses and draws nothing.
    Press area(std::string_view id, const Rect& r, bool enabled = true);
    Press button(std::string_view id, const Rect& r, std::string_view label, bool enabled = true);
    // A pane behind a group of widgets.
    void panel(const Rect& r, float alpha = 1.0f);
    // A tip for the widget made last, shown while the pointer rests on it or it has the focus (drawn in end(), over
    // everything).
    void tip(std::string_view text);

    // A slot of a grid: `index` names it, `filled` says it holds something. Its content can be dragged onto another
    // slot (or picked up and put down with the confirm button); the game draws the content itself, inside the
    // slot's rectangle and, while dragged() is its index, at dragPoint().
    SlotEvent slot(int index, const Rect& r, bool filled);
    int dragged() const { return m_dragging ? m_dragSlot : -1; }
    glm::vec2 dragPoint() const { return m_pointer; }

    // A tip was drawn in the last frame.
    bool tipShown() const { return m_tipShown; }

    // ---- under the thumbs (touch; the mouse works them too)
    // A stick: a finger that goes down inside `zone` gets a stick under it; the result is its deflection, length up
    // to 1, y up. Drawn only while held.
    glm::vec2 stick(std::string_view id, const Rect& zone, float radius);
    // A round button any finger can hold: `held` while one is on it, `pressed` when it went down.
    Press touchButton(std::string_view id, glm::vec2 center, float radius, std::string_view label);

    // ---- what lasts
    // 0..1 for something that appears and goes: toward 1 while `shown`, back to 0 otherwise, over `seconds` (use
    // easeOutBack on it for a pop).
    float presence(std::string_view id, bool shown, float seconds = 0.25f);
    // The middle of a widget of the last frame, by its id (a slot's id is "slot:<index>"): what a scenario clicks.
    std::optional<glm::vec2> point(std::string_view id) const;

private:
    using Id = std::uint64_t;
    struct Item {
        Id id;
        Rect rect;
    };
    static Id hash(std::string_view id);
    Press interact(Id id, const Rect& r, bool enabled);
    void navigate(glm::ivec2 direction);

    Theme m_theme;
    Canvas* m_canvas = nullptr;
    float m_dt = 0.0f;
    // The pointer
    glm::vec2 m_pointer{0.0f};
    bool m_down = false, m_wentDown = false, m_wentUp = false;
    Id m_active = 0; // the widget the pointer went down on
    bool m_overUi = false, m_overUiLast = false;
    // The focus
    bool m_navigation = false, m_focusVisible = false, m_confirm = false;
    Id m_focus = 0;
    std::vector<Item> m_items, m_itemsLast;
    std::unordered_map<Id, glm::vec2> m_points;
    // Dragging between slots
    bool m_dragging = false, m_carried = false; // by the pointer; picked up with the confirm button
    int m_dragSlot = -1;
    glm::vec2 m_dragStart{0.0f};
    // Fingers
    struct Finger {
        std::uint64_t id = 0;
        glm::vec2 position{0.0f};
        bool pressed = false, released = false;
    };
    std::vector<Finger> m_fingers;
    struct StickState {
        std::uint64_t finger = 0;
        bool mouse = false;
        glm::vec2 origin{0.0f};
        bool active = false;
    };
    std::unordered_map<Id, StickState> m_sticks;
    std::unordered_map<Id, float> m_presence;
    // The tip
    Rect m_lastRect;
    bool m_lastHot = false;
    std::string m_tip;
    bool m_tipShown = false;
    Rect m_tipRect;
};

} // namespace quench::ui
