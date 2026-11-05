// Physics: quench::PhysicsWorld (Jolt) with a stack of boxes and a row of balls on a fenced floor. Drop more bodies,
// push them with a blast (a click or tap on the floor, or at the center), reset. The simulation runs in fixed ticks
// of 1/120 s; the drawn transforms interpolate between the last two ticks.

#include "Demo.h"

#include <quench/physics/PhysicsWorld.h>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <memory>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
using app::Binding;
using app::GamepadButton;

constexpr float kBoxHalf = 0.35f;
constexpr float kBallRadius = 0.3f;
constexpr float kFloorHalf = 7.0f;
constexpr std::size_t kMaxBodies = 400;

const std::array<glm::vec4, 5> kPalette{glm::vec4(0.85f, 0.32f, 0.12f, 1.0f), glm::vec4(0.95f, 0.72f, 0.2f, 1.0f),
                                        glm::vec4(0.2f, 0.55f, 0.85f, 1.0f), glm::vec4(0.3f, 0.7f, 0.35f, 1.0f),
                                        glm::vec4(0.8f, 0.8f, 0.82f, 1.0f)};

glm::vec3 toGlm(JPH::RVec3Arg v) { return {float(v.GetX()), float(v.GetY()), float(v.GetZ())}; }
glm::quat toGlm(JPH::QuatArg q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }

class PhysicsSection final : public Section {
public:
    PhysicsSection() {
        camera.target = glm::vec3(0.0f, 1.0f, 0.0f);
        camera.distance = 13.0f;
        camera.yaw = glm::radians(30.0f);
        camera.pitch = glm::radians(30.0f);
    }

    std::string_view id() const override { return "physics"; }
    std::string_view title() const override { return "Physics"; }
    std::string_view about() const override {
        return "Jolt through quench::PhysicsWorld, stepped at 120 Hz and drawn interpolated. Drop boxes (B, gamepad "
               "X) and balls (N, gamepad Y), blast them (click or tap the floor; P or gamepad B at the center), reset "
               "(R, gamepad Back).";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> floor = addMesh(s, planeMesh(glm::vec2(kFloorHalf), kFloorHalf));
        const Result<MeshId> rail = addMesh(s, boxMesh(glm::vec3(kFloorHalf + 0.2f, 0.25f, 0.2f)));
        const Result<MeshId> box = addMesh(s, boxMesh(glm::vec3(kBoxHalf)));
        const Result<MeshId> ball = addMesh(s, sphereMesh(kBallRadius, 16, 32));
        for (const Result<MeshId>* m : {&floor, &rail, &box, &ball})
            if (!*m) return Error{m->error()};
        m_boxMesh = *box;
        m_ballMesh = *ball;
        s.addInstance(
            {.mesh = *floor,
             .material = addCheckerMaterial(c, s, glm::vec3(0.5f, 0.5f, 0.52f), glm::vec3(0.3f, 0.3f, 0.33f), 1.0f)});
        const MaterialId railMaterial =
            s.addMaterial({.baseColor = glm::vec4(0.25f, 0.27f, 0.3f, 1.0f), .roughness = 0.6f});
        m_bodyMaterial = s.addMaterial({.roughness = 0.45f, .name = "body"});
        const glm::vec3 towardSun = glm::normalize(glm::vec3(0.4f, 0.85f, 0.3f));
        s.addLight({.type = LightType::Directional, .color = glm::vec3(5.0f, 4.8f, 4.4f), .direction = -towardSun});

        m_world = std::make_unique<PhysicsWorld>();
        JPH::BodyInterface& bodies = m_world->bodies();
        bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(kFloorHalf, 0.5f, kFloorHalf)),
                                                          JPH::RVec3(0.0f, -0.5f, 0.0f), JPH::Quat::sIdentity(),
                                                          JPH::EMotionType::Static, PhysicsLayers::kStatic),
                                JPH::EActivation::DontActivate);
        // Rails along the four edges keep the bodies on the floor.
        for (int i = 0; i < 4; ++i) {
            const float angle = glm::radians(90.0f * float(i));
            const glm::vec3 p =
                glm::vec3(std::sin(angle), 0.25f, std::cos(angle)) * glm::vec3(kFloorHalf, 1, kFloorHalf);
            bodies.CreateAndAddBody(
                JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(kFloorHalf + 0.2f, 0.25f, 0.2f)),
                                          JPH::RVec3(p.x, p.y, p.z), JPH::Quat::sRotation(JPH::Vec3::sAxisY(), angle),
                                          JPH::EMotionType::Static, PhysicsLayers::kStatic),
                JPH::EActivation::DontActivate);
            s.addInstance({.mesh = *rail, .material = railMaterial, .transform = placed(p, glm::vec3(1.0f), angle)});
        }

        app::Input& input = c.app.input();
        m_dropBox = input.addAction("drop_box", {*Binding::key("B"), Binding::gamepad(GamepadButton::West)});
        m_dropBall = input.addAction("drop_ball", {*Binding::key("N"), Binding::gamepad(GamepadButton::North)});
        m_blast = input.addAction("blast", {*Binding::key("P"), Binding::gamepad(GamepadButton::East)});
        m_reset = input.addAction("reset", {*Binding::key("R"), Binding::gamepad(GamepadButton::Back)});
        reset();
        return {};
    }

    void fixedStep(Context& c, double dt) override {
        (void)c;
        for (Body& b : m_bodies) {
            b.previousPosition = b.position;
            b.previousRotation = b.rotation;
        }
        m_world->step(float(dt));
        readBodies();
    }

    void update(Context& c, double frameDt) override {
        (void)frameDt;
        const app::Input& input = c.app.input();
        if (input.pressed(m_dropBox)) drop(false);
        if (input.pressed(m_dropBall)) drop(true);
        if (input.pressed(m_blast)) blast(glm::vec3(0.0f));
        if (input.pressed(m_reset)) reset();
        const float alpha = float(c.app.alpha());
        for (const Body& b : m_bodies) {
            const glm::vec3 p = glm::mix(b.previousPosition, b.position, alpha);
            const glm::quat q = glm::slerp(b.previousRotation, b.rotation, alpha);
            m_scene->setTransform(b.instance, glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q));
        }
    }

    void ui(Context& c) override {
        (void)c;
        if (ImGui::Button("Drop a box")) drop(false);
        ImGui::SameLine();
        if (ImGui::Button("Drop a ball")) drop(true);
        if (ImGui::Button("Blast")) blast(glm::vec3(0.0f));
        ImGui::SameLine();
        if (ImGui::Button("Reset")) reset();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::SliderFloat("Blast strength", &m_blastStrength, 1.0f, 20.0f, "%.0f m/s");
        ImGui::Text("%zu bodies, %u awake", m_bodies.size(),
                    m_world->system().GetNumActiveBodies(JPH::EBodyType::RigidBody));
        ImGui::TextWrapped("Highest %.2f m, fastest %.2f m/s", double(highest()), double(fastest()));
    }

    void click(Context& c, glm::vec2 point) override {
        glm::vec3 origin, direction;
        camera.ray(point, glm::vec2(float(c.app.windowWidth()), float(c.app.windowHeight())), origin, direction);
        if (direction.y >= -1e-3f) return;
        const glm::vec3 hit = origin + direction * (-origin.y / direction.y);
        if (std::abs(hit.x) < kFloorHalf && std::abs(hit.z) < kFloorHalf) blast(hit);
    }

    // `floor`: the floor just in front of the stack, where a click blasts it (`click @floor` in a scenario).
    std::optional<glm::vec2> point(const Context& c, std::string_view name) const override {
        if (name != "floor") return std::nullopt;
        return camera.project(glm::vec3(0.4f, 0.0f, 0.2f),
                              glm::vec2(float(c.app.windowWidth()), float(c.app.windowHeight())));
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (name == "bodies") return double(m_bodies.size());
        if (name == "awake") return double(m_world->system().GetNumActiveBodies(JPH::EBodyType::RigidBody));
        if (name == "highest") return double(highest());
        if (name == "fastest") return double(fastest());
        if (name == "blasts") return double(m_blasts);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        if (name == "blast_strength" && number) {
            m_blastStrength = std::max(*number, 0.0f);
            return true;
        }
        return false;
    }

    void shutdown(Context& c) override {
        (void)c;
        clear();
        m_world.reset();
        m_scene.reset();
    }

private:
    struct Body {
        JPH::BodyID id;
        InstanceId instance;
        glm::vec3 position{0.0f}, previousPosition{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}, previousRotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    void add(bool ball, const glm::vec3& position, const glm::quat& rotation) {
        if (m_bodies.size() >= kMaxBodies) removeOldest();
        JPH::BodyCreationSettings settings(
            ball ? static_cast<const JPH::ShapeSettings*>(new JPH::SphereShapeSettings(kBallRadius))
                 : static_cast<const JPH::ShapeSettings*>(new JPH::BoxShapeSettings(JPH::Vec3::sReplicate(kBoxHalf))),
            JPH::RVec3(position.x, position.y, position.z), JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w),
            JPH::EMotionType::Dynamic, PhysicsLayers::kMoving);
        settings.mRestitution = ball ? 0.4f : 0.1f;
        settings.mFriction = 0.6f;
        Body b;
        b.id = m_world->bodies().CreateAndAddBody(settings, JPH::EActivation::Activate);
        if (b.id.IsInvalid()) return; // the world is full
        b.position = b.previousPosition = position;
        b.rotation = b.previousRotation = rotation;
        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation);
        b.instance = m_scene->addInstance({.mesh = ball ? m_ballMesh : m_boxMesh,
                                           .material = m_bodyMaterial,
                                           .transform = transform,
                                           .color = kPalette[m_colorIndex++ % kPalette.size()]});
        m_bodies.push_back(b);
    }

    void removeOldest() {
        const Body& b = m_bodies.front();
        m_world->bodies().RemoveBody(b.id);
        m_world->bodies().DestroyBody(b.id);
        m_scene->removeInstance(b.instance);
        m_bodies.pop_front();
    }

    void clear() {
        while (!m_bodies.empty()) removeOldest();
    }

    // A pyramid of boxes and a row of balls, the same every time.
    void reset() {
        clear();
        m_random = Random{};
        m_colorIndex = 0;
        const float step = kBoxHalf * 2.0f + 0.02f;
        for (int row = 0; row < 5; ++row)
            for (int i = 0; i < 5 - row; ++i) {
                const float x = (float(i) - float(4 - row) * 0.5f) * step;
                add(false, glm::vec3(x, kBoxHalf + float(row) * kBoxHalf * 2.0f, -1.0f), glm::quat(1, 0, 0, 0));
            }
        for (int i = 0; i < 6; ++i) add(true, glm::vec3(-2.5f + float(i), kBallRadius, 2.0f), glm::quat(1, 0, 0, 0));
        m_world->optimizeBroadPhase();
    }

    void drop(bool ball) {
        const glm::vec3 p(m_random.uniform(-2.0f, 2.0f), 6.0f + m_random.uniform(0.0f, 1.0f),
                          m_random.uniform(-2.0f, 2.0f));
        const glm::vec3 axis = glm::normalize(glm::vec3(m_random.uniform(-1, 1), m_random.uniform(-1, 1), 0.3f));
        add(ball, p, glm::angleAxis(m_random.uniform(0.0f, 6.28f), axis));
    }

    // Pushes the bodies within 4 m of `center` away and upward, harder the closer they are.
    void blast(const glm::vec3& center) {
        ++m_blasts;
        JPH::BodyInterface& bodies = m_world->bodies();
        for (const Body& b : m_bodies) {
            const glm::vec3 d = b.position - center;
            const float distance = glm::length(d);
            if (distance > 4.0f) continue;
            const glm::vec3 away = distance > 1e-3f ? d / distance : glm::vec3(0.0f);
            const glm::vec3 v =
                glm::normalize(away + glm::vec3(0.0f, 1.2f, 0.0f)) * m_blastStrength * (1.0f - distance / 4.0f);
            bodies.AddLinearVelocity(b.id, JPH::Vec3(v.x, v.y, v.z));
            bodies.ActivateBody(b.id);
        }
    }

    void readBodies() {
        const JPH::BodyInterface& bodies = m_world->bodies();
        for (Body& b : m_bodies) {
            JPH::RVec3 p;
            JPH::Quat q;
            bodies.GetPositionAndRotation(b.id, p, q);
            b.position = toGlm(p);
            b.rotation = toGlm(q);
        }
    }

    float highest() const {
        float y = 0.0f;
        for (const Body& b : m_bodies) y = std::max(y, b.position.y);
        return y;
    }

    float fastest() const {
        float v = 0.0f;
        for (const Body& b : m_bodies) {
            const JPH::Vec3 velocity = m_world->bodies().GetLinearVelocity(b.id);
            v = std::max(v, velocity.Length());
        }
        return v;
    }

    std::optional<Scene> m_scene;
    std::unique_ptr<PhysicsWorld> m_world;
    std::deque<Body> m_bodies;
    MeshId m_boxMesh, m_ballMesh;
    MaterialId m_bodyMaterial;
    app::ActionId m_dropBox = 0, m_dropBall = 0, m_blast = 0, m_reset = 0;
    Random m_random;
    std::size_t m_colorIndex = 0;
    float m_blastStrength = 9.0f;
    int m_blasts = 0;
};

} // namespace

std::unique_ptr<Section> makePhysicsSection() { return std::make_unique<PhysicsSection>(); }

} // namespace demo
