// ECS: a swarm kept by quench_ecs (docs/ecs.md). Every mover is a Flecs entity made of plain components (a position,
// a velocity, a lifetime, the scene instance that draws it); systems of the fixed step spawn, steer, move, charge and
// age them, a system of the frame writes their transforms into the scene, and observers keep the scene in step with
// the world: an entity that loses its Visual gives its instance back, one that gains or loses Charged changes colour.
// The controls switch systems off and on while they run, and the numbers show what Flecs holds.

#include "Demo.h"

#include <quench/ecs/World.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

// ---- components: plain data; the tags have none.
struct Position {
    glm::vec3 value{0.0f};
};
struct Velocity {
    glm::vec3 value{0.0f};
};
struct Lifetime {
    float left = 0.0f; // seconds
};
// The engine handle that draws the entity: engine APIs take handles, components hold them.
struct Visual {
    InstanceId instance;
    float size = 1.0f;
};
struct Charged {
    float left = 0.0f; // seconds until the charge is gone
};
struct Seeker {};  // flies after the beacon
struct Drifter {}; // keeps its course and bounces off the walls

constexpr glm::vec3 kArena{14.0f, 5.0f, 9.0f}; // half extents of the box the swarm lives in; the floor is y = 0
constexpr float kChargeRadius = 3.0f;
constexpr float kChargeSeconds = 1.6f;
constexpr int kMaxTarget = 4000;
constexpr glm::vec4 kSeekerColor{0.20f, 0.75f, 0.95f, 1.0f};
constexpr glm::vec4 kDrifterColor{0.95f, 0.62f, 0.18f, 1.0f};
constexpr glm::vec4 kChargedColor{1.0f, 0.25f, 0.75f, 1.0f};

class EcsSection final : public Section {
public:
    EcsSection() {
        camera.target = glm::vec3(0.0f, 2.2f, 0.0f);
        camera.distance = 26.0f;
        camera.yaw = glm::radians(28.0f);
        camera.pitch = glm::radians(30.0f);
        camera.maxDistance = 70.0f;
    }

    std::string_view id() const override { return "ecs"; }
    std::string_view title() const override { return "ECS"; }
    std::string_view about() const override {
        return "A swarm of Flecs entities: blue seekers chase the beacon, orange drifters bounce around, and those "
               "that come near it stay charged (pink) for a moment. Every entity lives a few seconds and is replaced. "
               "Switch a system off and its part of the behaviour stops; the numbers show the world.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(kArena.x + 6.0f, kArena.z + 6.0f), 20.0f));
        const Result<MeshId> dart = addMesh(s, boxMesh(glm::vec3(0.07f, 0.07f, 0.22f)));
        const Result<MeshId> ball = addMesh(s, sphereMesh(0.14f, 8, 12));
        const Result<MeshId> beacon = addMesh(s, sphereMesh(0.45f, 24, 48));
        for (const Result<MeshId>* m : {&ground, &dart, &ball, &beacon})
            if (!*m) return Error{m->error()};
        m_dart = *dart;
        m_ball = *ball;

        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.20f, 0.22f, 0.26f),
                                                      glm::vec3(0.15f, 0.16f, 0.19f), 2.0f, 0.6f)});
        // One material for all movers: their colour is the instance's.
        m_moverMaterial = s.addMaterial({.baseColor = glm::vec4(1.0f), .roughness = 0.45f});
        m_beacon = s.addInstance({.mesh = *beacon,
                                  .material = s.addMaterial({.baseColor = glm::vec4(1.0f, 0.3f, 0.8f, 1.0f),
                                                             .emissive = glm::vec3(6.0f, 1.2f, 4.5f),
                                                             .roughness = 0.3f}),
                                  .castsShadows = false});
        m_beaconLight = s.addLight({.type = LightType::Point,
                                    .color = glm::vec3(1.0f, 0.35f, 0.8f),
                                    .intensity = 40.0f,
                                    .range = 9.0f,
                                    .castsShadows = false});
        m_sun = s.addLight({.type = LightType::Directional});
        setDaylight(s, m_sun, 38.0f, 40.0f);

        m_world.emplace();
        buildWorld();
        moveBeacon(0.0);
        return {};
    }

    void fixedStep(Context& c, double dt) override {
        (void)c;
        m_time += dt;
        moveBeacon(m_time);
        m_world->fixedStep(float(dt));
    }

    void update(Context& c, double frameDt) override {
        (void)c;
        if (frameDt > 0.0) m_world->update(float(frameDt));
        // Time per fixed step and per update over the last frame, for the panel.
        const ecs::Stats now = m_world->stats();
        if (now.fixedSteps > m_last.fixedSteps)
            m_stepMicros =
                1.0e6 * (now.fixedStepSeconds - m_last.fixedStepSeconds) / double(now.fixedSteps - m_last.fixedSteps);
        if (now.updates > m_last.updates)
            m_updateMicros = 1.0e6 * (now.updateSeconds - m_last.updateSeconds) / double(now.updates - m_last.updates);
        m_last = now;
    }

    void controls(Context& c) override {
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::SliderInt("Entities", &m_target, 0, kMaxTarget);
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::SliderFloat("Lifetime (s)", &m_lifetime, 1.0f, 20.0f, "%.0f");
        if (ImGui::Button("Burst of 500")) burst(500);
        ImGui::SameLine();
        if (ImGui::Button("Clear")) clear();
        ImGui::Separator();
        ImGui::TextUnformatted("Systems");
        if (ImGui::Checkbox("Steer (seekers follow the beacon)", &m_steerOn)) apply();
        if (ImGui::Checkbox("Charge (near the beacon)", &m_chargeOn)) apply();
        if (ImGui::Checkbox("Age (entities end)", &m_ageOn)) apply();
        if (ImGui::Checkbox("Spawn (up to Entities)", &m_spawnOn)) apply();
        (void)c;
    }

    void stats(Context& c) override {
        const ecs::Stats s = m_world->stats();
        ImGui::TextWrapped("%d movers (%d charged), %llu made, %llu ended", movers(), charged(),
                           static_cast<unsigned long long>(m_spawned), static_cast<unsigned long long>(m_removed));
        ImGui::TextWrapped("World: %d entities, %d tables, %d systems (%d in the fixed step)", s.entities, s.tables,
                           s.systems, s.fixedSystems);
        // Wall times differ between runs: not in a deterministic run's picture.
        if (!c.app.deterministic())
            ImGui::TextWrapped("Fixed step %.0f us, update %.0f us", m_stepMicros, m_updateMicros);
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (!m_world) return std::nullopt;
        if (name == "movers") return double(movers());
        if (name == "charged") return double(charged());
        if (name == "seekers") return double(m_world->flecs().count<Seeker>());
        if (name == "drifters") return double(m_world->flecs().count<Drifter>());
        if (name == "spawned") return double(m_spawned);
        if (name == "removed") return double(m_removed);
        // Scene instances the movers hold: made minus given back. `leaked`: those no mover holds any more (0).
        if (name == "held") return double(m_spawned - m_removed);
        if (name == "leaked") return double(m_spawned - m_removed) - double(movers());
        if (name == "tables") return double(m_world->stats().tables);
        if (name == "fixed_systems") return double(m_world->stats().fixedSystems);
        if (name == "target") return double(m_target);
        if (name == "beacon_speed") return double(m_beaconSpeed);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "target" && number) {
            m_target = std::clamp(int(*number), 0, kMaxTarget);
        } else if (name == "lifetime" && number) {
            m_lifetime = std::clamp(*number, 0.2f, 60.0f);
        } else if (name == "burst" && number) {
            burst(std::clamp(int(*number), 0, kMaxTarget));
        } else if (name == "clear" && flag) {
            if (*flag) clear();
        } else if (name == "steer" && flag) {
            m_steerOn = *flag;
        } else if (name == "charge" && flag) {
            m_chargeOn = *flag;
        } else if (name == "age" && flag) {
            m_ageOn = *flag;
        } else if (name == "spawn" && flag) {
            m_spawnOn = *flag;
        } else if (name == "beacon_speed" && number) {
            m_beaconSpeed = *number;
        } else {
            return false;
        }
        apply();
        return true;
    }

    std::string perfDetails() const override {
        return "movers=" + std::to_string(movers()) + " ecs_step_us=" + std::to_string(int(m_stepMicros)) +
               " ecs_update_us=" + std::to_string(int(m_updateMicros));
    }

    void shutdown(Context& c) override {
        (void)c;
        // The world first: its entities give their instances back to the scene as they go.
        m_world.reset();
        m_scene.reset();
    }

private:
    int movers() const { return m_world->flecs().count<Visual>(); }
    int charged() const { return m_world->flecs().count<Charged>(); }

    void buildWorld() {
        flecs::world& w = m_world->flecs();
        w.component<Position>();
        w.component<Velocity>();
        w.component<Lifetime>();
        w.component<Visual>();
        w.component<Charged>();
        w.component<Seeker>();
        w.component<Drifter>();

        // ---- observers: the scene follows the world.
        // An entity that loses its Visual (it ended, or the world did) gives the instance back.
        w.observer<const Visual>("ReleaseInstance").event(flecs::OnRemove).each([this](const Visual& visual) {
            if (m_scene->contains(visual.instance)) m_scene->removeInstance(visual.instance);
            ++m_removed;
        });
        w.observer<const Visual>("ShowCharge").with<Charged>().event(flecs::OnAdd).each([this](const Visual& visual) {
            if (m_scene->contains(visual.instance)) m_scene->setInstanceColor(visual.instance, kChargedColor);
        });
        w.observer<const Visual>("HideCharge")
            .with<Charged>()
            .event(flecs::OnRemove)
            .each([this](flecs::entity e, const Visual& visual) {
                if (m_scene->contains(visual.instance))
                    m_scene->setInstanceColor(visual.instance, e.has<Seeker>() ? kSeekerColor : kDrifterColor);
            });

        // ---- the fixed step, in this order.
        m_spawn = w.system("Spawn").kind<ecs::FixedStep>().run([this](flecs::iter& it) {
            m_spawnBudget = std::min(m_spawnBudget + m_spawnRate * it.delta_time(), 400.0f);
            int room = m_target - movers();
            while (m_spawnBudget >= 1.0f && room > 0) {
                spawnOne();
                m_spawnBudget -= 1.0f;
                --room;
            }
        });
        m_steer = w.system<Velocity, const Position>("Steer").kind<ecs::FixedStep>().with<Seeker>().each(
            [this](flecs::iter& it, size_t, Velocity& v, const Position& p) {
                const glm::vec3 to = m_beaconPosition - p.value;
                const float distance = glm::length(to);
                if (distance > 1.0e-3f) v.value += to / distance * (9.0f * it.delta_time());
                const float speed = glm::length(v.value);
                if (speed > 7.0f) v.value *= 7.0f / speed;
            });
        w.system<Position, Velocity>("Move").kind<ecs::FixedStep>().each(
            [](flecs::iter& it, size_t, Position& p, Velocity& v) {
                p.value += v.value * it.delta_time();
                const glm::vec3 lo{-kArena.x, 0.3f, -kArena.z};
                const glm::vec3 hi{kArena.x, kArena.y, kArena.z};
                for (int axis = 0; axis < 3; ++axis) {
                    if (p.value[axis] < lo[axis]) {
                        p.value[axis] = lo[axis];
                        v.value[axis] = std::abs(v.value[axis]);
                    } else if (p.value[axis] > hi[axis]) {
                        p.value[axis] = hi[axis];
                        v.value[axis] = -std::abs(v.value[axis]);
                    }
                }
            });
        // Adding and removing a component moves the entity to another table: deferred to the end of the system.
        m_charge = w.system<const Position>("Charge").kind<ecs::FixedStep>().without<Charged>().each(
            [this](flecs::entity e, const Position& p) {
                const glm::vec3 d = p.value - m_beaconPosition;
                if (glm::dot(d, d) < kChargeRadius * kChargeRadius) e.set<Charged>({kChargeSeconds});
            });
        w.system<Charged>("Discharge").kind<ecs::FixedStep>().each([](flecs::iter& it, size_t row, Charged& charged) {
            charged.left -= it.delta_time();
            if (charged.left <= 0.0f) it.entity(row).remove<Charged>();
        });
        m_age =
            w.system<Lifetime>("Age").kind<ecs::FixedStep>().each([](flecs::iter& it, size_t row, Lifetime& lifetime) {
                lifetime.left -= it.delta_time();
                if (lifetime.left <= 0.0f) it.entity(row).destruct();
            });

        // ---- the frame: what the renderer needs, once per frame however many fixed steps ran.
        w.system<const Position, const Velocity, const Visual>("Draw")
            .kind(flecs::OnStore)
            .each([this](const Position& p, const Velocity& v, const Visual& visual) {
                const float yaw = std::atan2(v.value.x, v.value.z);
                m_scene->setTransform(visual.instance, placed(p.value, glm::vec3(visual.size), yaw));
            });
    }

    void spawnOne() {
        const bool seeker = m_random.uniform() < 0.6f;
        const float x = m_random.uniform(-kArena.x, kArena.x);
        const float y = m_random.uniform(0.6f, kArena.y);
        const float z = m_random.uniform(-kArena.z, kArena.z);
        const float angle = m_random.uniform(0.0f, 6.2831853f);
        const float speed = m_random.uniform(1.5f, 4.5f);
        const float climb = m_random.uniform(-0.8f, 0.8f);
        const float size = m_random.uniform(0.8f, 1.3f);
        const float life = m_lifetime * m_random.uniform(0.6f, 1.0f);
        const glm::vec3 position{x, y, z};
        const glm::vec3 velocity{std::sin(angle) * speed, climb, std::cos(angle) * speed};
        const InstanceId instance = m_scene->addInstance({.mesh = seeker ? m_dart : m_ball,
                                                          .material = m_moverMaterial,
                                                          .transform = placed(position, glm::vec3(size), angle),
                                                          .color = seeker ? kSeekerColor : kDrifterColor,
                                                          .castsShadows = false});
        flecs::entity e = m_world->flecs()
                              .entity()
                              .set<Position>({position})
                              .set<Velocity>({velocity})
                              .set<Lifetime>({life})
                              .set<Visual>({instance, size});
        if (seeker)
            e.add<Seeker>();
        else
            e.add<Drifter>();
        ++m_spawned;
    }

    void burst(int count) {
        for (int i = 0; i < count && movers() < kMaxTarget; ++i) spawnOne();
    }

    void clear() {
        m_world->flecs().delete_with<Visual>();
        m_spawnBudget = 0.0f;
    }

    // The controls' switches: a disabled system stays in the world and is skipped.
    void apply() {
        const auto enable = [](flecs::system system, bool on) {
            if (on)
                system.enable();
            else
                system.disable();
        };
        enable(m_spawn, m_spawnOn);
        enable(m_steer, m_steerOn);
        enable(m_charge, m_chargeOn);
        enable(m_age, m_ageOn);
    }

    void moveBeacon(double time) {
        const float t = float(time) * m_beaconSpeed;
        m_beaconPosition =
            glm::vec3(std::cos(t * 0.7f) * 9.0f, 2.4f + std::sin(t * 1.3f) * 1.2f, std::sin(t * 0.9f) * 5.5f);
        m_scene->setTransform(m_beacon, placed(m_beaconPosition));
        m_scene->setLight(m_beaconLight, {.type = LightType::Point,
                                          .color = glm::vec3(1.0f, 0.35f, 0.8f),
                                          .intensity = 40.0f,
                                          .position = m_beaconPosition,
                                          .range = 9.0f,
                                          .castsShadows = false});
    }

    // The scene before the world: the world's entities give instances back to it when the world goes.
    std::optional<Scene> m_scene;
    std::optional<ecs::World> m_world;
    flecs::system m_spawn, m_steer, m_charge, m_age;
    MeshId m_dart, m_ball;
    MaterialId m_moverMaterial;
    InstanceId m_beacon;
    LightId m_beaconLight, m_sun;
    glm::vec3 m_beaconPosition{0.0f};
    Random m_random;
    double m_time = 0.0;
    int m_target = 600;
    float m_lifetime = 8.0f;
    float m_spawnRate = 900.0f; // entities per second
    float m_spawnBudget = 0.0f;
    float m_beaconSpeed = 1.0f;
    bool m_spawnOn = true, m_steerOn = true, m_chargeOn = true, m_ageOn = true;
    std::uint64_t m_spawned = 0, m_removed = 0;
    ecs::Stats m_last;
    double m_stepMicros = 0.0, m_updateMicros = 0.0;
};

} // namespace

std::unique_ptr<Section> makeEcsSection() { return std::make_unique<EcsSection>(); }

} // namespace demo
