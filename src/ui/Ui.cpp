#include <quench/ui/Ui.h>

#include <quench/app/App.h>
#include <quench/app/Input.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace quench::ui {

namespace {

int scancode(const char* name) {
    const std::optional<app::Binding> binding = app::Binding::key(name);
    return binding ? binding->code : 0;
}

Color lighter(const Color& c, float by) {
    return {c.r + (1.0f - c.r) * by, c.g + (1.0f - c.g) * by, c.b + (1.0f - c.b) * by, c.a};
}

} // namespace

Ui::Id Ui::hash(std::string_view id) {
    // FNV-1a
    Id h = 1469598103934665603ull;
    for (const char c : id) h = (h ^ Id(static_cast<unsigned char>(c))) * 1099511628211ull;
    return h ? h : 1;
}

void Ui::begin(const app::App& app, Canvas& canvas, float dt) {
    m_canvas = &canvas;
    m_dt = dt;
    const app::Input& input = app.input();

    const glm::vec2 pointer = input.mousePosition();
    if (glm::distance(pointer, m_pointer) > 1.0f) m_focusVisible = false;
    m_pointer = pointer;
    // A tool window of ImGui's under the pointer takes the mouse: the game's widgets see no button then.
    const bool down = input.mouseDown(app::MouseButton::Left) && !input.uiCapturesMouse();
    m_wentDown = down && !m_down;
    m_wentUp = !down && m_down;
    m_down = down;

    m_fingers.clear();
    for (const app::Finger& finger : input.fingers())
        m_fingers.push_back({finger.id, finger.position, finger.pressed, finger.released});

    // The focus moves over the widgets of the last frame.
    m_confirm = false;
    if (m_navigation) {
        static const int kUp = scancode("Up"), kDown = scancode("Down"), kLeft = scancode("Left"),
                         kRight = scancode("Right"), kReturn = scancode("Return");
        using app::GamepadButton;
        glm::ivec2 direction(0);
        if (input.gamepadPressed(GamepadButton::DpadUp) || input.keyPressed(kUp)) direction.y = -1;
        if (input.gamepadPressed(GamepadButton::DpadDown) || input.keyPressed(kDown)) direction.y = 1;
        if (input.gamepadPressed(GamepadButton::DpadLeft) || input.keyPressed(kLeft)) direction.x = -1;
        if (input.gamepadPressed(GamepadButton::DpadRight) || input.keyPressed(kRight)) direction.x = 1;
        if (direction != glm::ivec2(0)) navigate(direction);
        if (input.gamepadPressed(GamepadButton::South) || input.keyPressed(kReturn)) {
            m_confirm = m_focusVisible;
            m_focusVisible = true;
        }
    }

    m_items.clear();
    m_points.clear();
    m_overUi = false;
    m_tip.clear();
    m_lastHot = false;
}

void Ui::end() {
    Canvas& c = *m_canvas;
    // A drag that ended over no slot is put back.
    if (m_dragging && !m_carried && !m_down) {
        m_dragging = false;
        m_dragSlot = -1;
    }
    if (!m_down) m_active = 0;

    // The tip, over everything, kept inside the safe area.
    m_tipShown = !m_tip.empty();
    if (!m_tip.empty()) {
        TextStyle style{.font = m_theme.font, .size = c.u(m_theme.textSize * 0.9f), .color = m_theme.text};
        const float wrap = c.u(260.0f);
        const float pad = c.u(8.0f);
        const glm::vec2 size = c.measure(style, m_tip, wrap) + glm::vec2(pad * 2.0f);
        const Rect safe = c.safe();
        glm::vec2 at(m_tipRect.center().x - size.x * 0.5f, m_tipRect.min.y - size.y - c.u(8.0f));
        if (at.y < safe.min.y) at.y = m_tipRect.max.y + c.u(8.0f);
        at.x = std::clamp(at.x, safe.min.x + pad, std::max(safe.min.x + pad, safe.max.x - size.x - pad));
        const Rect box = Rect::at(at, size);
        c.rect(box.grown(c.u(m_theme.inkWidth)), m_theme.ink, c.u(m_theme.radius));
        c.rect(box, m_theme.panel, c.u(m_theme.radius * 0.8f));
        c.text(box.min + glm::vec2(pad), style, m_tip, {0.0f, 0.0f}, wrap);
    }

    m_itemsLast = m_items;
    m_overUiLast = m_overUi || m_dragging;
    // A focus on a widget that is no longer there goes to the first one.
    if (m_navigation && m_focus != 0 &&
        std::none_of(m_itemsLast.begin(), m_itemsLast.end(), [&](const Item& item) { return item.id == m_focus; }))
        m_focus = m_itemsLast.empty() ? 0 : m_itemsLast.front().id;
}

void Ui::setNavigation(bool on) {
    if (on == m_navigation) return;
    m_navigation = on;
    if (!on) {
        m_focusVisible = false;
        m_carried = false;
        if (m_dragging && !m_down) m_dragging = false;
    }
}

void Ui::focus(std::string_view id) {
    m_focus = hash(id);
    m_focusVisible = true;
}

void Ui::navigate(glm::ivec2 direction) {
    m_focusVisible = true;
    if (m_itemsLast.empty()) return;
    const auto current =
        std::find_if(m_itemsLast.begin(), m_itemsLast.end(), [&](const Item& item) { return item.id == m_focus; });
    if (current == m_itemsLast.end()) {
        m_focus = m_itemsLast.front().id;
        return;
    }
    // The nearest widget in that direction: distance along it, plus more for what lies to the side.
    const glm::vec2 from = current->rect.center();
    const glm::vec2 along(float(direction.x), float(direction.y));
    float best = 1.0e9f;
    for (const Item& item : m_itemsLast) {
        if (item.id == m_focus) continue;
        const glm::vec2 to = item.rect.center() - from;
        const float forward = glm::dot(to, glm::normalize(along));
        if (forward < 1.0f) continue;
        const float side = std::abs(to.x * along.y - to.y * along.x) / glm::length(along);
        const float score = forward + side * 2.5f;
        if (score < best) {
            best = score;
            m_focus = item.id;
        }
    }
}

Press Ui::interact(Id id, const Rect& r, bool enabled) {
    m_items.push_back({id, r});
    m_points[id] = r.center();
    m_lastRect = r;
    Press press;
    if (!enabled) {
        m_lastHot = false;
        return press;
    }
    press.hovered = r.contains(m_pointer) && (m_active == 0 || m_active == id);
    if (press.hovered && m_wentDown) {
        m_active = id;
        m_focus = id; // the focus starts from what was pressed last
    }
    press.held = m_active == id && m_down;
    press.pressed = m_active == id && m_wentUp && r.contains(m_pointer);
    press.focused = m_navigation && m_focusVisible && m_focus == id;
    if (press.focused && m_confirm) press.pressed = true;
    if (press.hovered || press.held) m_overUi = true;
    m_lastHot = (press.hovered && !m_down) || press.focused;
    return press;
}

Press Ui::area(std::string_view id, const Rect& r, bool enabled) { return interact(hash(id), r, enabled); }

Press Ui::button(std::string_view id, const Rect& r, std::string_view label, bool enabled) {
    const Press press = interact(hash(id), r, enabled);
    Canvas& c = *m_canvas;
    const float ink = c.u(m_theme.inkWidth), radius = c.u(m_theme.radius);
    // A comic button: an ink edge with a thicker foot it sinks onto while held.
    const float sink = press.held ? ink : 0.0f;
    const Rect face{r.min + glm::vec2(0.0f, sink), r.max + glm::vec2(0.0f, sink)};
    c.rect({r.min - glm::vec2(ink), r.max + glm::vec2(ink, ink * 2.2f)}, m_theme.ink, radius + ink);
    Color fill = enabled ? m_theme.accent : m_theme.well;
    if (press.hovered || press.focused) fill = lighter(fill, 0.25f);
    c.rect(face, fill, radius);
    if (press.focused) c.frame(face.grown(ink * 2.0f), m_theme.text, ink, radius + ink * 2.0f);
    c.text(face.center(),
           {.font = m_theme.font, .size = c.u(m_theme.textSize), .color = enabled ? m_theme.ink : m_theme.textDim},
           label, {0.5f, 0.5f});
    return press;
}

void Ui::panel(const Rect& r, float alpha) {
    Canvas& c = *m_canvas;
    Color ink = m_theme.ink, fill = m_theme.panel;
    ink.a *= alpha;
    fill.a *= alpha;
    c.rect(r.grown(c.u(m_theme.inkWidth)), ink, c.u(m_theme.radius * 1.5f) + c.u(m_theme.inkWidth));
    c.rect(r, fill, c.u(m_theme.radius * 1.5f));
    if (r.contains(m_pointer)) m_overUi = true;
}

void Ui::tip(std::string_view text) {
    if (!m_lastHot || text.empty() || m_dragging) return;
    m_tip = std::string(text);
    m_tipRect = m_lastRect;
}

SlotEvent Ui::slot(int index, const Rect& r, bool filled) {
    const Id id = hash(std::format("slot:{}", index));
    SlotEvent event;
    event.press = interact(id, r, true);
    Canvas& c = *m_canvas;
    const float ink = c.u(m_theme.inkWidth), radius = c.u(m_theme.radius * 0.8f);

    // By the pointer: it leaves the slot once the pointer has moved a little with the button down.
    if (filled && event.press.hovered && m_wentDown) {
        m_dragSlot = index;
        m_dragStart = m_pointer;
        m_dragging = false;
        m_carried = false;
    }
    if (!m_dragging && m_dragSlot == index && m_active == id && m_down &&
        glm::distance(m_pointer, m_dragStart) > c.u(6.0f))
        m_dragging = true;
    const bool over = r.contains(m_pointer);
    if (m_dragging && !m_carried && m_wentUp && over && m_dragSlot != index) {
        event.droppedFrom = m_dragSlot;
        m_dragging = false;
        m_dragSlot = -1;
    }
    // By the confirm button: once on a filled slot picks its content up, again on another puts it down.
    if (event.press.focused && m_confirm) {
        if (m_carried && m_dragSlot != index) {
            event.droppedFrom = m_dragSlot;
            m_carried = m_dragging = false;
            m_dragSlot = -1;
        } else if (m_carried) {
            m_carried = m_dragging = false;
            m_dragSlot = -1;
        } else if (filled) {
            m_carried = m_dragging = true;
            m_dragSlot = index;
        }
        event.press.pressed = false;
    }
    event.dragging = m_dragging && m_dragSlot == index;

    const bool target = m_dragging && m_dragSlot != index && (m_carried ? event.press.focused : over);
    c.rect(r.grown(ink), m_theme.ink, radius + ink);
    c.rect(r, target ? lighter(m_theme.well, 0.25f) : m_theme.well, radius);
    if (target || event.press.focused)
        c.frame(r, m_theme.accent, ink, radius);
    else if (event.press.hovered && !m_dragging)
        c.frame(r, m_theme.textDim, ink * 0.6f, radius);
    return event;
}

glm::vec2 Ui::stick(std::string_view id, const Rect& zone, float radius) {
    StickState& state = m_sticks[hash(id)];
    glm::vec2 position(0.0f);
    bool held = false;
    if (state.active && !state.mouse) {
        for (const Finger& finger : m_fingers)
            if (finger.id == state.finger && !finger.released) {
                position = finger.position;
                held = true;
            }
    } else if (state.active) {
        position = m_pointer;
        held = m_down;
    }
    if (!held) {
        state.active = false;
        // A finger, or without fingers the mouse, that goes down in the zone and on no widget.
        for (const Finger& finger : m_fingers)
            if (finger.pressed && zone.contains(finger.position)) {
                state = {finger.id, false, finger.position, true};
                position = finger.position;
                held = true;
                break;
            }
        if (!held && m_fingers.empty() && m_wentDown && zone.contains(m_pointer) && m_active == 0) {
            state = {0, true, m_pointer, true};
            position = m_pointer;
            held = true;
        }
    }
    if (!held) return glm::vec2(0.0f);
    m_overUi = true;
    glm::vec2 offset = (position - state.origin) / std::max(radius, 1.0f);
    if (const float length = glm::length(offset); length > 1.0f) offset /= length;
    Canvas& c = *m_canvas;
    c.circle(state.origin, radius * 1.25f, {m_theme.ink.r, m_theme.ink.g, m_theme.ink.b, 0.45f});
    c.ring(state.origin, radius * 1.25f, m_theme.accent, c.u(m_theme.inkWidth));
    c.circle(state.origin + offset * radius, radius * 0.5f, m_theme.accent);
    return {offset.x, -offset.y};
}

Press Ui::touchButton(std::string_view id, glm::vec2 center, float radius, std::string_view label) {
    const Id key = hash(id);
    const Rect r = Rect::around(center, glm::vec2(radius * 2.0f));
    m_items.push_back({key, r});
    m_points[key] = center;
    Press press;
    const auto inside = [&](glm::vec2 p) { return glm::distance(p, center) <= radius; };
    for (const Finger& finger : m_fingers) {
        if (finger.released || !inside(finger.position)) continue;
        press.held = true;
        press.pressed = press.pressed || finger.pressed;
    }
    if (m_fingers.empty() && inside(m_pointer)) {
        press.hovered = true;
        press.held = m_down;
        press.pressed = m_wentDown;
    }
    if (press.held || press.hovered) m_overUi = true;
    Canvas& c = *m_canvas;
    const float ink = c.u(m_theme.inkWidth);
    c.circle(center, radius + ink, m_theme.ink);
    c.circle(center, radius,
             press.held ? m_theme.accent : Color(m_theme.accent.r, m_theme.accent.g, m_theme.accent.b, 0.35f));
    c.text(center, {.font = m_theme.font, .size = radius * 0.6f, .color = press.held ? m_theme.ink : m_theme.text},
           label, {0.5f, 0.5f});
    return press;
}

float Ui::presence(std::string_view id, bool shown, float seconds) {
    float& value = m_presence[hash(id)];
    const float step = seconds > 0.0f ? m_dt / seconds : 1.0f;
    value = std::clamp(value + (shown ? step : -step), 0.0f, 1.0f);
    return value;
}

std::optional<glm::vec2> Ui::point(std::string_view id) const {
    const auto found = m_points.find(hash(id));
    if (found == m_points.end()) return std::nullopt;
    return found->second;
}

} // namespace quench::ui
