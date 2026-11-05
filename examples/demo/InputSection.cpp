// Input: the engine's named actions over keyboard, mouse, gamepad and touch, shown live, driving a small character.
// Keys W A S D or the left stick or a finger on the left half of the screen (a virtual stick) move it, J, gamepad A
// or a finger on the right half jump, F, gamepad X, a click or a tap fire.

#include "Demo.h"

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
using app::Binding;
using app::GamepadAxis;
using app::GamepadButton;

constexpr float kArena = 9.0f;
constexpr float kHalf = 0.35f;
constexpr float kStickRadius = 50.0f; // points of finger travel for full deflection
constexpr std::size_t kShots = 8;

class InputSection final : public Section {
public:
    InputSection() {
        camera.target = glm::vec3(0.0f, 0.8f, 0.0f);
        camera.distance = 9.0f;
        camera.yaw = 0.0f;
        camera.pitch = glm::radians(32.0f);
    }

    std::string_view id() const override { return "input"; }
    std::string_view title() const override { return "Input"; }
    std::string_view about() const override {
        return "Named actions over every device. Move: W A S D, left stick, or a finger on the left half. Jump: J, "
               "gamepad A, or a finger on the right half. Fire: F, gamepad X, click or tap. Run: Left Shift or the "
               "left stick button.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(kArena + 1.0f), kArena + 1.0f));
        const Result<MeshId> box = addMesh(s, boxMesh(glm::vec3(kHalf)));
        const Result<MeshId> post = addMesh(s, boxMesh(glm::vec3(0.15f, 0.6f, 0.15f)));
        const Result<MeshId> shot = addMesh(s, sphereMesh(0.12f, 12, 24));
        for (const Result<MeshId>* m : {&ground, &box, &post, &shot})
            if (!*m) return Error{m->error()};
        s.addInstance(
            {.mesh = *ground,
             .material = addCheckerMaterial(c, s, glm::vec3(0.45f, 0.5f, 0.55f), glm::vec3(0.3f, 0.34f, 0.38f), 1.0f)});
        const MaterialId postMaterial = s.addMaterial({.baseColor = glm::vec4(0.7f, 0.7f, 0.72f, 1.0f)});
        for (int i = 0; i < 4; ++i) {
            const float a = glm::radians(45.0f + 90.0f * float(i));
            s.addInstance({.mesh = *post,
                           .material = postMaterial,
                           .transform = placed({std::sin(a) * 6.0f, 0.6f, std::cos(a) * 6.0f})});
        }
        m_player = s.addInstance(
            {.mesh = *box,
             .material = s.addMaterial({.baseColor = glm::vec4(0.9f, 0.4f, 0.1f, 1.0f), .roughness = 0.4f})});
        const MaterialId glow = s.addMaterial({.baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
                                               .emissive = glm::vec3(2.0f, 6.0f, 8.0f),
                                               .castsShadows = false});
        for (Shot& sh : m_shots) sh.instance = s.addInstance({.mesh = *shot, .material = glow, .visible = false});
        s.addLight({.type = LightType::Directional,
                    .color = glm::vec3(5.0f, 4.8f, 4.4f),
                    .direction = -glm::normalize(glm::vec3(-0.3f, 0.9f, 0.5f))});

        app::Input& input = c.app.input();
        m_left = input.addAction("move_left", {*Binding::key("A"), Binding::gamepadAxis(GamepadAxis::LeftX, false)});
        m_right = input.addAction("move_right", {*Binding::key("D"), Binding::gamepadAxis(GamepadAxis::LeftX, true)});
        m_forward =
            input.addAction("move_forward", {*Binding::key("W"), Binding::gamepadAxis(GamepadAxis::LeftY, false)});
        m_back = input.addAction("move_back", {*Binding::key("S"), Binding::gamepadAxis(GamepadAxis::LeftY, true)});
        m_jump = input.addAction("jump", {*Binding::key("J"), Binding::gamepad(GamepadButton::South)});
        m_fire = input.addAction("fire", {*Binding::key("F"), Binding::gamepad(GamepadButton::West)});
        m_run = input.addAction("run", {*Binding::key("Left Shift"), Binding::gamepad(GamepadButton::LeftStick)});
        m_actions = {m_left, m_right, m_forward, m_back, m_jump, m_fire, m_run};
        place(0.0f);
        return {};
    }

    void enter(Context& c) override {
        (void)c;
        m_stickFinger.reset();
    }

    void fixedStep(Context& c, double dt) override {
        (void)c;
        const float step = float(dt);
        m_previous = m_position;
        // Camera-relative movement on the ground.
        const glm::vec3 forward(-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw));
        const glm::vec3 right(std::cos(camera.yaw), 0.0f, -std::sin(camera.yaw));
        glm::vec2 move = m_move;
        if (glm::length(move) > 1.0f) move = glm::normalize(move);
        const float speed = m_running ? 8.0f : 4.0f;
        m_position += (right * move.x + forward * move.y) * speed * step;
        m_position.x = std::clamp(m_position.x, -kArena, kArena);
        m_position.z = std::clamp(m_position.z, -kArena, kArena);
        if (m_jumpQueued && m_position.y <= 0.0f) {
            m_velocityY = 7.0f;
            ++m_jumps;
        }
        m_jumpQueued = false;
        m_velocityY -= 20.0f * step;
        m_position.y = std::max(0.0f, m_position.y + m_velocityY * step);
        if (m_position.y <= 0.0f) m_velocityY = 0.0f;
        for (Shot& sh : m_shots) {
            if (sh.life <= 0.0f) continue;
            sh.previous = sh.position;
            sh.position += sh.velocity * step;
            sh.life -= step;
        }
    }

    void update(Context& c, double frameDt) override {
        (void)frameDt;
        const app::Input& input = c.app.input();
        for (std::size_t i = 0; i < m_actions.size(); ++i)
            if (input.pressed(m_actions[i])) ++m_presses[i];
        updateTouch(c);
        m_move = glm::vec2(input.axis(m_left, m_right), input.axis(m_back, m_forward)) + m_stick;
        m_running = input.down(m_run);
        if (input.pressed(m_jump)) m_jumpQueued = true;
        if (input.pressed(m_fire)) fire(std::nullopt);
        place(float(c.app.alpha()));
        camera.target = glm::mix(m_previous, m_position, float(c.app.alpha())) + glm::vec3(0.0f, 0.8f, 0.0f);
    }

    void click(Context& c, glm::vec2 point) override {
        glm::vec3 origin, direction;
        camera.ray(point, glm::vec2(float(c.app.windowWidth()), float(c.app.windowHeight())), origin, direction);
        if (direction.y >= -1e-3f) {
            fire(std::nullopt);
            return;
        }
        fire(origin + direction * (-origin.y / direction.y));
    }

    bool orbitWithFingers() const override { return false; }

    void ui(Context& c) override {
        const app::Input& input = c.app.input();
        if (ImGui::BeginTable("actions", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Action");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Down");
            ImGui::TableSetupColumn("Times");
            ImGui::TableHeadersRow();
            static constexpr std::array<const char*, 7> kNames{"move_left", "move_right", "move_forward", "move_back",
                                                               "jump",      "fire",       "run"};
            for (std::size_t i = 0; i < m_actions.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kNames[i]);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", double(input.value(m_actions[i])));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(input.down(m_actions[i]) ? "yes" : "");
                ImGui::TableNextColumn();
                ImGui::Text("%d", m_presses[i]);
            }
            ImGui::EndTable();
        }
        std::string keys;
        for (int code = 0; code < SDL_SCANCODE_COUNT; ++code)
            if (input.keyDown(code)) {
                if (!keys.empty()) keys += ", ";
                keys += SDL_GetScancodeName(SDL_Scancode(code));
            }
        ImGui::Text("Keys: %s", keys.empty() ? "-" : keys.c_str());
        const glm::vec2 mouse = input.mousePosition();
        ImGui::Text("Mouse %.0f, %.0f  %s%s%s%s", double(mouse.x), double(mouse.y),
                    input.mouseDown(app::MouseButton::Left) ? "L " : "",
                    input.mouseDown(app::MouseButton::Middle) ? "M " : "",
                    input.mouseDown(app::MouseButton::Right) ? "R " : "", input.mouseFromTouch() ? "(touch)" : "");
        if (input.gamepadConnected()) {
            ImGui::Text("Gamepad: %s", input.gamepadName().c_str());
            static constexpr std::array<const char*, 6> kAxes{"LX", "LY", "RX", "RY", "LT", "RT"};
            for (std::size_t a = 0; a < kAxes.size(); ++a) {
                const float v = input.gamepadAxis(GamepadAxis(a));
                const std::string label = std::string(kAxes[a]) + " " + std::to_string(int(std::lround(v * 100.0f)));
                ImGui::ProgressBar(a < 4 ? v * 0.5f + 0.5f : v, ImVec2(ImGui::GetFontSize() * 4.5f, 0.0f),
                                   label.c_str());
                if (a % 3 != 2) ImGui::SameLine();
            }
        } else {
            ImGui::TextUnformatted("Gamepad: none");
        }
        ImGui::Text("Fingers: %zu, stick %.2f, %.2f", input.fingers().size(), double(m_stick.x), double(m_stick.y));
        ImGui::TextWrapped("Position %.1f, %.1f; jumps %d, shots %d", double(m_position.x), double(m_position.z),
                           m_jumps, m_shotCount);

        // Fingers and the virtual stick on top of everything.
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const float em = ImGui::GetFontSize();
        for (const app::Finger& f : input.fingers())
            draw->AddCircle(ImVec2(f.position.x, f.position.y), 1.6f * em, IM_COL32(255, 255, 255, 180), 0, 2.0f);
        if (m_stickFinger) {
            draw->AddCircle(ImVec2(m_stickOrigin.x, m_stickOrigin.y), kStickRadius, IM_COL32(255, 200, 80, 160), 0,
                            2.0f);
            const glm::vec2 knob = m_stickOrigin + m_stick * glm::vec2(1.0f, -1.0f) * kStickRadius;
            draw->AddCircleFilled(ImVec2(knob.x, knob.y), 0.8f * em, IM_COL32(255, 200, 80, 200));
        }
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "player_x") return double(m_position.x);
        if (name == "player_y") return double(m_position.y);
        if (name == "player_z") return double(m_position.z);
        if (name == "jumps") return double(m_jumps);
        if (name == "shots") return double(m_shotCount);
        if (name == "stick_x") return double(m_stick.x);
        if (name == "stick_y") return double(m_stick.y);
        return std::nullopt;
    }

    void shutdown(Context& c) override {
        (void)c;
        m_scene.reset();
    }

private:
    struct Shot {
        InstanceId instance;
        glm::vec3 position{0.0f}, previous{0.0f}, velocity{0.0f};
        float life = 0.0f;
    };

    // The first finger on the left half (not on a window) is a virtual stick; a finger on the right half jumps.
    void updateTouch(Context& c) {
        const app::Input& input = c.app.input();
        const float half = float(c.app.windowWidth()) * 0.5f;
        m_stick = glm::vec2(0.0f);
        bool stickDown = false;
        for (const app::Finger& f : input.fingers()) {
            if (f.pressed && !input.uiCapturesMouse()) {
                if (f.position.x < half && !m_stickFinger) {
                    m_stickFinger = f.id;
                    m_stickOrigin = f.position;
                } else if (f.position.x >= half) {
                    m_jumpQueued = true;
                }
            }
            if (m_stickFinger && f.id == *m_stickFinger) {
                if (f.released) continue;
                stickDown = true;
                glm::vec2 offset = (f.position - m_stickOrigin) / kStickRadius;
                if (glm::length(offset) > 1.0f) offset = glm::normalize(offset);
                m_stick = glm::vec2(offset.x, -offset.y);
            }
        }
        if (!stickDown) m_stickFinger.reset();
    }

    void fire(std::optional<glm::vec3> toward) {
        Shot& sh = m_shots[std::size_t(m_shotCount) % kShots];
        ++m_shotCount;
        const glm::vec3 start = m_position + glm::vec3(0.0f, kHalf + 0.2f, 0.0f);
        glm::vec3 direction(-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw));
        if (toward) {
            const glm::vec3 d = *toward + glm::vec3(0.0f, 0.3f, 0.0f) - start;
            if (glm::length(d) > 1e-3f) direction = glm::normalize(d);
        }
        sh.position = sh.previous = start;
        sh.velocity = direction * 14.0f;
        sh.life = 1.2f;
        m_scene->setTransform(sh.instance, placed(start), true);
    }

    void place(float alpha) {
        const glm::vec3 p = glm::mix(m_previous, m_position, alpha) + glm::vec3(0.0f, kHalf, 0.0f);
        m_scene->setTransform(m_player, placed(p));
        for (Shot& sh : m_shots) {
            m_scene->setVisible(sh.instance, sh.life > 0.0f);
            if (sh.life > 0.0f) m_scene->setTransform(sh.instance, placed(glm::mix(sh.previous, sh.position, alpha)));
        }
    }

    std::optional<Scene> m_scene;
    InstanceId m_player;
    std::array<Shot, kShots> m_shots{};
    app::ActionId m_left = 0, m_right = 0, m_forward = 0, m_back = 0, m_jump = 0, m_fire = 0, m_run = 0;
    std::array<app::ActionId, 7> m_actions{};
    std::array<int, 7> m_presses{};
    glm::vec3 m_position{0.0f}, m_previous{0.0f};
    float m_velocityY = 0.0f;
    glm::vec2 m_move{0.0f}, m_stick{0.0f};
    std::optional<std::uint64_t> m_stickFinger;
    glm::vec2 m_stickOrigin{0.0f};
    bool m_running = false, m_jumpQueued = false;
    int m_jumps = 0, m_shotCount = 0;
};

} // namespace

std::unique_ptr<Section> makeInputSection() { return std::make_unique<InputSection>(); }

} // namespace demo
