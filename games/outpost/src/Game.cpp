// Outpost: a small demo game on Quench. A two-legged walker gathers energy cells around an outpost and carries them
// to the hangar; every three cells bring one of the hangar's four pods online.
//
// The outpost, the walker and the cells are models built in Blender by tools/make_models.py and cooked at build time;
// the same script writes assets/level.json with the places the game needs (lamps, pods, the door, what blocks the
// way). The renderer draws them from a retained scene with a narrow field of view from far away (an isometric look);
// the walker finds its way on a navmesh built at start from the level's blockers, and its legs are placed by a small
// two-bone IK from a walk cycle.

#include <quench/app/App.h>
#include <quench/app/Input.h>
#include <quench/app/Scenario.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Json.h>
#include <quench/core/Log.h>
#include <quench/navigation/Crowd.h>
#include <quench/navigation/NavMesh.h>
#include <quench/navigation/NavQuery.h>
#include <quench/particles/Particles.h>
#include <quench/render/Daylight.h>
#include <quench/render/MeshBuilder.h>
#include <quench/render/Projection.h>
#include <quench/render/Renderer.h>
#include <quench/render/SceneAssets.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <vector>

namespace {

using namespace quench;
using app::App;
using render::ModelInstanceId;

constexpr float kPi = 3.14159265f;
constexpr int kCapacity = 3;    // cells the walker carries
constexpr int kCellsPerPod = 3; // cells that bring a pod online
constexpr int kPods = 4;
constexpr int kMaxCells = 5; // cells lying around at a time
constexpr float kAgentRadius = 1.2f;
constexpr int kMarkerParts = 32; // the tubes of the ring that marks where a click sent the walker
constexpr float kMarkerRadius = 0.9f, kMarkerTube = 0.05f;
constexpr float kWalkSpeed = 4.2f;   // m/s
constexpr float kStride = 1.5f;      // m a foot travels in one step
constexpr float kStepLift = 0.38f;   // m a foot rises in its swing
constexpr float kLegHeight = 1.72f;  // m from the hip down to the ankle when standing
constexpr float kPickupRange = 1.7f; // m
constexpr float kFovY = 20.0f;       // degrees: narrow, seen from far away
constexpr float kDayElevation = 46.0f;
constexpr float kDuskElevation = -7.0f; // the blue hour: the sun is down, the lamps light the pad
constexpr float kMinElevation = -15.0f, kMaxElevation = 70.0f;
constexpr float kSunAzimuth = 198.0f;

glm::vec3 vec3Of(const json::Value& v) {
    if (!v.isArray() || v.array.size() < 3) return glm::vec3(0.0f);
    return {float(v.array[0].numberOr(0.0)), float(v.array[1].numberOr(0.0)), float(v.array[2].numberOr(0.0))};
}

glm::vec4 vec4Of(const json::Value& v) {
    if (!v.isArray() || v.array.size() < 4) return glm::vec4(0.0f);
    return {float(v.array[0].numberOr(0.0)), float(v.array[1].numberOr(0.0)), float(v.array[2].numberOr(0.0)),
            float(v.array[3].numberOr(0.0))};
}

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float wrapAngle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

// The places of assets/level.json, in game coordinates (rectangles: x0, z0, x1, z1).
struct Level {
    std::vector<glm::vec3> lamps, pods, interiorLights;
    std::vector<glm::vec4> blockers;
    std::vector<glm::vec2> pad; // the concrete pad's outline in x, z
    glm::vec4 bounds{0.0f};
    glm::vec4 ramp{0.0f};
    // Ground the walker steps over that is higher than the pad or the ramp (the ramp's curbs, lip and teeth): its
    // top at the rectangle's front (z1) and back (z0).
    struct Raised {
        glm::vec4 rect{0.0f};
        float frontY = 0.0f, backY = 0.0f;
    };
    std::vector<Raised> raised;
    float padY = 0.2f;
    float rampFrontY = 0.3f, rampBackY = 0.56f, rampFrontZ = 8.0f, rampBackZ = 1.0f;
    glm::vec3 door{0.0f};
    float doorHalfWidth = 7.0f;
    glm::vec3 walkerStart{0.0f};
    glm::vec3 hip{1.12f, -0.35f, -0.1f}; // the right hip in the body's frame
    float thigh = 1.0f, shin = 1.1f, ankle = 0.3f;

    static Result<Level> parse(const std::string& text) {
        Result<json::Value> root = json::parse(text);
        if (!root) return Error{root.error()};
        Level level;
        for (const json::Value& v : (*root)["lamps"].array) level.lamps.push_back(vec3Of(v));
        for (const json::Value& v : (*root)["pods"].array) level.pods.push_back(vec3Of(v));
        for (const json::Value& v : (*root)["interior_lights"].array) level.interiorLights.push_back(vec3Of(v));
        for (const json::Value& v : (*root)["blockers"].array) level.blockers.push_back(vec4Of(v));
        for (const json::Value& v : (*root)["pad"].array)
            if (v.isArray() && v.array.size() >= 2)
                level.pad.emplace_back(float(v.array[0].numberOr(0.0)), float(v.array[1].numberOr(0.0)));
        level.bounds = vec4Of((*root)["bounds"]);
        const json::Value& ramp = (*root)["ramp"];
        level.ramp = vec4Of(ramp["rect"]);
        level.rampFrontY = float(ramp["front_y"].numberOr(level.rampFrontY));
        level.rampBackY = float(ramp["back_y"].numberOr(level.rampBackY));
        level.rampFrontZ = float(ramp["front_z"].numberOr(level.rampFrontZ));
        level.rampBackZ = float(ramp["back_z"].numberOr(level.rampBackZ));
        level.padY = float((*root)["pad_y"].numberOr(level.padY));
        for (const json::Value& v : (*root)["raised"].array)
            level.raised.push_back(
                {vec4Of(v["rect"]), float(v["front_y"].numberOr(0.0)), float(v["back_y"].numberOr(0.0))});
        level.door = vec3Of((*root)["door"]);
        level.doorHalfWidth = float((*root)["door_half_width"].numberOr(level.doorHalfWidth));
        level.walkerStart = vec3Of((*root)["walker_start"]);
        const json::Value& walker = (*root)["walker"];
        if (walker.isObject()) {
            level.hip = vec3Of(walker["hip"]);
            level.thigh = float(walker["thigh"].numberOr(level.thigh));
            level.shin = float(walker["shin"].numberOr(level.shin));
            level.ankle = float(walker["ankle"].numberOr(level.ankle));
        }
        if (level.pods.size() != std::size_t(kPods) || level.lamps.empty() || level.pad.size() < 3 ||
            !(level.bounds.z > level.bounds.x) || !(level.bounds.w > level.bounds.y))
            return Error{"pods, lamps, the pad or the bounds are missing"};
        return level;
    }

    bool onRamp(glm::vec2 p, float margin = 0.0f) const {
        return p.x >= ramp.x - margin && p.x <= ramp.z + margin && p.y >= ramp.y - margin && p.y <= ramp.w + margin;
    }

    bool onPad(glm::vec2 p) const {
        bool inside = false;
        for (std::size_t i = 0, j = pad.size() - 1; i < pad.size(); j = i++) {
            const glm::vec2 a = pad[i], b = pad[j];
            if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
        }
        return inside;
    }

    // The height of the ground the walker stands on at x, z.
    float groundHeight(glm::vec2 p) const {
        if (onRamp(p)) {
            const float t = std::clamp((rampFrontZ - p.y) / (rampFrontZ - rampBackZ), 0.0f, 1.0f);
            return glm::mix(rampFrontY, rampBackY, t);
        }
        return onPad(p) ? padY : 0.0f;
    }

    // The height of what is seen from above at x, z among the ground and what the walker steps over: where
    // something drawn on the ground has to lie to stay in sight.
    float surfaceHeight(glm::vec2 p) const {
        float height = groundHeight(p);
        for (const Raised& r : raised) {
            if (p.x < r.rect.x || p.x > r.rect.z || p.y < r.rect.y || p.y > r.rect.w) continue;
            height = std::max(height, glm::mix(r.backY, r.frontY, (p.y - r.rect.y) / (r.rect.w - r.rect.y)));
        }
        return height;
    }
};

// What the walker's pose is made from; kept for the last two ticks and interpolated.
struct WalkerState {
    glm::vec3 position{0.0f}; // x, z on the navmesh; y: the smoothed ground height
    float heading = 0.5f;     // radians about +y, 0: facing +z; not wrapped
    float phase = 0.0f;       // walk cycles, not wrapped
    float gait = 0.0f;        // 0 standing .. 1 walking
};

// A cooked model and its outline: the same shapes grown a little, turned inside out and black, so only their rim
// shows around the model (tools/make_models.py).
struct Model {
    assets::AssetRef<render::ModelAsset> mesh, outline;
};

// A model placed in the scene with its outline.
struct Part {
    ModelInstanceId mesh, outline;
    bool visible = true;
};
using PartId = std::size_t;

struct Cell {
    glm::vec3 position{0.0f};
    PartId part = 0;
    bool active = false;
    double respawnAt = 0.0; // simulation time
};

enum class Goal : std::uint8_t { None, Cell, Door, Manual, Steer };

class Outpost final : public app::Game {
public:
    explicit Outpost(bool autopilot) : m_autopilot(autopilot) {}

    Result<void> init(App& app) override {
        Result<std::string> text = fs::readAssetText("level.json");
        if (!text) return Error{text.error()};
        Result<Level> level = Level::parse(*text);
        if (!level) return Error{std::format("level.json: {}", level.error())};
        m_level = std::move(*level);

        m_rayTracingAvailable = app.device().caps().rayTracing;
        Result<render::Renderer> renderer = render::Renderer::create(app.device(), renderSettings());
        if (!renderer) return Error{renderer.error()};
        m_renderer.emplace(std::move(*renderer));
        m_scene.emplace(app.device());
        m_sceneAssets.emplace(*m_scene, app.assets());

        if (Result<void> r = buildScene(); !r) return r;
        if (Result<void> r = buildNavigation(app); !r) return r;

        using app::Binding;
        app::Input& input = app.input();
        m_left = input.addAction("left", {*Binding::key("A"), *Binding::key("Left")});
        m_right = input.addAction("right", {*Binding::key("D"), *Binding::key("Right")});
        m_forward = input.addAction("forward", {*Binding::key("W"), *Binding::key("Up")});
        m_back = input.addAction("back", {*Binding::key("S"), *Binding::key("Down")});
        m_turnLeft = input.addAction("turn_left", {*Binding::key("Q")});
        m_turnRight = input.addAction("turn_right", {*Binding::key("E")});
        m_toggleTime = input.addAction("time", {*Binding::key("T")});
        m_toggleAutopilot = input.addAction("autopilot", {*Binding::key("P")});
        return {};
    }

    void fixedStep(App& app, double dt) override {
        const float step = float(dt);
        m_simTime += dt;
        const app::Input& input = app.input();

        // The camera's turn and the sun on the simulation clock, so scripted runs repeat.
        m_azimuth += wrapAngle(m_azimuthTarget - m_azimuth) * std::min(1.0f, step * 6.0f);
        const float sunStep = 14.0f * step;
        m_sunElevation += std::clamp(m_sunTarget - m_sunElevation, -sunStep, sunStep);

        if (!m_crowd || !m_agent.valid()) return;

        // Steering by keys, relative to the camera; it takes over from any target.
        const glm::vec2 stick(input.axis(m_left, m_right), input.axis(m_back, m_forward));
        if (glm::length(stick) > 0.1f) {
            const glm::vec2 forward(-std::sin(m_azimuth), -std::cos(m_azimuth));
            const glm::vec2 right(-forward.y, forward.x);
            glm::vec2 move = right * stick.x + forward * stick.y;
            if (const float length = glm::length(move); length > 1.0f) move /= length;
            m_crowd->setVelocity(m_agent, glm::vec3(move.x, 0.0f, move.y) * kWalkSpeed);
            m_goal = Goal::Steer;
        } else if (m_goal == Goal::Steer) {
            m_crowd->setVelocity(m_agent, glm::vec3(0.0f));
            m_crowd->clearTarget(m_agent);
            m_goal = Goal::None;
        }

        if (m_autopilot && m_navMesh->ready()) planAutopilot();

        m_crowd->update(step);
        const navigation::AgentState agent = m_crowd->state(m_agent);
        if (m_goal == Goal::Manual && (agent.arrived || !agent.hasTarget)) m_goal = Goal::None;

        // The walker follows its agent: it turns toward where it moves and its legs cycle with the distance walked.
        m_previous = m_walker;
        const glm::vec2 velocity(agent.velocity.x, agent.velocity.z);
        const float speed = glm::length(velocity);
        m_speed = speed;
        const glm::vec2 ground(agent.position.x, agent.position.z);
        const float height = m_level.groundHeight(ground);
        m_walker.position.x = ground.x;
        m_walker.position.z = ground.y;
        m_walker.position.y += (height - m_walker.position.y) * std::min(1.0f, step * 10.0f);
        if (speed > 0.3f) {
            const float wanted = std::atan2(velocity.x, velocity.y);
            m_walker.heading += wrapAngle(wanted - m_walker.heading) * std::min(1.0f, step * 9.0f);
        }
        m_walker.phase += speed * step / (2.0f * kStride);
        const float gait = std::clamp(speed / 1.6f, 0.0f, 1.0f);
        m_walker.gait += (gait - m_walker.gait) * std::min(1.0f, step * 7.0f);

        collectAndDeliver(ground);

        // A cell the walker cannot get to (it stands still short of it) is put somewhere else.
        m_stuck = m_goal == Goal::Cell && speed < 0.3f ? m_stuck + step : 0.0f;
        if (m_stuck > 1.5f && m_goalCell) {
            for (Cell& cell : m_cells)
                if (&cell == m_goalCell) {
                    cell.active = false;
                    cell.respawnAt = m_simTime;
                }
            m_goalCell = nullptr;
            m_goal = Goal::None;
            m_stuck = 0.0f;
        }
        for (Cell& cell : m_cells)
            if (!cell.active && m_navMesh->ready() && m_simTime >= cell.respawnAt) spawnCell(cell);
    }

    void update(App& app, double frameDt) override {
        (void)frameDt;
        m_navMesh->update();
        m_sceneAssets->update();

        app::Input& input = app.input();
        m_windowSize = glm::vec2(float(std::max(app.windowWidth(), 1)), float(std::max(app.windowHeight(), 1)));

        // The camera: Q and E turn it by an eighth, a right drag freely, the wheel zooms.
        if (input.pressed(m_turnLeft)) m_azimuthTarget -= kPi / 4.0f;
        if (input.pressed(m_turnRight)) m_azimuthTarget += kPi / 4.0f;
        if (input.mouseDown(app::MouseButton::Right) && !input.uiCapturesMouse()) {
            m_azimuthTarget -= input.mouseDelta().x * 0.006f;
            m_azimuth = m_azimuthTarget;
            m_elevation = std::clamp(m_elevation + input.mouseDelta().y * 0.2f, 14.0f, 75.0f);
        }
        if (!input.uiCapturesMouse() && input.wheel().y != 0.0f)
            m_distance = std::clamp(m_distance * std::pow(0.9f, input.wheel().y), 45.0f, 220.0f);
        if (input.pressed(m_toggleTime)) m_sunTarget = m_sunTarget > 20.0f ? kDuskElevation : kDayElevation;
        if (input.pressed(m_toggleAutopilot)) setAutopilot(!m_autopilot);

        const float alpha = float(app.alpha());
        const WalkerState walker = interpolated(alpha);
        updateCamera(walker);

        // A click or a tap on the ground sends the walker there.
        if (input.mousePressed(app::MouseButton::Left) && !input.uiCapturesMouse() && m_agent.valid()) {
            if (std::optional<glm::vec3> point = groundPoint(input.mousePosition())) {
                if (m_crowd->setTarget(m_agent, glm::vec3(point->x, 0.0f, point->z))) {
                    m_goal = Goal::Manual;
                    // The marker lies where the walker goes: the walkable point nearest to the click.
                    m_marker = glm::vec2(point->x, point->z);
                    if (m_query)
                        if (const auto near = m_query->nearestPoint(glm::vec3(point->x, 0.0f, point->z)))
                            m_marker = glm::vec2(near->position.x, near->position.z);
                    m_markerTime = app.time();
                }
            }
        }

        placeWalker(walker);
        placeCells(app.time());
        updateLights(walker);

        placeMarker(m_goal == Goal::Manual, app.time());

        if (const render::RenderSettings settings = renderSettings(); !(settings == m_renderer->settings()))
            m_renderer->setSettings(settings);
    }

    void ui(App& app) override {
        const app::WindowRect safe = app.safeArea();
        ImGui::SetNextWindowPos(ImVec2(float(safe.x) + 12.0f, float(safe.y) + 12.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin(QUENCH_GAME_NAME, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        const int online = podsOnline();
        ImGui::Text("Cells carried %d / %d, delivered %d", m_carried, kCapacity, m_delivered);
        const float charge = online >= kPods ? 1.0f : float(m_delivered % kCellsPerPod) / float(kCellsPerPod);
        ImGui::ProgressBar(charge, ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f),
                           online >= kPods ? "outpost online" : std::format("pod {} charging", online + 1).c_str());
        ImGui::Text("Pods online %d / %d", online, kPods);
        ImGui::Separator();
        bool autopilot = m_autopilot;
        if (ImGui::Checkbox("Autopilot (P)", &autopilot)) setAutopilot(autopilot);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (ImGui::SliderFloat("Sun (T)", &m_sunElevation, kMinElevation, kMaxElevation, "%.0f deg"))
            m_sunTarget = m_sunElevation;
        if (m_rayTracingAvailable) ImGui::Checkbox("Ray-traced shadows and occlusion", &m_rayTracing);
        ImGui::Checkbox("Outlines", &m_outlines);
        ImGui::Separator();
        ImGui::TextDisabled("Click: send the walker   WASD: steer");
        ImGui::TextDisabled("Q, E, right drag: turn   Wheel: zoom");
        if (!app.deterministic()) ImGui::TextDisabled("%.0f fps, %.2f ms", app.stats().fps, app.stats().frameMs);
        ImGui::End();
    }

    void render(App& app, app::Frame& frame) override {
        const render::View view{.view = m_view,
                                .fovY = glm::radians(kFovY),
                                .nearPlane = std::max(m_distance - 70.0f, 1.0f),
                                .cut = std::exchange(m_cut, false),
                                .time = app.time(),
                                .probeCenter = m_target};
        m_renderer->render(*m_scene, view, frame.commands, frame.target, frame.output);
    }

    void shutdown(App& app) override {
        (void)app;
        m_crowd.reset();
        m_query.reset();
        m_navMesh.reset();
        m_base = m_podGlow = m_bodyModel = m_thighModel = m_shinModel = m_footModel = m_cellModel = {};
        m_sceneAssets.reset();
        m_scene.reset();
        m_renderer.reset();
    }

    std::optional<double> scenarioValue(std::string_view name) const override {
        if (name == "models_ready")
            return ready(m_base) && ready(m_bodyModel) && ready(m_thighModel) && ready(m_shinModel) &&
                           ready(m_footModel) && ready(m_cellModel) && ready(m_podGlow)
                       ? 1.0
                       : 0.0;
        if (name == "nav_ready") return m_navMesh && m_navMesh->ready() ? 1.0 : 0.0;
        if (name == "walker_x") return m_walker.position.x;
        if (name == "walker_y") return m_walker.position.y;
        if (name == "walker_z") return m_walker.position.z;
        if (name == "walker_speed") return m_speed;
        if (name == "carried") return m_carried;
        if (name == "delivered") return m_delivered;
        if (name == "pods_online") return podsOnline();
        if (name == "cells")
            return double(std::count_if(m_cells.begin(), m_cells.end(), [](const Cell& c) { return c.active; }));
        if (name == "autopilot") return m_autopilot ? 1.0 : 0.0;
        if (name == "goal") return double(m_goal);
        if (name == "stuck") return m_stuck;
        if (name == "cell_distance")
            return m_goalCell ? glm::distance(glm::vec2(m_goalCell->position.x, m_goalCell->position.z),
                                              glm::vec2(m_walker.position.x, m_walker.position.z))
                              : -1.0;
        if (name == "night") return night();
        if (name == "sun") return m_sunElevation;
        if (name == "ray_shadows") return m_renderer->effects().sunShadows == render::SunShadows::RayTraced ? 1.0 : 0.0;
        if (name == "instances") return m_renderer->stats().instances;
        return std::nullopt;
    }

    // autopilot on|off; sun DEGREES; camera AZIMUTH_DEGREES [DISTANCE]; raytracing on|off; outlines on|off
    app::ScenarioExec scenarioCommand(App& app, const app::ScenarioCommand& command) override {
        (void)app;
        const auto flag = [&](bool& value) {
            if (command.args.size() != 1 || (command.args[0] != "on" && command.args[0] != "off"))
                return app::ScenarioExec::Failed;
            value = command.args[0] == "on";
            return app::ScenarioExec::Done;
        };
        if (command.op == "autopilot") {
            bool on = m_autopilot;
            const app::ScenarioExec result = flag(on);
            if (result == app::ScenarioExec::Done) setAutopilot(on);
            return result;
        }
        if (command.op == "raytracing") return flag(m_rayTracing);
        if (command.op == "outlines") return flag(m_outlines);
        if (command.op == "occlusion") return flag(m_occlusion);
        if (command.op == "sharpen") return flag(m_sharpen);
        if (command.op == "occlusion_rays" && command.args.size() == 1) {
            m_occlusionRays = std::clamp(std::stoi(command.args[0]), 1, 8);
            return app::ScenarioExec::Done;
        }
        if (command.op == "sun" && command.args.size() == 1) {
            m_sunElevation = m_sunTarget = std::clamp(std::stof(command.args[0]), kMinElevation, kMaxElevation);
            return app::ScenarioExec::Done;
        }
        if (command.op == "camera" && !command.args.empty() && command.args.size() <= 2) {
            m_azimuth = m_azimuthTarget = glm::radians(std::stof(command.args[0]));
            if (command.args.size() == 2) m_distance = std::clamp(std::stof(command.args[1]), 30.0f, 220.0f);
            m_cut = true;
            return app::ScenarioExec::Done;
        }
        if (command.op == "look" && command.args.size() == 3) { // the camera's target, in place of the walker
            m_look = glm::vec3(std::stof(command.args[0]), std::stof(command.args[1]), std::stof(command.args[2]));
            m_cut = true;
            return app::ScenarioExec::Done;
        }
        return app::ScenarioExec::Unknown;
    }

    // @walker, @door, @start, @cell (the active cell nearest to the walker), @pad (an open place on the pad)
    std::optional<glm::vec2> scenarioPoint(std::string_view name) const override {
        if (name == "walker") return project(m_walker.position);
        if (name == "door") return project(m_level.door + glm::vec3(0.0f, 0.0f, 1.2f));
        if (name == "start") return project(m_level.walkerStart);
        if (name == "pad") return project(m_level.walkerStart + glm::vec3(-9.0f, 0.0f, 1.5f));
        if (name == "cell") {
            const Cell* cell = nearestCell();
            return cell ? project(cell->position) : std::nullopt;
        }
        return std::nullopt;
    }

private:
    // ---- setup

    render::RenderSettings renderSettings() const {
        render::RenderSettings s;
        // The camera is far away: the cascades start just in front of the outpost (View::nearPlane) and end behind it.
        s.shadowCascades = 3;
        s.shadowMapSize = 4096;
        s.shadowDistance = 320.0f;
        s.shadowFilter = 2;
        s.bloomStrength = 0.07f;
        s.vignette = 0.12f;
        s.sharpen = m_sharpen ? 0.2f : 0.0f;
        if (m_rayTracing && m_rayTracingAvailable) {
            s.sunShadows = render::SunShadows::RayTraced;
            s.shadowSoftness = 0.6f;
            s.ambientOcclusion = render::AmbientOcclusion::RayTraced;
            s.occlusionRadius = 1.6f;
        } else if (m_occlusion) {
            s.ambientOcclusion = render::AmbientOcclusion::ScreenSpace;
            s.occlusionRadius = 1.6f;
            s.occlusionRays = m_occlusionRays;
        }
        return s;
    }

    Result<void> buildScene() {
        render::Scene& scene = *m_scene;

        m_base = loadModel("base", 10.0f);
        addPart(m_base);
        m_podGlow = loadModel("pod_glow", 1.0f, false);
        for (int i = 0; i < kPods; ++i) {
            m_podGlowParts[std::size_t(i)] = addPart(m_podGlow, false);
            setPart(m_podGlowParts[std::size_t(i)], glm::translate(glm::mat4(1.0f), m_level.pods[std::size_t(i)]));
        }
        m_bodyModel = loadModel("walker_body", 8.0f);
        m_thighModel = loadModel("walker_thigh", 8.0f);
        m_shinModel = loadModel("walker_shin", 8.0f);
        m_footModel = loadModel("walker_foot", 8.0f);
        m_body = addPart(m_bodyModel);
        for (std::size_t leg = 0; leg < 2; ++leg) {
            m_thigh[leg] = addPart(m_thighModel);
            m_shin[leg] = addPart(m_shinModel);
            m_foot[leg] = addPart(m_footModel);
        }
        m_cellModel = loadModel("cell", 6.0f);
        for (Cell& cell : m_cells) cell.part = addPart(m_cellModel, false);
        for (PartId& carried : m_carriedParts) carried = addPart(m_cellModel, false);

        m_walker.position = glm::vec3(m_level.walkerStart.x, m_level.padY, m_level.walkerStart.z);
        m_previous = m_walker;

        // Lights: the sun, the lamps, the hangar's bay, a light per pod (off until it is online), the walker's lamp.
        m_sun = scene.addLight({.type = render::LightType::Directional});
        for (const glm::vec3& lamp : m_level.lamps)
            m_lampLights.push_back(scene.addLight({.color = {0.35f, 0.85f, 1.0f},
                                                   .intensity = 0.0f,
                                                   .position = lamp,
                                                   .range = 15.0f,
                                                   .castsShadows = false}));
        for (const glm::vec3& light : m_level.interiorLights)
            m_bayLights.push_back(scene.addLight({.color = {0.62f, 0.86f, 1.0f},
                                                  .intensity = 0.0f,
                                                  .position = light,
                                                  .range = 9.0f,
                                                  .castsShadows = false}));
        for (const glm::vec3& pod : m_level.pods)
            m_podLights.push_back(scene.addLight({.color = {0.3f, 0.85f, 1.0f},
                                                  .intensity = 0.0f,
                                                  .position = pod + glm::vec3(0.0f, 0.2f, 2.4f),
                                                  .range = 8.0f,
                                                  .castsShadows = false}));
        m_headLight = scene.addLight({.type = render::LightType::Spot,
                                      .color = {0.75f, 0.92f, 1.0f},
                                      .intensity = 0.0f,
                                      .range = 16.0f,
                                      .innerAngle = 0.3f,
                                      .outerAngle = 0.55f,
                                      .castsShadows = false});

        // The ring that shows where a click sent the walker: short tubes from point to point (placeMarker).
        Result<render::MeshId> ring = scene.addMesh(render::cylinderMesh(kMarkerTube, 0.5f, 8).desc("marker"));
        if (!ring) return Error{ring.error()};
        const render::MaterialId glow = scene.addMaterial({.baseColor = {0.02f, 0.1f, 0.12f, 1.0f},
                                                           .emissive = {0.5f, 2.6f, 3.2f},
                                                           .shadows = render::ShadowMode::None,
                                                           .name = "marker"});
        for (render::InstanceId& part : m_markerParts)
            part = scene.addInstance({.mesh = *ring, .material = glow, .visible = false, .castsShadows = false});

        // Sparks for a pickup and a delivery.
        particles::ParticleDesc sparks;
        sparks.maxParticles = 512;
        sparks.rate = 0.0f;
        sparks.shape = particles::Shape::Sphere;
        sparks.extent = glm::vec3(0.4f);
        sparks.lifetime = {0.5f, 1.2f};
        sparks.speed = {1.5f, 4.5f};
        sparks.spread = 1.3f;
        sparks.gravity = {0.0f, -3.0f, 0.0f};
        sparks.drag = 1.4f;
        sparks.size = {0.06f, 0.14f};
        sparks.color = {0.5f, 2.2f, 3.0f};
        sparks.twinkle = 0.5f;
        sparks.twinkleRate = 6.0f;
        sparks.sprite = particles::Sprite::Star;
        if (Result<render::ParticleSystemId> system = scene.addParticles(sparks))
            m_sparks = *system;
        else
            log::warning("game", "no sparks: {}", system.error());
        return {};
    }

    Result<void> buildNavigation(App& app) {
        // The ground is one flat quad; everything that stands on it is cut out. The boxes are cut after the mesh
        // kept its distance from walls, so they are grown by the walker's radius here.
        const glm::vec4 b = m_level.bounds;
        navigation::Geometry geometry;
        geometry.vertices = {{b.x, 0.0f, b.y}, {b.x, 0.0f, b.w}, {b.z, 0.0f, b.w}, {b.z, 0.0f, b.y}};
        geometry.indices = {0, 1, 2, 0, 2, 3};
        const float grow = kAgentRadius * 0.85f;
        for (const glm::vec4& r : m_level.blockers)
            geometry.boxes.push_back({.min = {r.x - grow, -1.0f, r.y - grow},
                                      .max = {r.z + grow, 6.0f, r.w + grow},
                                      .area = navigation::kNotWalkable});
        navigation::BuildSettings settings;
        settings.agent = {.radius = kAgentRadius, .height = 3.4f, .maxClimb = 0.4f, .maxSlope = 45.0f};
        settings.cellSize = 0.3f;
        settings.cellHeight = 0.2f;
        settings.tileSize = 48;
        Result<navigation::NavMesh> mesh = navigation::NavMesh::build(std::move(geometry), settings, app.jobs());
        if (!mesh) return Error{std::format("navmesh: {}", mesh.error())};
        m_navMesh.emplace(std::move(*mesh));
        m_query.emplace(*m_navMesh);
        m_crowd.emplace(*m_navMesh, navigation::CrowdSettings{.maxAgents = 8, .maxAgentRadius = kAgentRadius + 0.1f});
        Result<navigation::AgentId> agent = m_crowd->addAgent(
            glm::vec3(m_level.walkerStart.x, 0.0f, m_level.walkerStart.z), {.radius = kAgentRadius,
                                                                            .height = 3.4f,
                                                                            .maxSpeed = kWalkSpeed,
                                                                            .maxAcceleration = 9.0f,
                                                                            .separation = 0.0f,
                                                                            .avoidance = navigation::Avoidance::Off,
                                                                            .arrivalDistance = 0.35f});
        if (!agent) return Error{std::format("walker: {}", agent.error())};
        m_agent = *agent;
        return {};
    }

    // ---- models with outlines

    Model loadModel(std::string_view name, float priority, bool outline = true) {
        Model model;
        model.mesh = m_sceneAssets->loadModel(std::format("models/{}.qmodel", name), priority);
        if (outline) model.outline = m_sceneAssets->loadModel(std::format("models/{}_outline.qmodel", name), priority);
        return model;
    }

    static bool ready(const Model& model) { return model.mesh.ready() && (!model.outline || model.outline.ready()); }

    // The outlines are closed shells around the models. They cast no shadows (addPart), which also keeps them out
    // of the traced shadows' and occlusion's geometry.
    bool outlinesShown() const { return m_outlines; }

    PartId addPart(const Model& model, bool visible = true) {
        Part part;
        part.visible = visible;
        part.mesh = m_sceneAssets->addModel(model.mesh, glm::mat4(1.0f), visible);
        if (model.outline) {
            part.outline = m_sceneAssets->addModel(model.outline, glm::mat4(1.0f), visible && outlinesShown());
            // A shell a few centimeters around its model: as a caster it shadows the model wherever the shadow maps
            // are finer than that (the camera close up).
            m_sceneAssets->setCastsShadows(part.outline, false);
        }
        m_parts.push_back(part);
        return m_parts.size() - 1;
    }

    void setPart(PartId id, const glm::mat4& transform) {
        const Part& part = m_parts[id];
        m_sceneAssets->setTransform(part.mesh, transform);
        if (part.outline) m_sceneAssets->setTransform(part.outline, transform);
    }

    void showPart(PartId id, bool visible) {
        Part& part = m_parts[id];
        if (part.visible == visible) return;
        part.visible = visible;
        m_sceneAssets->setVisible(part.mesh, visible);
        if (part.outline) m_sceneAssets->setVisible(part.outline, visible && m_outlinesShown);
    }

    void applyOutlines() {
        if (m_outlinesShown == outlinesShown()) return;
        m_outlinesShown = outlinesShown();
        for (const Part& part : m_parts)
            if (part.outline) m_sceneAssets->setVisible(part.outline, part.visible && m_outlinesShown);
    }

    // ---- the game

    int podsOnline() const { return std::min(m_delivered / kCellsPerPod, kPods); }

    // 0 by day .. 1 at dusk: how much the lamps matter.
    float night() const { return smoothstep(24.0f, 5.0f, m_sunElevation); }

    void setAutopilot(bool on) {
        m_autopilot = on;
        if (m_goal == Goal::Cell || m_goal == Goal::Door) {
            if (m_crowd && m_agent.valid()) m_crowd->clearTarget(m_agent);
            m_goal = Goal::None;
        }
    }

    const Cell* nearestCell() const {
        const Cell* best = nullptr;
        float bestDistance = 1e9f;
        for (const Cell& cell : m_cells) {
            if (!cell.active) continue;
            const float d = glm::distance(glm::vec2(cell.position.x, cell.position.z),
                                          glm::vec2(m_walker.position.x, m_walker.position.z));
            if (d < bestDistance) {
                bestDistance = d;
                best = &cell;
            }
        }
        return best;
    }

    // The autopilot walks to the nearest cell, and to the door when it is full or nothing is left to pick up. A
    // target the player set by a click or the keys is finished first.
    void planAutopilot() {
        if (m_goal == Goal::Manual || m_goal == Goal::Steer) return;
        const Cell* cell = m_carried < kCapacity ? nearestCell() : nullptr;
        if (cell) {
            if (m_goal != Goal::Cell || cell != m_goalCell) {
                if (m_crowd->setTarget(m_agent, glm::vec3(cell->position.x, 0.0f, cell->position.z))) {
                    m_goal = Goal::Cell;
                    m_goalCell = cell;
                }
            }
        } else if (m_carried > 0) {
            if (m_goal != Goal::Door) {
                const float half = m_level.doorHalfWidth - 1.5f;
                const float x = std::clamp(m_walker.position.x, m_level.door.x - half, m_level.door.x + half);
                if (m_crowd->setTarget(m_agent, glm::vec3(x, 0.0f, m_level.door.z + 0.6f))) m_goal = Goal::Door;
            }
        } else {
            m_goal = Goal::None;
        }
    }

    void collectAndDeliver(glm::vec2 ground) {
        for (Cell& cell : m_cells) {
            if (!cell.active || m_carried >= kCapacity) continue;
            if (glm::distance(ground, glm::vec2(cell.position.x, cell.position.z)) > kPickupRange) continue;
            cell.active = false;
            cell.respawnAt = m_simTime + 2.5;
            ++m_carried;
            burst(cell.position + glm::vec3(0.0f, 0.8f, 0.0f), 36);
            if (m_goalCell == &cell) {
                m_goalCell = nullptr;
                if (m_goal == Goal::Cell) m_goal = Goal::None;
            }
        }
        const bool atDoor = std::abs(ground.x - m_level.door.x) <= m_level.doorHalfWidth &&
                            ground.y <= m_level.door.z + 1.6f && ground.y >= m_level.door.z - 3.0f;
        if (atDoor && m_carried > 0) {
            m_delivered += m_carried;
            m_carried = 0;
            burst(glm::vec3(ground.x, m_level.door.y + 2.2f, m_level.door.z - 0.4f), 90);
            if (m_goal == Goal::Door) m_goal = Goal::None;
            log::info("game", "delivered {} cells, {} of {} pods online", m_delivered, podsOnline(), kPods);
        }
    }

    void burst(glm::vec3 position, std::uint32_t count) {
        if (!m_sparks) return;
        m_scene->setParticleTransform(*m_sparks, glm::translate(glm::mat4(1.0f), position), true);
        m_scene->burstParticles(*m_sparks, count);
    }

    // A free place on the navmesh, away from the ramp, the walker and the other cells.
    void spawnCell(Cell& cell) {
        for (int attempt = 0; attempt < 24; ++attempt) {
            Result<navigation::NavPoint> point = m_query->randomPoint(m_seed);
            if (!point) return;
            const glm::vec2 p(point->position.x, point->position.z);
            if (m_level.onRamp(p, 2.0f)) continue;
            if (glm::distance(p, glm::vec2(m_walker.position.x, m_walker.position.z)) < 6.0f) continue;
            bool crowded = false;
            for (const Cell& other : m_cells)
                crowded |= other.active && glm::distance(p, glm::vec2(other.position.x, other.position.z)) < 5.0f;
            if (crowded) continue;
            // Only where the walker can walk to from the pad in front of the door.
            const Result<navigation::Path> path =
                m_query->findPath(glm::vec3(m_level.walkerStart.x, 0.0f, m_level.walkerStart.z), point->position);
            if (!path || path->partial) continue;
            cell.position = glm::vec3(p.x, m_level.groundHeight(p), p.y);
            cell.active = true;
            return;
        }
        cell.respawnAt = m_simTime + 0.5; // no place this time
    }

    // ---- drawing

    WalkerState interpolated(float alpha) const {
        WalkerState s;
        s.position = glm::mix(m_previous.position, m_walker.position, alpha);
        s.heading = glm::mix(m_previous.heading, m_walker.heading, alpha);
        s.phase = glm::mix(m_previous.phase, m_walker.phase, alpha);
        s.gait = glm::mix(m_previous.gait, m_walker.gait, alpha);
        return s;
    }

    void updateCamera(const WalkerState& walker) {
        // Zoomed out the camera looks at the middle of the outpost, zoomed in it follows the walker.
        const float follow = smoothstep(100.0f, 55.0f, m_distance);
        m_target = glm::mix(glm::vec3(1.5f, 3.0f, 0.0f), walker.position + glm::vec3(0.0f, 1.5f, 0.0f), follow);
        if (m_look) m_target = *m_look;
        const float elevation = glm::radians(m_elevation);
        const glm::vec3 offset(std::sin(m_azimuth) * std::cos(elevation), std::sin(elevation),
                               std::cos(m_azimuth) * std::cos(elevation));
        m_eye = m_target + offset * m_distance;
        m_view = glm::lookAt(m_eye, m_target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    // The marker's ring lies on the ground it crosses: each of its points at the height of the ground there, or of
    // higher ground right next to it (the pad's edge, the ramp's sides), so no part of it is under a step.
    void placeMarker(bool visible, double time) {
        for (const render::InstanceId part : m_markerParts) m_scene->setVisible(part, visible);
        if (!visible) return;
        const float radius = kMarkerRadius * (1.0f + 0.12f * std::sin(float(time - m_markerTime) * 6.0f));
        const auto pointAt = [&](int i) {
            const float angle = 2.0f * kPi * float(i) / float(kMarkerParts);
            const glm::vec2 p = m_marker + radius * glm::vec2(std::cos(angle), std::sin(angle));
            float height = m_level.surfaceHeight(p);
            for (int k = 0; k < 8; ++k) {
                const float around = kPi / 4.0f * float(k);
                height =
                    std::max(height, m_level.surfaceHeight(p + 0.15f * glm::vec2(std::cos(around), std::sin(around))));
            }
            return glm::vec3(p.x, height + kMarkerTube + 0.02f, p.y);
        };
        glm::vec3 from = pointAt(0);
        for (int i = 0; i < kMarkerParts; ++i) {
            const glm::vec3 to = pointAt(i + 1);
            const glm::vec3 along = to - from;
            const float length = glm::length(along);
            const glm::vec3 y = along / length, x = glm::normalize(glm::cross(y, glm::vec3(0.0f, 1.0f, 0.0f)));
            // The tube is around its y axis, one unit long; a little longer than the span, the ends overlap.
            const glm::mat4 transform(glm::vec4(x, 0.0f), glm::vec4(y * (length + kMarkerTube), 0.0f),
                                      glm::vec4(glm::cross(x, y), 0.0f), glm::vec4((from + to) * 0.5f, 1.0f));
            m_scene->setTransform(m_markerParts[std::size_t(i)], transform);
            from = to;
        }
    }

    // The point of the ground under a window position, inside the level's bounds.
    std::optional<glm::vec3> groundPoint(glm::vec2 window) const {
        const float tanHalf = std::tan(glm::radians(kFovY) * 0.5f);
        const float aspect = m_windowSize.x / m_windowSize.y;
        const glm::vec2 ndc(2.0f * window.x / m_windowSize.x - 1.0f, 1.0f - 2.0f * window.y / m_windowSize.y);
        const glm::vec3 inView = glm::normalize(glm::vec3(ndc.x * tanHalf * aspect, ndc.y * tanHalf, -1.0f));
        const glm::vec3 direction = glm::mat3(glm::inverse(m_view)) * inView;
        if (direction.y > -1e-4f) return std::nullopt;
        // On the pad's plane first, then on the ground's height there (the grass is lower, the ramp higher).
        glm::vec3 point = m_eye + direction * ((m_level.padY - m_eye.y) / direction.y);
        for (int i = 0; i < 2; ++i)
            point = m_eye + direction * ((m_level.groundHeight({point.x, point.z}) - m_eye.y) / direction.y);
        const glm::vec4 b = m_level.bounds;
        if (point.x < b.x || point.x > b.z || point.z < b.y || point.z > b.w) return std::nullopt;
        return point;
    }

    std::optional<glm::vec2> project(glm::vec3 point) const {
        const float aspect = m_windowSize.x / m_windowSize.y;
        const glm::mat4 projection = render::reversedPerspective(glm::radians(kFovY), aspect, 1.0f);
        const glm::vec4 clip = projection * m_view * glm::vec4(point, 1.0f);
        if (clip.w <= 0.0f) return std::nullopt;
        return glm::vec2((clip.x / clip.w * 0.5f + 0.5f) * m_windowSize.x,
                         (0.5f - clip.y / clip.w * 0.5f) * m_windowSize.y);
    }

    void placeWalker(const WalkerState& walker) {
        const float cycle = walker.phase - std::floor(walker.phase);
        // The body is lowest when both feet are down and far apart, and sways toward the foot that stands.
        const float bob = -0.07f * walker.gait * std::cos(4.0f * kPi * cycle);
        const float roll = 0.05f * walker.gait * std::sin(2.0f * kPi * cycle);
        const float centerHeight = m_level.ankle + kLegHeight - m_level.hip.y + bob;
        const glm::mat4 root =
            glm::rotate(glm::translate(glm::mat4(1.0f), walker.position + glm::vec3(0.0f, centerHeight, 0.0f)),
                        walker.heading, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 body = glm::rotate(root, roll, glm::vec3(0.0f, 0.0f, 1.0f));
        body = glm::rotate(body, 0.07f * walker.gait, glm::vec3(1.0f, 0.0f, 0.0f));
        setPart(m_body, body);

        const glm::vec3 xAxis(1.0f, 0.0f, 0.0f);
        for (std::size_t leg = 0; leg < 2; ++leg) {
            const float side = leg == 0 ? -1.0f : 1.0f;
            const float u = std::fmod(cycle + (leg == 0 ? 0.0f : 0.5f), 1.0f);
            float reach = 0.0f, lift = 0.0f, toe = 0.0f;
            if (u < 0.5f) { // standing on it: the foot goes back under the body
                reach = kStride * (0.5f - 2.0f * u);
            } else { // swinging it forward
                const float t = (u - 0.5f) * 2.0f;
                reach = kStride * (t * t * (3.0f - 2.0f * t) - 0.5f);
                lift = kStepLift * std::sin(kPi * t);
                toe = 0.4f * std::sin(kPi * t);
            }
            reach *= walker.gait;
            lift *= walker.gait;
            toe *= walker.gait;

            // Two-bone IK in the leg's y, z plane, the knee bending backward.
            const glm::vec3 hip(side * m_level.hip.x, m_level.hip.y, m_level.hip.z);
            const glm::vec3 ankle(hip.x, -centerHeight + m_level.ankle + lift, hip.z + reach);
            const float dy = ankle.y - hip.y, dz = ankle.z - hip.z;
            const float l1 = m_level.thigh, l2 = m_level.shin;
            const float distance = std::clamp(std::sqrt(dy * dy + dz * dz), 0.4f, l1 + l2 - 0.02f);
            const float toAnkle = std::atan2(dz, -dy); // from straight down, forward positive
            const float atHip =
                std::acos(std::clamp((l1 * l1 + distance * distance - l2 * l2) / (2.0f * l1 * distance), -1.0f, 1.0f));
            const float thighAngle = toAnkle - atHip;
            const glm::vec3 knee = hip + l1 * glm::vec3(0.0f, -std::cos(thighAngle), std::sin(thighAngle));
            const float shinAngle = std::atan2(ankle.z - knee.z, -(ankle.y - knee.y));
            setPart(m_thigh[leg], glm::rotate(glm::translate(root, hip), -thighAngle, xAxis));
            setPart(m_shin[leg], glm::rotate(glm::translate(root, knee), -shinAngle, xAxis));
            setPart(m_foot[leg], glm::rotate(glm::translate(root, ankle), toe, xAxis));
        }

        // The cells it carries ride behind its back.
        for (int i = 0; i < kCapacity; ++i) {
            const PartId part = m_carriedParts[std::size_t(i)];
            showPart(part, i < m_carried);
            const glm::vec3 seat(float(i - 1) * 0.55f, -0.35f, -1.25f - (i == 1 ? 0.12f : 0.0f));
            setPart(part, glm::scale(glm::translate(body, seat), glm::vec3(0.8f)));
        }
    }

    void placeCells(double time) {
        for (std::size_t i = 0; i < m_cells.size(); ++i) {
            const Cell& cell = m_cells[i];
            showPart(cell.part, cell.active);
            if (!cell.active) continue;
            const float t = float(time) + float(i) * 1.7f;
            const glm::vec3 position = cell.position + glm::vec3(0.0f, 0.45f + 0.14f * std::sin(t * 2.0f), 0.0f);
            setPart(cell.part, glm::rotate(glm::translate(glm::mat4(1.0f), position), t * 1.3f, glm::vec3(0, 1, 0)));
        }
        const int online = podsOnline();
        for (int i = 0; i < kPods; ++i) showPart(m_podGlowParts[std::size_t(i)], i < online);
        applyOutlines();
    }

    void updateLights(const WalkerState& walker) {
        render::Scene& scene = *m_scene;
        const glm::vec3 towardSun = render::skyDirection(m_sunElevation, kSunAzimuth);
        if (m_sunElevation != m_shownSunElevation) {
            m_shownSunElevation = m_sunElevation;
            const render::Daylight day = render::daylightFor(towardSun);
            scene.setLight(m_sun,
                           {.type = render::LightType::Directional, .color = day.sunColor, .direction = -towardSun});
            scene.setEnvironment(day.environment);
        }
        const float dark = night();
        const int online = podsOnline();
        const float boost = online >= kPods ? 1.5f : 1.0f; // the outpost is online: everything shines
        for (std::size_t i = 0; i < m_lampLights.size(); ++i)
            scene.setLight(m_lampLights[i], {.color = {0.35f, 0.85f, 1.0f},
                                             .intensity = (2.0f + 26.0f * dark) * boost,
                                             .position = m_level.lamps[i],
                                             .range = 15.0f,
                                             .castsShadows = false});
        for (std::size_t i = 0; i < m_bayLights.size(); ++i)
            scene.setLight(m_bayLights[i], {.color = {0.62f, 0.86f, 1.0f},
                                            .intensity = (40.0f - 20.0f * dark) * boost,
                                            .position = m_level.interiorLights[i],
                                            .range = 9.0f,
                                            .castsShadows = false});
        for (std::size_t i = 0; i < m_podLights.size(); ++i)
            scene.setLight(m_podLights[i], {.color = {0.3f, 0.85f, 1.0f},
                                            .intensity = int(i) < online ? 14.0f + 10.0f * dark : 0.0f,
                                            .position = m_level.pods[i] + glm::vec3(0.0f, 0.2f, 2.4f),
                                            .range = 8.0f,
                                            .castsShadows = false});
        const glm::vec3 forward(std::sin(walker.heading), 0.0f, std::cos(walker.heading));
        const float height = m_level.ankle + kLegHeight - m_level.hip.y;
        scene.setLight(m_headLight,
                       {.type = render::LightType::Spot,
                        .color = {0.75f, 0.92f, 1.0f},
                        .intensity = 60.0f * dark,
                        .position = walker.position + glm::vec3(0.0f, height + 0.2f, 0.0f) + forward * 1.1f,
                        .direction = glm::normalize(forward + glm::vec3(0.0f, -0.42f, 0.0f)),
                        .range = 18.0f,
                        .innerAngle = 0.3f,
                        .outerAngle = 0.55f,
                        .castsShadows = false});
    }

    Level m_level;
    std::optional<render::Renderer> m_renderer;
    std::optional<render::Scene> m_scene;
    std::optional<render::SceneAssets> m_sceneAssets;
    Model m_base, m_podGlow, m_bodyModel, m_thighModel, m_shinModel, m_footModel, m_cellModel;
    std::vector<Part> m_parts;
    std::array<PartId, kPods> m_podGlowParts{};
    PartId m_body = 0;
    std::array<PartId, 2> m_thigh{}, m_shin{}, m_foot{};
    std::array<PartId, kCapacity> m_carriedParts{};
    std::array<render::InstanceId, kMarkerParts> m_markerParts{};
    render::LightId m_sun, m_headLight;
    std::vector<render::LightId> m_lampLights, m_bayLights, m_podLights;
    std::optional<render::ParticleSystemId> m_sparks;

    // The mesh before what uses it.
    std::optional<navigation::NavMesh> m_navMesh;
    std::optional<navigation::NavQuery> m_query;
    std::optional<navigation::Crowd> m_crowd;
    navigation::AgentId m_agent;

    app::ActionId m_left = 0, m_right = 0, m_forward = 0, m_back = 0, m_turnLeft = 0, m_turnRight = 0, m_toggleTime = 0,
                  m_toggleAutopilot = 0;

    WalkerState m_walker, m_previous;
    float m_speed = 0.0f;
    std::array<Cell, kMaxCells> m_cells;
    const Cell* m_goalCell = nullptr;
    float m_stuck = 0.0f; // s the walker has stood still on its way to a cell
    Goal m_goal = Goal::None;
    bool m_autopilot = false;
    int m_carried = 0;
    int m_delivered = 0;
    double m_simTime = 0.0;
    std::uint64_t m_seed = 20261110;
    glm::vec2 m_marker{0.0f}; // x, z
    double m_markerTime = 0.0;

    float m_azimuth = kPi / 4.0f, m_azimuthTarget = kPi / 4.0f; // radians about +y from +z
    float m_elevation = 33.0f;                                  // degrees
    float m_distance = 108.0f;                                  // m
    std::optional<glm::vec3> m_look;                            // a scenario's camera target
    glm::vec3 m_target{1.5f, 3.0f, 0.0f}, m_eye{0.0f};
    glm::mat4 m_view{1.0f};
    glm::vec2 m_windowSize{1280.0f, 720.0f};
    bool m_cut = true;
    float m_sunElevation = kDayElevation, m_sunTarget = kDayElevation, m_shownSunElevation = -1.0f;
    bool m_rayTracing = false, m_rayTracingAvailable = false;
    bool m_outlines = true, m_outlinesShown = false;
    bool m_occlusion = true, m_sharpen = true; // switched off by scenario commands, to tell their part of the picture
    int m_occlusionRays = render::RenderSettings{}.occlusionRays;
};

} // namespace

Result<std::unique_ptr<quench::app::Game>> quench::app::createGame(AppDesc& desc,
                                                                   std::span<const std::string_view> args) {
    desc.name = QUENCH_GAME_NAME;
    desc.organization = "";
    desc.version = QUENCH_GAME_VERSION;
    bool autopilot = false;
    for (const std::string_view arg : args) {
        if (arg == "--help") {
            std::printf("%s options:\n  --autopilot  the walker gathers and delivers cells by itself\n",
                        QUENCH_GAME_NAME);
            continue;
        }
        if (arg == "--autopilot") {
            autopilot = true;
            continue;
        }
        return Error{std::format("unknown argument '{}' (--help lists the options)", arg)};
    }
    return std::unique_ptr<Game>(std::make_unique<Outpost>(autopilot));
}
