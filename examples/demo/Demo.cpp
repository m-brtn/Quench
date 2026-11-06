// The engine demo: a menu of sections, each a small interactive scene of one engine feature with its controls and the
// frame, CPU and GPU times. Sections: Demo.h; scripted checks: examples/demo/scenarios; docs/demo.md.

#include "Demo.h"

#include <quench/app/Scenario.h>
#include <quench/core/Log.h>

#include <demo_checker_shaders.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace {

using namespace quench;
using app::App;
using app::Binding;
using app::GamepadAxis;
using app::GamepadButton;
using demo::Section;

class Demo final : public app::Game {
public:
    explicit Demo(std::string startSection) : m_startSection(std::move(startSection)) {}

    Result<void> init(App& app) override {
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        // The demo's keys keep working while a window has the keyboard focus (they avoid the navigation keys).
        io.ConfigNavCaptureKeyboard = false;

        Result<render::Renderer> renderer = render::Renderer::create(app.device());
        if (!renderer) return Error{std::format("renderer: {}", renderer.error())};
        m_renderer.emplace(std::move(*renderer));
        render::SurfaceId checker{};
        Result<render::SurfaceId> surface = m_renderer->addSurface({
            .vertexShader = shaders::demo_checker.find("surface_vs"),
            .fragmentShader = shaders::demo_checker.find("surface_fs"),
            .name = "checker",
        });
        if (surface)
            checker = *surface;
        else
            log::warning("demo", "checker surface: {}; grounds use the standard surface", surface.error());
        m_context.emplace(demo::Context{app, *m_renderer, checker});

        m_sections.push_back(demo::makeMaterialsSection());
        m_sections.push_back(demo::makeShadowsSection());
        m_sections.push_back(demo::makeSkySection());
        m_sections.push_back(demo::makePostSection());
        m_sections.push_back(demo::makeHdrSection());
        m_sections.push_back(demo::makePhysicsSection());
        m_sections.push_back(demo::makeInputSection());
        m_sections.push_back(demo::makeAssetsSection());
        m_sections.push_back(demo::makeAudioSection());
        m_sections.push_back(demo::makeAnimationSection());
        for (const std::unique_ptr<Section>& section : m_sections)
            if (Result<void> r = section->init(*m_context); !r)
                return Error{std::format("section {}: {}", section->id(), r.error())};

        app::Input& input = app.input();
        m_next = input.addAction("next_section", {*Binding::key("]"), *Binding::key("PageDown"),
                                                  Binding::gamepad(GamepadButton::RightShoulder)});
        m_prev = input.addAction("prev_section", {*Binding::key("["), *Binding::key("PageUp"),
                                                  Binding::gamepad(GamepadButton::LeftShoulder)});
        // Keys 1 to 9, then 0 for the tenth.
        for (std::size_t i = 0; i < m_sections.size() && i < 10; ++i)
            m_direct.push_back(
                input.addAction(std::format("section_{}", i + 1), {*Binding::key(std::to_string((i + 1) % 10))}));
        m_toggleUi = input.addAction("toggle_ui", {*Binding::key("H"), Binding::gamepad(GamepadButton::Start)});
        m_drag = input.addAction("orbit_drag",
                                 {Binding::mouse(app::MouseButton::Left), Binding::mouse(app::MouseButton::Right)});
        m_orbitLeft = input.addAction("orbit_left", {Binding::gamepadAxis(GamepadAxis::RightX, false)});
        m_orbitRight = input.addAction("orbit_right", {Binding::gamepadAxis(GamepadAxis::RightX, true)});
        m_orbitUp = input.addAction("orbit_up", {Binding::gamepadAxis(GamepadAxis::RightY, false)});
        m_orbitDown = input.addAction("orbit_down", {Binding::gamepadAxis(GamepadAxis::RightY, true)});
        m_zoomIn = input.addAction("zoom_in", {Binding::gamepadAxis(GamepadAxis::RightTrigger, true)});
        m_zoomOut = input.addAction("zoom_out", {Binding::gamepadAxis(GamepadAxis::LeftTrigger, true)});

        std::size_t start = 0;
        if (!m_startSection.empty()) {
            const std::optional<std::size_t> found = find(m_startSection);
            if (!found) return Error{std::format("--section: no section '{}' (--help lists them)", m_startSection)};
            start = *found;
        }
        select(start);
        log::info("demo", "{} sections, starting with '{}'", m_sections.size(), current().id());
        return {};
    }

    void fixedStep(App& app, double dt) override {
        (void)app;
        current().fixedStep(*m_context, dt);
    }

    void update(App& app, double frameDt) override {
        const app::Input& input = app.input();
        if (input.pressed(m_next)) select((m_current + 1) % m_sections.size());
        if (input.pressed(m_prev)) select((m_current + m_sections.size() - 1) % m_sections.size());
        for (std::size_t i = 0; i < m_direct.size(); ++i)
            if (input.pressed(m_direct[i])) select(i);
        if (input.pressed(m_toggleUi)) {
            m_showUi = !m_showUi;
            m_uiReappears = m_showUi;
        }
        updateCamera(app, float(frameDt));
        Section& section = current();
        section.update(*m_context, frameDt);
        if (!(m_renderer->settings() == section.settings)) m_renderer->setSettings(section.settings);
    }

    void ui(App& app) override {
        m_menuPoints.assign(m_sections.size(), std::nullopt);
        if (!m_showUi) return;
        const app::WindowRect safe = app.safeArea();
        const float em = ImGui::GetFontSize();
        const float margin = 0.6f * em;
        const float left = float(safe.x), top = float(safe.y);
        const float right = float(safe.x + safe.width), bottom = float(safe.y + safe.height);

        // Both windows collapse with their title arrow (H and Start are not there on a phone).
        ImGui::SetNextWindowPos(ImVec2(left + margin, top + margin), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.8f);
        if (ImGui::Begin("Quench Demo", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            for (std::size_t i = 0; i < m_sections.size(); ++i) {
                const std::string label = std::format("{}  {}", (i + 1) % 10, m_sections[i]->title());
                if (ImGui::Selectable(label.c_str(), i == m_current)) select(i);
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                m_menuPoints[i] = glm::vec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            }
            ImGui::Separator();
            ImGui::TextDisabled("Switch: [ ] PgUp PgDn LB RB");
            ImGui::TextDisabled("Hide: H, Start");
            timings(app);
        }
        const ImVec2 menuEnd(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                             ImGui::GetWindowPos().y + ImGui::GetWindowSize().y);
        ImGui::End();

        // The section's window at the top right, or under the menu where the screen is too narrow for both.
        Section& section = current();
        const float width = 18.0f * em;
        const float x = right - margin - width;
        const bool stacked = x < menuEnd.x + margin;
        const float y = stacked ? menuEnd.y + margin : top + margin;
        const float limit = stacked ? m_captionTop - margin : top + float(safe.height) * 0.72f;
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, std::max(limit - y, 6.0f * em)));
        ImGui::SetNextWindowBgAlpha(0.8f);
        const std::string title = std::format("{}###section", section.title());
        const bool open =
            ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
        if (open) {
            ImGui::PushItemWidth(8.0f * em);
            section.ui(*m_context);
            demo::effectsPanel(*m_context, section.settings);
            ImGui::PopItemWidth();
        }
        ImGui::End();
        hideNavCursorIfReappeared();

        // What the section shows, as a caption at the bottom; collapsing the section window hides it too.
        m_captionTop = bottom;
        if (!open) return;
        ImGui::SetNextWindowPos(ImVec2(left + float(safe.width) * 0.5f, bottom - margin), ImGuiCond_Always,
                                ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##about", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::PushTextWrapPos(std::min(34.0f * em, float(safe.width) - 4.0f * margin));
        ImGui::TextUnformatted(section.about().data(), section.about().data() + section.about().size());
        ImGui::PopTextWrapPos();
        m_captionTop = ImGui::GetWindowPos().y;
        ImGui::End();
    }

    // Windows that reappear take the focus, and after a key (H) ImGui shows its keyboard navigation frame on their
    // first item; the demo is driven by clicks and taps there, so the frame would only be noise.
    void hideNavCursorIfReappeared() {
        if (!m_uiReappears) return;
        ImGui::SetNavCursorVisible(false);
        m_uiReappears = false;
    }

    void render(App& app, app::Frame& frame) override {
        (void)app;
        Section& section = current();
        const float aspect = float(frame.width) / float(std::max(frame.height, 1u));
        const render::View view{.view = section.camera.view(),
                                .fovY = section.camera.fovFor(aspect),
                                .nearPlane = 0.1f,
                                .cut = m_cut,
                                .time = m_context->app.time()};
        m_renderer->render(section.scene(), view, frame.commands, frame.target, section.output(frame.output));
        m_targetWidth = frame.width;
        m_cut = false;
    }

    void shutdown(App& app) override {
        (void)app;
        for (const std::unique_ptr<Section>& section : m_sections) section->shutdown(*m_context);
        m_sections.clear();
        m_context.reset();
        m_renderer.reset();
    }

    std::optional<double> scenarioValue(std::string_view name) const override {
        const render::RenderStats& s = m_renderer->stats();
        const render::ActiveEffects& e = m_renderer->effects();
        if (name == "section") return double(m_current);
        if (name == "sections") return double(m_sections.size());
        if (name == "ui") return m_showUi ? 1.0 : 0.0;
        if (name == "aa") return double(int(e.antiAliasing));
        if (name == "cascades") return double(e.shadowCascades);
        if (name == "bloom") return e.bloom ? 1.0 : 0.0;
        if (name == "draws") return double(s.draws);
        if (name == "instances") return double(s.instances);
        if (name == "culled") return double(s.culled);
        if (name == "shadow_draws") return double(s.shadowDraws);
        if (name == "shadow_instances") return double(s.shadowInstances);
        if (name == "transmittance_draws") return double(s.transmittanceDraws);
        if (name == "translucent_shadows") return double(int(e.translucentShadows));
        if (name == "moved") return double(s.movedInstances);
        if (name == "lights") return double(s.lights);
        if (name == "skinned_instances") return double(s.skinnedInstances);
        if (name == "skinned_vertices") return double(s.skinnedVertices);
        if (name == "render_w") return double(s.renderWidth);
        if (name == "render_h") return double(s.renderHeight);
        if (name == "render_scale") return m_targetWidth > 0 ? double(s.renderWidth) / double(m_targetWidth) : 0.0;
        if (name == "camera_yaw") return double(glm::degrees(current().camera.yaw));
        if (name == "camera_pitch") return double(glm::degrees(current().camera.pitch));
        if (name == "camera_distance") return double(current().camera.distance);
        if (name == "clicks") return double(m_clicks);
        return current().value(name);
    }

    // `menu:<section id>`: the center of that menu entry while the menu is shown; other names: the section's.
    std::optional<glm::vec2> scenarioPoint(std::string_view name) const override {
        if (name.starts_with("menu:")) {
            for (std::size_t i = 0; i < m_sections.size() && i < m_menuPoints.size(); ++i)
                if (m_sections[i]->id() == name.substr(5)) return m_menuPoints[i];
            return std::nullopt;
        }
        return current().point(*m_context, name);
    }

    app::ScenarioExec scenarioCommand(App& app, const app::ScenarioCommand& command) override {
        (void)app;
        const auto fail = [&command](std::string_view why) {
            log::error("demo", "line {}: {}", command.line, why);
            return app::ScenarioExec::Failed;
        };
        if (command.op == "section") {
            if (command.args.size() != 1) return fail("section ID");
            const std::optional<std::size_t> found = find(command.args[0]);
            if (!found) return fail(std::format("no section '{}'", command.args[0]));
            select(*found);
            return app::ScenarioExec::Done;
        }
        if (command.op == "camera") {
            // camera YAW PITCH DISTANCE (degrees, m)
            if (command.args.size() != 3) return fail("camera YAW PITCH DISTANCE");
            const std::optional<float> yaw = demo::parseFloat(command.args[0]);
            const std::optional<float> pitch = demo::parseFloat(command.args[1]);
            const std::optional<float> distance = demo::parseFloat(command.args[2]);
            if (!yaw || !pitch || !distance) return fail("camera: numbers expected");
            demo::OrbitCamera& camera = current().camera;
            camera.yaw = glm::radians(*yaw);
            camera.pitch = glm::radians(*pitch);
            camera.distance = *distance;
            return app::ScenarioExec::Done;
        }
        if (command.op == "set") {
            if (command.args.size() != 2) return fail("set NAME VALUE");
            if (setRenderSetting(command.args[0], command.args[1]) ||
                current().set(*m_context, command.args[0], command.args[1]))
                return app::ScenarioExec::Done;
            return fail(std::format("set {} {}: unknown name or bad value", command.args[0], command.args[1]));
        }
        return app::ScenarioExec::Unknown;
    }

    std::string perfDetails(App& app) override {
        (void)app;
        const render::RenderStats& s = m_renderer->stats();
        const render::ActiveEffects& e = m_renderer->effects();
        std::string details = std::format(
            "section={} render={}x{} draws={} instances={} shadow_draws={} lights={} aa={} cascades={} skinned={}",
            current().id(), s.renderWidth, s.renderHeight, s.draws, s.instances, s.shadowDraws, s.lights,
            demo::antiAliasingName(e.antiAliasing), e.shadowCascades, s.skinnedInstances);
        const std::string section = current().perfDetails();
        if (!section.empty()) details += ' ' + section;
        return details;
    }

private:
    Section& current() { return *m_sections[m_current]; }
    const Section& current() const { return *m_sections[m_current]; }

    std::optional<std::size_t> find(std::string_view id) const {
        for (std::size_t i = 0; i < m_sections.size(); ++i)
            if (m_sections[i]->id() == id) return i;
        return std::nullopt;
    }

    void select(std::size_t index) {
        if (m_entered && index == m_current) return;
        if (m_entered) current().leave(*m_context);
        m_current = index;
        m_entered = true;
        current().enter(*m_context);
        m_renderer->setSettings(current().settings);
        m_cut = true; // another scene: no motion vectors from the previous one
        m_pressed = false;
    }

    // Generic `set` names: the renderer settings of the current section.
    bool setRenderSetting(std::string_view name, std::string_view value) {
        render::RenderSettings& s = current().settings;
        const std::optional<float> number = demo::parseFloat(value);
        const std::optional<bool> flag = demo::parseBool(value);
        if (name == "aa") {
            if (value == "none")
                s.antiAliasing = render::AntiAliasing::None;
            else if (value == "fxaa")
                s.antiAliasing = render::AntiAliasing::Fxaa;
            else if (value == "taa")
                s.antiAliasing = render::AntiAliasing::Taa;
            else
                return false;
        } else if (name == "tone") {
            if (value == "aces")
                s.toneCurve = render::ToneCurve::Aces;
            else if (value == "agx")
                s.toneCurve = render::ToneCurve::Agx;
            else
                return false;
        } else if (name == "cascades" && number) {
            s.shadowCascades = std::clamp(int(*number), 0, render::kMaxCascades);
        } else if (name == "shadow_size" && number) {
            s.shadowMapSize = std::uint32_t(std::clamp(*number, 256.0f, 4096.0f));
        } else if (name == "shadow_filter" && number) {
            s.shadowFilter = std::clamp(int(*number), 0, 2);
        } else if (name == "shadow_distance" && number) {
            s.shadowDistance = std::max(*number, 1.0f);
        } else if (name == "translucent_shadows") {
            if (value == "map")
                s.translucentShadows = render::TranslucentShadows::Map;
            else if (value == "dithered")
                s.translucentShadows = render::TranslucentShadows::Dithered;
            else
                return false;
        } else if (name == "scale" && number) {
            s.renderScale = std::clamp(*number, 0.25f, 1.0f);
        } else if (name == "bloom" && flag) {
            s.bloom = *flag;
        } else if (name == "bloom_strength" && number) {
            s.bloomStrength = std::max(*number, 0.0f);
        } else if (name == "sky" && flag) {
            s.sky = *flag;
        } else if (name == "dither" && flag) {
            s.dither = *flag;
        } else if (name == "sharpen" && number) {
            s.sharpen = std::clamp(*number, 0.0f, 1.0f);
        } else if (name == "vignette" && number) {
            s.vignette = std::clamp(*number, 0.0f, 1.0f);
        } else {
            return false;
        }
        return true;
    }

    void updateCamera(App& app, float dt) {
        const app::Input& input = app.input();
        Section& section = current();
        demo::OrbitCamera& camera = section.camera;
        glm::vec2 turn(0.0f);
        float zoom = 1.0f;

        // Mouse: drag with either button, wheel; a press and release without a drag is a click.
        if (!input.mouseFromTouch()) {
            if (input.pressed(m_drag)) {
                m_pressed = true;
                m_pressPoint = input.mousePosition();
                m_dragged = false;
            }
            if (input.down(m_drag) && m_pressed) {
                if (glm::length(input.mousePosition() - m_pressPoint) > 4.0f) m_dragged = true;
                if (m_dragged) turn += input.mouseDelta() * 0.008f;
            }
            if (input.released(m_drag) && m_pressed) {
                if (!m_dragged) click(app, m_pressPoint);
                m_pressed = false;
            }
            if (!input.uiCapturesMouse()) zoom *= std::pow(0.9f, input.wheel().y);
        }

        // Fingers (not on a window): one drags the orbit, two pinch; a short touch without a drag is a tap.
        const std::span<const app::Finger> fingers = input.fingers();
        for (const app::Finger& f : fingers) {
            if (f.pressed) {
                m_touchStart = f.position;
                m_touchMoved = input.uiCapturesMouse();
                m_touchId = f.id;
            }
            if (f.id == m_touchId && glm::length(f.position - m_touchStart) > 8.0f) m_touchMoved = true;
            if (f.released && f.id == m_touchId && !m_touchMoved && fingers.size() == 1) click(app, f.position);
        }
        if (section.orbitWithFingers() && !input.uiCapturesMouse()) {
            if (fingers.size() == 1 && !fingers[0].pressed) turn += fingers[0].delta * 0.008f;
            if (fingers.size() == 2) {
                const float now = glm::length(fingers[0].position - fingers[1].position);
                const float before =
                    glm::length((fingers[0].position - fingers[0].delta) - (fingers[1].position - fingers[1].delta));
                if (now > 1.0f && before > 1.0f) zoom *= before / now;
            }
        }

        // Gamepad: right stick turns, the triggers zoom.
        if (section.orbitWithFingers()) {
            turn.x += input.axis(m_orbitLeft, m_orbitRight) * 2.0f * dt;
            turn.y += input.axis(m_orbitUp, m_orbitDown) * 1.5f * dt;
        }
        zoom *= std::pow(2.0f, (input.value(m_zoomOut) - input.value(m_zoomIn)) * dt);

        camera.yaw -= turn.x;
        camera.pitch = std::clamp(camera.pitch + turn.y, glm::radians(-5.0f), glm::radians(85.0f));
        camera.distance = std::clamp(camera.distance * zoom, camera.minDistance, camera.maxDistance);
    }

    void click(App& app, glm::vec2 point) {
        if (app.input().uiCapturesMouse()) return;
        ++m_clicks;
        current().click(*m_context, point);
    }

    void timings(App& app) {
        const app::FrameStats& stats = app.stats();
        if (app.deterministic()) {
            ImGui::TextUnformatted("Timings hidden (deterministic run)"); // captures compare against golden images
        } else {
            ImGui::Text("%.0f fps, frame %.2f ms", stats.fps, stats.frameMs);
            if (stats.gpuMs >= 0.0)
                ImGui::Text("CPU %.2f ms, GPU %.2f ms", stats.cpuMs, stats.gpuMs);
            else
                ImGui::Text("CPU %.2f ms, GPU time n/a", stats.cpuMs);
            const gpu::Caps& caps = app.device().caps();
            ImGui::Text("GPU backend %s, pass timing %s", gpu::backendName(caps.backend),
                        caps.gpuPassTiming ? "on" : "n/a");
            // GPU busy time per range and when it ran in the frame; nested ranges indented under theirs.
            for (const gpu::GpuTiming& t : stats.gpuPasses)
                ImGui::Text("%*s%-8s %.3f ms  (%.2f .. %.2f)", int(2 + 2 * t.depth), "", t.name.c_str(), t.milliseconds,
                            t.startMilliseconds, t.endMilliseconds);
        }
        const render::RenderStats& s = m_renderer->stats();
        ImGui::Text("%u draws, %u shadow draws", s.draws, s.shadowDraws);
        ImGui::Text("%s, %s", app.device().caps().driver.c_str(), app.device().caps().deviceName.c_str());
    }

    std::string m_startSection;
    std::optional<render::Renderer> m_renderer;
    std::optional<demo::Context> m_context;
    std::vector<std::unique_ptr<Section>> m_sections;
    std::size_t m_current = 0;
    bool m_entered = false;
    bool m_cut = true;
    bool m_showUi = true;
    bool m_uiReappears = false;                         // the windows were hidden and are shown again this frame
    float m_captionTop = 1.0e9f;                        // top of the caption window in the last frame (points)
    std::vector<std::optional<glm::vec2>> m_menuPoints; // menu entry centers in the last frame (points)
    std::uint32_t m_targetWidth = 0;
    app::ActionId m_next = 0, m_prev = 0, m_toggleUi = 0, m_drag = 0;
    app::ActionId m_orbitLeft = 0, m_orbitRight = 0, m_orbitUp = 0, m_orbitDown = 0, m_zoomIn = 0, m_zoomOut = 0;
    std::vector<app::ActionId> m_direct;
    bool m_pressed = false, m_dragged = false;
    glm::vec2 m_pressPoint{0.0f};
    std::uint64_t m_touchId = 0;
    glm::vec2 m_touchStart{0.0f};
    bool m_touchMoved = true;
    int m_clicks = 0;
};

constexpr const char* kHelp =
    "quench_demo options:\n"
    "  --section ID   start in a section: materials, shadows, sky, post, hdr, physics, input, assets, audio,\n"
    "                 animation\n";

} // namespace

Result<std::unique_ptr<quench::app::Game>> quench::app::createGame(AppDesc& desc,
                                                                   std::span<const std::string_view> args) {
    desc.name = QUENCH_GAME_NAME;
    desc.organization = "Quench";
    desc.version = QUENCH_GAME_VERSION;
    std::string section;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help") {
            std::fputs(kHelp, stdout);
            continue;
        }
        if (args[i] == "--section" && i + 1 < args.size()) {
            section = std::string(args[++i]);
            continue;
        }
        return Error{std::format("demo: unknown argument '{}' (--help lists the options)", args[i])};
    }
    return std::unique_ptr<Game>(std::make_unique<Demo>(std::move(section)));
}
