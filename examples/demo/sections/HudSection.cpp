// Game UI: quench_ui (docs/ui.md) over a small scene. A HUD of the comic kind: three meters, a radio line whose
// speaker pops in as a sticker (a face drawn from shapes, turned a little) with a speech bubble, an inventory whose
// items are dragged between slots or carried with the gamepad's focus, tips, buttons, and on a touch screen a stick
// under the left thumb. Everything is drawn behind the demo's own windows, which take the mouse first.

#include "Demo.h"

#include <quench/ui/Ui.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
using ui::Canvas;
using ui::Color;
using ui::Rect;
using ui::rgb;

constexpr int kColumns = 4, kRows = 3, kSlots = kColumns * kRows;
constexpr std::array<const char*, 4> kLines{
    "Kai, do you copy? Good. Try not to touch anything that glows.",
    "The door ahead is sealed from my side. Give me a minute.",
    "If you say \"be careful\" once more, I will come out and carefully strangle you.",
    "Module secured. Nobody saw a thing. Probably.",
};

struct ItemKind {
    const char* name;
    const char* tip;
    Color color;
    int sides; // of its icon: 0 a disc
};
constexpr std::array<ItemKind, 5> kItems{
    ItemKind{"Plasma cutter", "Plasma cutter\nMelee. A hit vents the suit's heat.", rgb(0xffb23a), 3},
    ItemKind{"Pulse rifle", "Pulse rifle\nRanged. Every shot heats the suit.", rgb(0x1ad9cc), 4},
    ItemKind{"Medkit", "Medkit\nRestores 25 health.", rgb(0x3df07a), 0},
    ItemKind{"EMP chip", "EMP chip\nStuns machines. Fits a weapon's socket.", rgb(0x4d7dff), 6},
    ItemKind{"Scrap", "Scrap\nThe workbench rerolls one affix for it.", rgb(0xb8bec9), 5},
};

class HudSection final : public Section {
public:
    HudSection() {
        camera.target = glm::vec3(0.0f, 0.8f, 0.0f);
        camera.distance = 11.0f;
        camera.yaw = glm::radians(30.0f);
        camera.pitch = glm::radians(24.0f);
        m_font = ui::addFont("demo-hud", {"fonts/Manrope-Bold.ttf", "fonts/Manrope-Bold-Cyrillic.ttf"});
        m_ui.theme().font = m_font;
        resetItems();
    }

    std::string_view id() const override { return "hud"; }
    std::string_view title() const override { return "Game UI"; }
    std::string_view about() const override {
        return "A game's interface from quench_ui: meters, a radio line with a sticker face and a speech bubble, an "
               "inventory (drag an item to another slot; with Focus on, the D-pad or the arrows move and A or Return "
               "carries), tips, buttons. Hide the demo's windows (H) to see all of it.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(30.0f), 30.0f));
        const Result<MeshId> box = addMesh(s, boxMesh(glm::vec3(0.5f)));
        const Result<MeshId> ball = addMesh(s, sphereMesh(0.5f, 24, 48));
        for (const Result<MeshId>* m : {&ground, &box, &ball})
            if (!*m) return Error{m->error()};
        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.30f, 0.32f, 0.36f),
                                                      glm::vec3(0.22f, 0.24f, 0.28f), 2.0f)});
        const MaterialId orange = s.addMaterial({.baseColor = glm::vec4(0.9f, 0.4f, 0.1f, 1.0f), .roughness = 0.5f});
        const MaterialId dark = s.addMaterial({.baseColor = glm::vec4(0.12f, 0.13f, 0.16f, 1.0f), .roughness = 0.6f});
        s.addInstance({.mesh = *box, .material = dark, .transform = placed({0.0f, 0.55f, 0.0f}, {0.8f, 1.1f, 0.5f})});
        s.addInstance({.mesh = *box, .material = orange, .transform = placed({0.0f, 1.2f, 0.0f}, {1.1f, 0.2f, 0.55f})});
        m_orb = s.addInstance(
            {.mesh = *ball,
             .material = s.addMaterial({.baseColor = glm::vec4(0.1f, 0.85f, 0.8f, 1.0f), .roughness = 0.3f}),
             .transform = placed({2.5f, 0.5f, 1.0f})});
        m_sun = s.addLight({.type = LightType::Directional});
        setDaylight(s, m_sun, 30.0f, 60.0f);
        return {};
    }

    void update(Context& c, double frameDt) override {
        (void)c;
        m_dt = float(frameDt);
        m_time += frameDt;
        m_scene->setTransform(m_orb, placed({2.5f, 0.5f + 0.25f * float(std::sin(m_time * 2.0)), 1.0f}));
        // The meters live a little: heat climbs and vents, the shield comes back.
        m_heat = 0.5f + 0.5f * float(std::sin(m_time * 1.3));
        m_shield = std::min(1.0f, m_shield + m_dt * 0.25f);
        m_lineAge += m_dt;
    }

    void controls(Context& c) override {
        (void)c;
        if (ImGui::Button("Next line")) nextLine();
        ImGui::SameLine();
        if (ImGui::Button("Hurt")) hurt();
        ImGui::Checkbox("Focus (D-pad, arrows, A / Return)", &m_navigation);
        ImGui::Checkbox("Touch stick", &m_touch);
        if (ImGui::Button("Reset the items")) resetItems();
    }

    void stats(Context& c) override {
        (void)c;
        ImGui::TextWrapped("%d button presses, %d items moved; line %d of %d", m_presses, m_moves, m_line + 1,
                           int(kLines.size()));
        ImGui::TextWrapped("Stick %.2f, %.2f", double(m_stick.x), double(m_stick.y));
    }

    // The interface itself, behind the demo's windows.
    void overlay(Context& c) override {
        const glm::vec2 window = windowSize(c);
        const app::WindowRect area = c.app.safeArea();
        const Rect safe{{float(area.x), float(area.y)}, {float(area.x + area.width), float(area.y + area.height)}};
        Canvas canvas(ImGui::GetBackgroundDrawList(), window, safe, std::max(window.y / 720.0f, 0.6f));
        m_ui.setNavigation(m_navigation);
        m_ui.begin(c.app, canvas, m_dt);
        const float u = canvas.unit();
        const ui::TextStyle label{.font = m_font,
                                  .size = 15.0f * u,
                                  .color = rgb(0xffffff),
                                  .outline = 1.5f * u,
                                  .outlineColor = rgb(0x0b0d12)};

        // ---- the meters, bottom left
        struct Meter {
            const char* name;
            float fill;
            Color color;
        };
        const Meter meters[3] = {{"Health", m_health, rgb(0xff4d5e)},
                                 {"Shield", m_shield, rgb(0x1ad9cc)},
                                 {"Heat", m_heat, m_heat > 0.85f ? rgb(0xff5a3c) : rgb(0xffb23a)}};
        float y = safe.max.y - 26.0f * u * 3.0f - 70.0f * u;
        for (const Meter& meter : meters) {
            canvas.text({safe.min.x + 22.0f * u, y - 2.0f * u}, label, meter.name);
            canvas.bar(Rect::at({safe.min.x + 96.0f * u, y}, {210.0f * u, 14.0f * u}), meter.fill, meter.color);
            y += 26.0f * u;
        }

        // ---- the radio line, top middle: the speaker's sticker pops in, the bubble follows
        const float shown = m_ui.presence("line", m_lineAge < 6.0f, 0.3f);
        if (shown > 0.0f) {
            const float pop = ui::easeOutBack(shown);
            const glm::vec2 face(safe.min.x + (safe.size().x * 0.5f) - 250.0f * u, safe.min.y + 96.0f * u);
            {
                const Canvas::Scope turned = canvas.transform(face, glm::radians(-7.0f), pop, shown);
                drawFace(canvas, face, 62.0f * u);
            }
            const Rect body = Rect::at({face.x + 78.0f * u, face.y - 46.0f * u}, {430.0f * u, 92.0f * u});
            {
                const Canvas::Scope grown = canvas.transform(body.min + glm::vec2(0.0f, body.size().y * 0.5f), 0.0f,
                                                             ui::easeOutCubic(shown), shown);
                canvas.bubble(body, face + glm::vec2(46.0f * u, 10.0f * u), rgb(0xfbfbf6), rgb(0x0b0d12), 16.0f * u,
                              3.0f * u);
                canvas.halftone({{body.max.x - 150.0f * u, body.min.y + 4.0f * u}, body.max - glm::vec2(6.0f * u)},
                                rgb(0x1ad9cc, 0.35f), 9.0f * u, 0.0f, 0.8f);
                canvas.text(body.min + glm::vec2(18.0f * u, 14.0f * u),
                            {.font = m_font, .size = 17.0f * u, .color = rgb(0x0b0d12)}, kLines[std::size_t(m_line)],
                            {0.0f, 0.0f}, body.size().x - 36.0f * u);
                // A tag turned against the bubble's edge.
                const glm::vec2 tag(body.max.x - 40.0f * u, body.min.y);
                const Canvas::Scope tilted = canvas.transform(tag, glm::radians(6.0f));
                canvas.rect(Rect::around(tag, {74.0f * u, 24.0f * u}).grown(2.5f * u), rgb(0x0b0d12), 7.0f * u);
                canvas.rect(Rect::around(tag, {74.0f * u, 24.0f * u}), rgb(0xff5a3c), 5.0f * u);
                canvas.text(tag, {.font = m_font, .size = 13.0f * u, .color = rgb(0xffffff)}, "RADIO", {0.5f, 0.5f});
            }
        }

        // ---- the inventory, right
        const float cell = 62.0f * u, gap = 10.0f * u;
        const glm::vec2 gridSize(float(kColumns) * cell + float(kColumns - 1) * gap,
                                 float(kRows) * cell + float(kRows - 1) * gap);
        const glm::vec2 grid(safe.max.x - gridSize.x - 34.0f * u, safe.min.y + (safe.size().y - gridSize.y) * 0.5f);
        m_ui.panel({grid - glm::vec2(16.0f * u, 44.0f * u), grid + gridSize + glm::vec2(16.0f * u, 62.0f * u)});
        canvas.text(grid - glm::vec2(0.0f, 34.0f * u), {.font = m_font, .size = 18.0f * u, .color = rgb(0xf2f4f8)},
                    "Inventory");
        for (int i = 0; i < kSlots; ++i) {
            const Rect slot =
                Rect::at(grid + glm::vec2(float(i % kColumns), float(i / kColumns)) * (cell + gap), glm::vec2(cell));
            const int item = m_slots[std::size_t(i)];
            const ui::SlotEvent event = m_ui.slot(i, slot, item >= 0);
            if (item >= 0) m_ui.tip(kItems[std::size_t(item)].tip);
            if (event.droppedFrom >= 0) {
                std::swap(m_slots[std::size_t(i)], m_slots[std::size_t(event.droppedFrom)]);
                ++m_moves;
            }
            if (item >= 0 && !event.dragging) drawItem(canvas, slot.center(), cell * 0.3f, item);
        }
        // ---- buttons under it
        const float buttonY = grid.y + gridSize.y + 16.0f * u;
        const float buttonW = (gridSize.x - gap) * 0.5f;
        if (m_ui.button("next", Rect::at({grid.x, buttonY}, {buttonW, 34.0f * u}), "Next line").pressed) nextLine();
        m_ui.tip("The speaker says the next line.");
        if (m_ui.button("hurt", Rect::at({grid.x + buttonW + gap, buttonY}, {buttonW, 34.0f * u}), "Hurt").pressed)
            hurt();
        m_ui.tip("The shield takes the damage first.");
        // The item in the air, over the slots.
        if (m_ui.dragged() >= 0 && m_slots[std::size_t(m_ui.dragged())] >= 0) {
            const Canvas::Scope lifted = canvas.transform(m_ui.dragPoint(), glm::radians(-8.0f), 1.2f);
            drawItem(canvas, m_ui.dragPoint(), cell * 0.3f, m_slots[std::size_t(m_ui.dragged())]);
        }

        // ---- a stick for the left thumb
        m_stick = glm::vec2(0.0f);
        if (m_touch)
            m_stick = m_ui.stick("move",
                                 {{safe.min.x, safe.min.y + safe.size().y * 0.35f},
                                  {safe.min.x + safe.size().x * 0.4f, safe.max.y - 150.0f * u}},
                                 54.0f * u);
        m_tip = m_ui.tipShown();
        m_ui.end();
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "ui_presses") return double(m_presses);
        if (name == "ui_moves") return double(m_moves);
        if (name == "ui_line") return double(m_line);
        if (name == "ui_tip") return m_tip ? 1.0 : 0.0;
        if (name == "ui_dragged") return double(m_ui.dragged());
        if (name == "ui_health") return double(m_health);
        if (name == "ui_shield") return double(m_shield);
        if (name == "ui_stick_x") return double(m_stick.x);
        if (name == "ui_stick_y") return double(m_stick.y);
        if (name == "ui_over") return m_ui.pointerOverUi() ? 1.0 : 0.0;
        if (name.starts_with("ui_slot")) {
            const std::optional<float> index = parseFloat(name.substr(7));
            if (index && *index >= 0.0f && *index < float(kSlots)) return double(m_slots[std::size_t(*index)]);
        }
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<bool> flag = parseBool(value);
        if (name == "ui_focus" && flag) {
            m_navigation = *flag;
        } else if (name == "ui_touch" && flag) {
            m_touch = *flag;
        } else if (name == "ui_reset" && flag) {
            resetItems();
            m_presses = m_moves = 0;
            m_line = 0;
            m_lineAge = 0.0f;
            m_health = m_shield = 1.0f;
        } else {
            return false;
        }
        return true;
    }

    // `@hud:<widget id>`: the middle of a widget of the last frame (`@hud:next`, `@hud:slot:5`).
    std::optional<glm::vec2> point(const Context& c, std::string_view name) const override {
        (void)c;
        if (!name.starts_with("hud:")) return std::nullopt;
        return m_ui.point(name.substr(4));
    }

    void shutdown(Context& c) override {
        (void)c;
        m_scene.reset();
    }

private:
    void resetItems() {
        m_slots.fill(-1);
        m_slots[0] = 0;
        m_slots[1] = 1;
        m_slots[2] = 2;
        m_slots[5] = 3;
        m_slots[9] = 4;
        m_slots[10] = 4;
    }

    void nextLine() {
        ++m_presses;
        m_line = (m_line + 1) % int(kLines.size());
        m_lineAge = 0.0f;
        // The sticker goes and pops in again.
        m_ui.presence("line", false, 0.0f);
    }

    void hurt() {
        ++m_presses;
        const float taken = std::min(m_shield, 0.4f);
        m_shield -= taken;
        m_health = std::max(0.1f, m_health - (0.4f - taken));
    }

    // An item's icon: a disc or a polygon of its kind's colour with an ink edge and a highlight.
    static void drawItem(Canvas& canvas, glm::vec2 center, float radius, int item) {
        const ItemKind& kind = kItems[std::size_t(item)];
        const float ink = radius * 0.16f;
        if (kind.sides == 0) {
            canvas.circle(center, radius + ink, rgb(0x0b0d12));
            canvas.circle(center, radius, kind.color);
        } else {
            std::vector<glm::vec2> points;
            for (int i = 0; i < kind.sides; ++i) {
                const float a = 6.2831853f * float(i) / float(kind.sides) - 1.5708f;
                points.push_back(center + glm::vec2(std::cos(a), std::sin(a)) * radius * 1.12f);
            }
            canvas.polyline(points, rgb(0x0b0d12), ink * 2.0f, true);
            canvas.polygon(points, kind.color);
        }
        canvas.circle(center + glm::vec2(-radius * 0.3f, -radius * 0.32f), radius * 0.2f, rgb(0xffffff, 0.7f));
    }

    // The speaker: a face cut out as a sticker, from ellipses and polygons.
    static void drawFace(Canvas& canvas, glm::vec2 c, float r) {
        const auto ellipse = [&](glm::vec2 center, glm::vec2 radii, float from = 0.0f, float to = 6.2831853f) {
            std::vector<glm::vec2> points;
            for (int i = 0; i <= 28; ++i) {
                const float a = from + (to - from) * float(i) / 28.0f;
                points.push_back(center + glm::vec2(std::cos(a) * radii.x, std::sin(a) * radii.y));
            }
            return points;
        };
        const Color inkColor = rgb(0x0b0d12);
        const float ink = r * 0.07f;
        // The cut: the head and the shoulders together.
        std::vector<glm::vec2> cut = ellipse(c + glm::vec2(0.0f, -r * 0.1f), {r * 0.95f, r * 1.05f}, 2.6f, 6.83f);
        cut.push_back(c + glm::vec2(r * 1.15f, r * 1.05f));
        cut.push_back(c + glm::vec2(-r * 1.15f, r * 1.05f));
        canvas.sticker(cut, r * 0.16f);
        // The coat and its collar.
        const glm::vec2 coat[5] = {c + glm::vec2(-r * 1.1f, r * 1.02f), c + glm::vec2(-r * 0.75f, r * 0.55f),
                                   c + glm::vec2(0.0f, r * 0.78f), c + glm::vec2(r * 0.75f, r * 0.55f),
                                   c + glm::vec2(r * 1.1f, r * 1.02f)};
        canvas.polygon(coat, rgb(0xeef1f5));
        canvas.polyline(coat, inkColor, ink, false);
        // The head, the hair over it, the ink around both.
        const std::vector<glm::vec2> head = ellipse(c, {r * 0.66f, r * 0.8f});
        canvas.polygon(head, rgb(0xe8b99a));
        std::vector<glm::vec2> hair = ellipse(c + glm::vec2(0.0f, -r * 0.12f), {r * 0.74f, r * 0.82f}, 3.0f, 6.42f);
        hair.push_back(c + glm::vec2(r * 0.3f, -r * 0.3f));
        hair.push_back(c + glm::vec2(-r * 0.5f, -r * 0.36f));
        canvas.polygon(hair, rgb(0x2a2f3a));
        canvas.polyline(head, inkColor, ink, true);
        // Glasses with a cracked lens, the eyes behind them, a dry mouth.
        for (const float side : {-1.0f, 1.0f}) {
            const glm::vec2 lens = c + glm::vec2(side * r * 0.3f, -r * 0.05f);
            canvas.circle(lens, r * 0.22f, rgb(0xffffff, 0.55f));
            canvas.ring(lens, r * 0.22f, inkColor, ink);
            canvas.circle(lens + glm::vec2(side * r * 0.02f, r * 0.02f), r * 0.06f, inkColor);
        }
        canvas.line(c + glm::vec2(-r * 0.08f, -r * 0.05f), c + glm::vec2(r * 0.08f, -r * 0.05f), inkColor, ink);
        canvas.line(c + glm::vec2(r * 0.2f, -r * 0.2f), c + glm::vec2(r * 0.38f, r * 0.08f), inkColor, ink * 0.5f);
        canvas.line(c + glm::vec2(-r * 0.2f, r * 0.42f), c + glm::vec2(r * 0.16f, r * 0.38f), inkColor, ink);
        // The headset: a band, a microphone with a teal light.
        canvas.line(c + glm::vec2(r * 0.66f, r * 0.05f), c + glm::vec2(r * 0.42f, r * 0.5f), inkColor, ink);
        canvas.circle(c + glm::vec2(r * 0.4f, r * 0.52f), r * 0.09f, rgb(0x1ad9cc));
        canvas.ring(c + glm::vec2(r * 0.4f, r * 0.52f), r * 0.09f, inkColor, ink * 0.6f);
    }

    std::optional<Scene> m_scene;
    InstanceId m_orb;
    LightId m_sun;
    ui::Ui m_ui;
    ui::FontId m_font = 0;
    std::array<int, kSlots> m_slots{};
    double m_time = 0.0;
    float m_dt = 0.0f;
    float m_health = 1.0f, m_shield = 1.0f, m_heat = 0.0f;
    float m_lineAge = 0.0f;
    int m_line = 0, m_presses = 0, m_moves = 0;
    bool m_navigation = false, m_touch = true, m_tip = false;
    glm::vec2 m_stick{0.0f};
};

} // namespace

std::unique_ptr<Section> makeHudSection() { return std::make_unique<HudSection>(); }

} // namespace demo
