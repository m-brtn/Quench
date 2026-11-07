// Navigation (quench_navigation, on Recast and Detour): a small level of two floors joined by a ramp, stairs and a
// narrow bridge with a passage under it (two layers of the navmesh above each other), a mud field that costs more to
// cross (an area type), jumps down two ledges (one-way off-mesh links), and two crates and a barrel that move (dynamic
// obstacles: the tiles under them are built again on jobs). The navmesh is built at run time from the level's
// triangles, tile by tile on the job system, across frames. Two groups of mannequins (the animation section's
// character: its walk and run blended by its agent's speed, its jump played on a link) patrol between waypoints or go
// where a click or tap on a floor sends the selected group, keeping out of each other's way (a crowd). A game pass
// draws the navmesh by area, the obstacles, the paths ahead and the agents from navigation::DebugGeometry.

#include "Demo.h"

#include <quench/animation/AnimationSystem.h>
#include <quench/assets/AssetManager.h>
#include <quench/core/Log.h>
#include <quench/navigation/Crowd.h>
#include <quench/navigation/DebugGeometry.h>
#include <quench/navigation/NavMesh.h>
#include <quench/navigation/NavQuery.h>
#include <quench/render/SceneAssets.h>

#include <demo_debug_shaders.h>

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
using animation::AnimationSystem;
namespace nav = quench::navigation;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDeck = 2.4f; // m: the upper floor
constexpr nav::AreaId kMud = 1;
constexpr float kAgentRadius = 0.35f;
constexpr float kAgentHeight = 1.8f;
constexpr int kGroups = 2;
constexpr int kMaxPerGroup = 12;
constexpr float kSpacing = 1.1f;       // m between the agents of a group at its target
constexpr float kArrival = 0.6f;       // m from its place in the group: there (the others press a little)
constexpr float kJumpArc = 0.45f;      // m: a jump down a ledge rises this much above the straight line, at its middle
constexpr float kObstacleStep = 0.25f; // m an obstacle moves before its tiles are built again
const std::array<glm::vec3, kGroups> kGroupColors{glm::vec3(1.0f, 0.5f, 0.08f), glm::vec3(0.3f, 1.0f, 0.4f)};
constexpr std::array<float, kGroups> kGroupSpeeds{2.0f, 1.4f}; // m/s: between the clips' walk (1.1) and run (2.7)

struct Waypoint {
    const char* name; // also a point for the scenario command `click @NAME`
    glm::vec3 position;
};
const std::array<Waypoint, 5> kWaypoints{{
    {"deck_west", {-9.0f, kDeck, -6.5f}},
    {"deck_east", {8.5f, kDeck, -6.5f}},
    {"south_east", {9.5f, 0.0f, 7.5f}},
    {"south_west", {-8.5f, 0.0f, 7.5f}},
    {"under_bridge", {-0.5f, 0.0f, -5.5f}},
}};
// Each group's round of waypoints: it starts at the last one.
const std::array<std::vector<int>, kGroups> kRounds{{{1, 2, 3, 0}, {3, 0, 4, 1, 2}}};

// The mud field's area box and a path across it that goes around it unless the mud is cheap.
const glm::vec3 kMudMin(-3.0f, -0.5f, 2.0f), kMudMax(4.0f, 0.5f, 7.0f);
const glm::vec3 kProbeStart(-4.6f, 0.0f, 6.0f), kProbeEnd(5.6f, 0.0f, 6.0f);

// An obstacle sliding back and forth: shape at offset travel * sin(rate * t + phase).
struct MovingObstacle {
    nav::Obstacle shape;
    glm::vec3 travel{0.0f};
    float rate = 0.5f; // rad/s
    float phase = 0.0f;
};
const std::array<MovingObstacle, 3> kObstacles{{
    {{.shape = nav::ObstacleShape::Box, .position = {-7.5f, 0.6f, 4.5f}, .halfExtents = glm::vec3(0.6f)},
     {2.0f, 0.0f, 0.0f},
     0.45f,
     0.0f},
    {{.shape = nav::ObstacleShape::Box, .position = {6.5f, 0.6f, 2.5f}, .halfExtents = glm::vec3(0.6f), .yaw = 0.5f},
     {0.0f, 0.0f, 1.8f},
     0.5f,
     1.0f},
    {{.shape = nav::ObstacleShape::Cylinder, .position = {0.5f, 0.0f, 8.6f}, .radius = 0.45f, .height = 1.1f},
     {3.0f, 0.0f, 0.0f},
     0.35f,
     2.0f},
}};

glm::mat4 yawed(const glm::vec3& position, float yaw) {
    return glm::rotate(glm::translate(glm::mat4(1.0f), position), yaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

// Slot k of a group at its target: the middle, then rings of 6, 12, 18 ... around it.
glm::vec3 formation(int k) {
    if (k == 0) return glm::vec3(0.0f);
    int ring = 1, first = 1;
    while (k >= first + 6 * ring) {
        first += 6 * ring;
        ++ring;
    }
    const float a = 2.0f * kPi * float(k - first) / float(6 * ring) + 0.5f * float(ring);
    return float(ring) * kSpacing * glm::vec3(std::cos(a), 0.0f, std::sin(a));
}

// The level: every face for navigation (Recast keeps what is walkable: the tops, the ramp, the steps) and the same
// faces, flat shaded in vertex colors, for the renderer.
struct Level {
    nav::Geometry geometry;
    MeshData mesh;

    // A convex polygon, counter-clockwise seen from its front; drawn unless navOnly.
    void polygon(std::initializer_list<glm::vec3> corners, glm::vec3 color, bool navOnly = false) {
        const std::vector<glm::vec3> p(corners);
        const glm::vec3 n = glm::normalize(glm::cross(p[1] - p[0], p[2] - p[0]));
        const glm::vec3 a = glm::abs(n);
        const auto navBase = std::uint32_t(geometry.vertices.size());
        const auto meshBase = std::uint32_t(mesh.vertices.size());
        for (const glm::vec3& v : p) {
            geometry.vertices.push_back(v);
            if (navOnly) continue;
            const glm::vec2 uv = a.y >= a.x && a.y >= a.z ? glm::vec2(v.x, v.z)
                                 : a.x >= a.z             ? glm::vec2(v.z, v.y)
                                                          : glm::vec2(v.x, v.y);
            mesh.vertices.push_back({.position = v, .normal = n, .uv = uv, .color = glm::vec4(color, 1.0f)});
        }
        for (std::uint32_t k = 2; k < p.size(); ++k)
            for (const std::uint32_t i : {0u, k - 1, k}) {
                geometry.indices.push_back(navBase + i);
                if (!navOnly) mesh.indices.push_back(meshBase + i);
            }
    }
    // Floor seen from above at height y over [x0, x1] x [z0, z1].
    void floor(float x0, float x1, float z0, float z1, float y, glm::vec3 color, bool navOnly = false) {
        polygon({{x0, y, z0}, {x0, y, z1}, {x1, y, z1}, {x1, y, z0}}, color, navOnly);
    }
    // A block standing on lo.y: its top and its four sides.
    void block(glm::vec3 lo, glm::vec3 hi, glm::vec3 top, glm::vec3 side) {
        floor(lo.x, hi.x, lo.z, hi.z, hi.y, top);
        polygon({{hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}}, side);
        polygon({{lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}}, side);
        polygon({{lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}}, side);
        polygon({{hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}}, side);
    }
    // A ramp over [x0, x1] from height h at z = high down to the ground at z = low (high < low): its slope and sides.
    void ramp(float x0, float x1, float high, float low, float h, glm::vec3 top, glm::vec3 side) {
        polygon({{x0, h, high}, {x0, 0.0f, low}, {x1, 0.0f, low}, {x1, h, high}}, top);
        polygon({{x0, 0.0f, high}, {x0, 0.0f, low}, {x0, h, high}}, side);
        polygon({{x1, 0.0f, low}, {x1, 0.0f, high}, {x1, h, high}}, side);
    }
};

// Where a ray first meets one of the level's triangles.
std::optional<glm::vec3> pick(const nav::Geometry& g, glm::vec3 origin, glm::vec3 direction) {
    float best = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i + 2 < g.indices.size(); i += 3) {
        const glm::vec3 a = g.vertices[g.indices[i]], b = g.vertices[g.indices[i + 1]],
                        c = g.vertices[g.indices[i + 2]];
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 p = glm::cross(direction, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-8f) continue;
        const glm::vec3 s = origin - a;
        const float u = glm::dot(s, p) / det;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(direction, q) / det;
        const float t = glm::dot(e2, q) / det;
        if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && t > 0.0f && t < best) best = t;
    }
    if (best == std::numeric_limits<float>::max()) return std::nullopt;
    return origin + direction * best;
}

// One mannequin and its agent.
struct Walker {
    int group = 0;
    nav::AgentId agent;
    animation::CharacterId character;
    ModelInstanceId model;
    int skin = -1;            // AnimationSystem::addSkin, once the model is ready
    glm::vec3 previous{0.0f}; // the agent's position before the last step and after it
    glm::vec3 position{0.0f};
    nav::AgentState state;
    float yaw = 0.0f;
    glm::mat4 transform{1.0f};
    bool fresh = true; // not drawn yet: its first transform is a teleport
};

struct Group {
    int count = 6;
    int leg = -1; // the waypoint of its round it walks to (-1: its start)
    glm::vec3 target{0.0f};
    bool hasTarget = false;
    bool reached = false; // every agent arrived or stopped at the target
    double legTime = 0.0;
    double waitTime = 0.0;
    int legsDone = 0;
};

class NavigationSection final : public Section {
public:
    NavigationSection() {
        camera.target = glm::vec3(0.0f, 0.6f, 0.5f);
        camera.distance = 24.0f;
        camera.yaw = glm::radians(18.0f);
        camera.pitch = glm::radians(44.0f);
        camera.maxDistance = 70.0f;
    }

    std::string_view id() const override { return "navigation"; }
    std::string_view title() const override { return "Navigation"; }
    std::string_view about() const override {
        return "A navmesh built at run time on jobs: two floors, a ramp, stairs, a narrow bridge, mud that costs "
               "more to cross, jumps down the ledges, and crates and a barrel that move (their tiles are built "
               "again). Two groups patrol or go where a click or tap sends them, keeping out of each other's way.";
    }

    Result<void> init(Context& c) override {
        m_device = &c.app.device();
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;

        const glm::vec3 stone(0.62f, 0.6f, 0.56f), wall(0.46f, 0.44f, 0.41f), wood(0.5f, 0.34f, 0.2f);
        Level level;
        // The ground, where it is open (not under the decks, the ramp or the stairs: no navmesh inside them).
        level.floor(-14.0f, 14.0f, 3.0f, 10.0f, 0.0f, stone, true);
        level.floor(-10.0f, 10.0f, -3.0f, 3.0f, 0.0f, stone, true);
        level.floor(10.0f, 14.0f, 1.2f, 3.0f, 0.0f, stone, true);
        level.floor(-4.0f, 3.0f, -10.0f, -3.0f, 0.0f, stone, true);
        // The decks, the bridge between them, the ramp up to the west one and the stairs up to the east one.
        level.block({-14.0f, 0.0f, -10.0f}, {-4.0f, kDeck, -3.0f}, stone, wall);
        level.block({3.0f, 0.0f, -10.0f}, {14.0f, kDeck, -3.0f}, stone, wall);
        level.block({-4.0f, kDeck - 0.2f, -7.7f}, {3.0f, kDeck, -6.3f}, wood, wood * 0.8f);
        level.ramp(-13.0f, -10.0f, -3.0f, 3.0f, kDeck, glm::vec3(0.58f, 0.52f, 0.44f), wall);
        for (int k = 1; k <= 7; ++k)
            level.block({10.0f, 0.0f, -3.0f + float(7 - k) * 0.6f},
                        {13.0f, 0.3f * float(k), -3.0f + float(8 - k) * 0.6f}, k % 2 ? stone : stone * 0.88f, wall);
        level.geometry.boxes.push_back({.min = kMudMin, .max = kMudMax, .area = kMud});
        // Jumps down the decks' south ledges, one way.
        level.geometry.links.push_back({.start = {-6.5f, kDeck, -3.8f}, .end = {-6.5f, 0.0f, -1.6f}, .id = 1});
        level.geometry.links.push_back({.start = {5.5f, kDeck, -3.8f}, .end = {5.5f, 0.0f, -1.6f}, .id = 2});
        m_pickGeometry = level.geometry;

        const Result<MeshId> levelMesh = addMesh(s, level.mesh);
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(14.0f, 10.0f), 14.0f));
        const Result<MeshId> mud = addMesh(s, planeMesh(glm::vec2(3.5f, 2.5f), 3.0f));
        const Result<MeshId> crate = addMesh(s, boxMesh(glm::vec3(0.6f)));
        const Result<MeshId> barrel = addMesh(s, cylinderMesh(0.45f, 0.55f, 32));
        const Result<MeshId> lid = addMesh(s, diskMesh(0.45f, 32));
        const Result<MeshId> rail = addMesh(s, boxMesh(glm::vec3(3.5f, 0.03f, 0.03f)));
        const Result<MeshId> post = addMesh(s, boxMesh(glm::vec3(0.03f, 0.45f, 0.03f)));
        for (const Result<MeshId>* m : {&levelMesh, &ground, &mud, &crate, &barrel, &lid, &rail, &post})
            if (!*m) return Error{m->error()};
        s.addInstance(
            {.mesh = *ground,
             .material = addCheckerMaterial(c, s, glm::vec3(0.6f, 0.58f, 0.54f), glm::vec3(0.5f, 0.48f, 0.45f), 1.0f),
             .transform = placed({0.0f, -0.002f, 0.0f})});
        s.addInstance({.mesh = *levelMesh, .material = s.addMaterial({.roughness = 0.85f, .name = "level"})});
        s.addInstance({.mesh = *mud,
                       .material = s.addMaterial(
                           {.baseColor = glm::vec4(0.2f, 0.13f, 0.07f, 1.0f), .roughness = 0.3f, .name = "mud"}),
                       .transform = placed((kMudMin + kMudMax) * 0.5f + glm::vec3(0.0f, 0.012f, 0.0f))});
        // Railings along the bridge (outside the agents' way: the navmesh keeps a radius from the edges).
        const MaterialId metal =
            s.addMaterial({.baseColor = glm::vec4(0.3f, 0.32f, 0.35f, 1.0f), .roughness = 0.4f, .metallic = 0.8f});
        for (const float z : {-7.66f, -6.34f}) {
            s.addInstance({.mesh = *rail, .material = metal, .transform = placed({-0.5f, kDeck + 0.9f, z})});
            for (int k = 0; k <= 4; ++k)
                s.addInstance({.mesh = *post,
                               .material = metal,
                               .transform = placed({-4.0f + 1.75f * float(k), kDeck + 0.45f, z})});
        }
        const MaterialId crateMaterial =
            s.addMaterial({.baseColor = glm::vec4(0.55f, 0.36f, 0.18f, 1.0f), .roughness = 0.7f, .name = "crate"});
        const MaterialId barrelMaterial = s.addMaterial(
            {.baseColor = glm::vec4(0.2f, 0.36f, 0.5f, 1.0f), .roughness = 0.45f, .metallic = 0.5f, .name = "barrel"});
        for (const MovingObstacle& o : kObstacles) {
            if (o.shape.shape == nav::ObstacleShape::Box) {
                m_obstacleInstances.push_back({s.addInstance({.mesh = *crate, .material = crateMaterial}), {}});
            } else {
                m_obstacleInstances.push_back({s.addInstance({.mesh = *barrel, .material = barrelMaterial}),
                                               s.addInstance({.mesh = *lid, .material = barrelMaterial})});
            }
        }
        setDaylight(s, s.addLight({.type = LightType::Directional}), 50.0f, 35.0f);

        // The navmesh: built on jobs, it fills in over the next frames.
        nav::BuildSettings build;
        build.agent = {.radius = kAgentRadius, .height = kAgentHeight, .maxClimb = 0.4f, .maxSlope = 45.0f};
        build.cellSize = 0.15f;
        build.cellHeight = 0.1f;
        build.tileSize = 48;
        build.areas = {{"ground", 1.0f}, {"mud", m_mudCost}};
        Result<nav::NavMesh> mesh = nav::NavMesh::build(std::move(level.geometry), build, c.app.jobs());
        if (!mesh) return Error{std::format("navmesh: {}", mesh.error())};
        m_mesh.emplace(std::move(*mesh));
        for (const MovingObstacle& o : kObstacles) m_obstacleIds.push_back(m_mesh->addObstacle(o.shape));
        m_obstacleApplied.resize(kObstacles.size());
        m_obstaclePositions.resize(kObstacles.size());
        m_crowd.emplace(*m_mesh,
                        nav::CrowdSettings{.maxAgents = kGroups * kMaxPerGroup, .maxAgentRadius = kAgentRadius});
        m_query.emplace(*m_mesh);
        applyFilter();

        // The animation section's character, on the section's own asset manager (as there).
        m_manager = std::make_unique<assets::AssetManager>(c.app.jobs(), c.app.assets().settings());
        m_assets.emplace(s, *m_manager);
        m_anim = std::make_unique<AnimationSystem>(*m_manager, c.app.jobs());
        m_model = m_assets->loadModel(mannequin::kModel);
        m_skeleton = m_anim->loadSkeleton(mannequin::kSkeleton);
        m_clips = m_anim->loadClips(mannequin::kClips);
        m_graph = m_anim->loadGraph(mannequin::kGraph);
        restart();
        return {};
    }

    void enter(Context& c) override {
        m_pass = c.renderer.addPass(PassStage::Transparent, "demo navigation debug",
                                    [this](PassContext& context) { drawDebug(context); });
    }

    void leave(Context& c) override {
        c.renderer.removePass(m_pass);
        m_pass = {};
    }

    void fixedStep(Context& c, double dt) override {
        (void)c;
        if (m_moveObstacles) {
            m_obstacleTime += dt;
            placeObstacles(false);
        }
        if (m_spawnPending) return;
        for (Walker& w : m_walkers) w.previous = w.position;
        const auto start = std::chrono::steady_clock::now();
        m_crowd->update(float(dt));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        m_crowdMs = m_crowdSteps == 0 ? ms : m_crowdMs * 0.95 + ms * 0.05;
        ++m_crowdSteps;
        for (const nav::CrowdEvent& e : m_crowd->events()) {
            if (e.type == nav::CrowdEvent::Type::LinkStarted) {
                ++m_linkUses;
                if (Walker* w = walkerOf(e.agent)) m_anim->trigger(w->character, "jump");
            } else if (e.type == nav::CrowdEvent::Type::LinkFinished) {
                ++m_landings;
            }
        }
        for (Walker& w : m_walkers) {
            w.state = m_crowd->state(w.agent);
            w.position = w.state.position;
        }
        measure();
        patrol(dt);
    }

    void update(Context& c, double frameDt) override {
        const float dt = std::min(float(frameDt), 0.1f);
        m_mesh->update();
        if (m_spawnPending && m_mesh->ready()) spawn();
        bindSkins();
        m_manager->update();
        m_assets->update();
        if (m_probeRevision != m_mesh->revision() || m_probeCost != m_mudCost) probeMud();

        // The characters where their agents are between the last two steps, facing the way they go.
        const float alpha = float(c.app.alpha());
        for (Walker& w : m_walkers) {
            glm::vec3 p = glm::mix(w.previous, w.position, alpha);
            glm::vec3 heading = w.state.velocity;
            float speed = glm::length(glm::vec2(heading.x, heading.z));
            if (w.state.move == nav::AgentMove::OnLink) {
                const float t = w.state.linkProgress;
                p.y += kJumpArc * 4.0f * t * (1.0f - t);
                heading = w.state.linkEnd - w.state.linkStart;
                speed = kGroupSpeeds[std::size_t(w.group)];
            }
            if (glm::length(glm::vec2(heading.x, heading.z)) > 0.05f) {
                const float wanted = std::atan2(heading.x, heading.z);
                const float turn = std::remainder(wanted - w.yaw, 2.0f * kPi);
                w.yaw += std::clamp(turn, -8.0f * dt, 8.0f * dt);
            }
            m_anim->setFloat(w.character, "speed", speed);
            w.transform = yawed(p, w.yaw);
            m_anim->setTransform(w.character, w.transform);
        }
        m_anim->update(dt);
        for (Walker& w : m_walkers) {
            if (w.skin >= 0) m_assets->setJointPalette(w.model, 0, m_anim->skinPalette(w.character, w.skin), w.fresh);
            m_assets->setTransform(w.model, w.transform, w.fresh);
            w.fresh = false;
        }
        for (std::size_t i = 0; i < kObstacles.size(); ++i) {
            const glm::vec3 p = m_obstaclePositions[i];
            const nav::Obstacle& shape = kObstacles[i].shape;
            if (shape.shape == nav::ObstacleShape::Box) {
                m_scene->setTransform(m_obstacleInstances[i].first, placed(p, glm::vec3(1.0f), shape.yaw));
            } else {
                m_scene->setTransform(m_obstacleInstances[i].first, placed(p + glm::vec3(0.0f, 0.55f, 0.0f)));
                m_scene->setTransform(m_obstacleInstances[i].second, placed(p + glm::vec3(0.0f, 1.1f, 0.0f)));
            }
        }
        updateDebug();
    }

    void prepare(Context& c, gpu::CommandBuffer& commands) override {
        m_drawTriangles = m_drawLines = 0;
        const std::size_t vertices = m_debug.triangles.size() + m_debug.lines.size();
        if (vertices == 0) return;
        static_assert(sizeof(nav::DebugVertex) == 28, "position and color, tightly packed");
        const auto bytes = std::uint32_t(vertices * sizeof(nav::DebugVertex));
        if (bytes > m_capacity) {
            std::uint32_t capacity = std::max(m_capacity, 64u * 1024u);
            while (capacity < bytes) capacity *= 2;
            Result<gpu::Buffer> buffer = c.app.device().createBuffer(
                {.usage = gpu::BufferUsage::Vertex, .size = capacity, .name = "demo navigation debug"});
            Result<gpu::TransferBuffer> transfer =
                c.app.device().createTransferBuffer({.size = capacity, .name = "demo navigation debug upload"});
            if (!buffer || !transfer) {
                log::error("demo", "navigation debug buffers: {}", !buffer ? buffer.error() : transfer.error());
                return;
            }
            m_vertices = std::move(*buffer);
            m_transfer = std::move(*transfer);
            m_capacity = capacity;
        }
        const std::span<std::byte> mapped = m_transfer.map(true);
        if (mapped.size() < bytes) return;
        const std::size_t triangleBytes = m_debug.triangles.size() * sizeof(nav::DebugVertex);
        if (triangleBytes > 0) std::memcpy(mapped.data(), m_debug.triangles.data(), triangleBytes);
        if (!m_debug.lines.empty())
            std::memcpy(mapped.data() + triangleBytes, m_debug.lines.data(), bytes - triangleBytes);
        m_transfer.unmap();
        gpu::CopyPass copy = commands.beginCopyPass();
        copy.upload(m_transfer, 0, m_vertices, 0, bytes, true);
        copy.end();
        m_drawTriangles = std::uint32_t(m_debug.triangles.size());
        m_drawLines = std::uint32_t(m_debug.lines.size());
    }

    void controls(Context& c) override {
        (void)c;
        ImGui::Combo("Group", &m_selected,
                     "Orange\0"
                     "Green\0"
                     "Both\0");
        ImGui::TextWrapped("Click or tap a floor: the group goes there.");
        int count = m_groups[0].count;
        if (ImGui::SliderInt("Agents per group", &count, 1, kMaxPerGroup)) setCount(count);
        ImGui::Checkbox("Patrol", &m_patrol);
        ImGui::Checkbox("Move obstacles", &m_moveObstacles);
        if (ImGui::SliderFloat("Mud cost", &m_mudCost, 1.0f, 10.0f, "%.1f")) applyFilter();
        ImGui::Checkbox("Navmesh", &m_showMesh);
        ImGui::SameLine();
        ImGui::Checkbox("Paths", &m_showPaths);
        if (ImGui::Button("Scatter")) scatter();
        ImGui::SameLine();
        if (ImGui::Button("Restart")) restart();
    }

    void stats(Context& c) override {
        const nav::NavMeshStats s = m_mesh->stats();
        ImGui::TextWrapped("Navmesh: %d x %d tiles, %d layers, %d polygons, %.0f KB", s.tilesX, s.tilesZ, s.tiles,
                           s.polygons, double(s.memoryBytes) / 1024.0);
        ImGui::TextWrapped("%d obstacles, %d tiles building", s.obstacles, s.pendingTiles);
        int walking = 0, onLink = 0, arrived = 0, lost = 0;
        for (const Walker& w : m_walkers) {
            walking += w.state.move == nav::AgentMove::Walking;
            onLink += w.state.move == nav::AgentMove::OnLink;
            lost += w.state.move == nav::AgentMove::Lost;
            arrived += w.state.arrived;
        }
        ImGui::TextWrapped("Agents: %zu (%d walking, %d jumping, %d arrived, %d lost)", m_walkers.size(), walking,
                           onLink, arrived, lost);
        ImGui::TextWrapped("Jumps: %d; the path across the mud: %.1f m in it", m_linkUses, double(m_probeMud));
        if (c.app.deterministic()) {
            ImGui::TextUnformatted("Times hidden (fixed clock)");
            return;
        }
        ImGui::TextWrapped("Last build: %.1f ms on %d threads", s.lastBuildSeconds * 1000.0,
                           c.app.jobs().threadCount());
        ImGui::Text("Crowd step: %.3f ms", m_crowdMs);
        if (m_clickPathUs >= 0.0) ImGui::Text("Last click's path: %.0f us", m_clickPathUs);
    }

    void click(Context& c, glm::vec2 point) override {
        glm::vec3 origin, direction;
        camera.ray(point, windowSize(c), origin, direction);
        const std::optional<glm::vec3> hit = pick(m_pickGeometry, origin, direction);
        if (!hit || m_spawnPending) return;
        ++m_clicks;
        const auto start = std::chrono::steady_clock::now();
        if (const Walker* first = firstSelected(); first && m_query->findPath(first->position, *hit, &m_filter))
            m_clickPathUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        send(*hit, [&](const Walker& w) { return selected(w.group); });
    }

    Scene& scene() override { return *m_scene; }

    std::optional<glm::vec2> point(const Context& c, std::string_view name) const override {
        for (const Waypoint& w : kWaypoints)
            if (name == w.name) return camera.project(w.position, windowSize(c));
        return std::nullopt;
    }

    std::optional<double> value(std::string_view name) const override {
        if (!m_mesh) return std::nullopt;
        const nav::NavMeshStats s = m_mesh->stats();
        if (name == "nav_ready") return ready() ? 1.0 : 0.0;
        if (name == "nav_tiles") return double(s.tiles);
        if (name == "nav_polygons") return double(s.polygons);
        if (name == "nav_pending") return double(s.pendingTiles);
        if (name == "nav_obstacles") return double(s.obstacles);
        if (name == "nav_revision") return double(m_mesh->revision());
        if (name == "nav_agents") return double(m_walkers.size());
        if (name == "nav_link_uses") return double(m_linkUses);
        if (name == "nav_landings") return double(m_landings);
        if (name == "nav_min_gap") return double(m_minGap);
        if (name == "nav_mud_steps") return double(m_mudSteps);
        // Times (not checked: they vary): the last tile builds' wall time, the crowd step's moving average.
        if (name == "nav_build_ms") return s.lastBuildSeconds * 1000.0;
        if (name == "nav_crowd_ms") return m_crowdMs;
        if (name == "nav_probe_mud") return double(m_probeMud);
        if (name == "nav_probe_length") return double(m_probeLength);
        if (name == "nav_clicks") return double(m_clicks);
        if (name == "nav_debug_triangles") return double(m_drawTriangles / 3);
        if (name == "nav_debug_lines") return double(m_drawLines / 2);
        int arrived[kGroups] = {}, lost = 0, onLink = 0;
        for (const Walker& w : m_walkers) {
            arrived[w.group] += w.state.arrived;
            lost += w.state.move == nav::AgentMove::Lost;
            onLink += w.state.move == nav::AgentMove::OnLink;
        }
        if (name == "nav_lost") return double(lost);
        if (name == "nav_on_link") return double(onLink);
        for (int g = 0; g < kGroups; ++g) {
            if (name == std::format("nav_arrived_{}", g)) return double(arrived[g]);
            if (name == std::format("nav_legs_{}", g)) return double(m_groups[std::size_t(g)].legsDone);
            if (name == std::format("nav_count_{}", g)) return double(m_groups[std::size_t(g)].count);
        }
        if (name == "nav_group") return double(m_selected);
        if (name == "nav_mud_cost") return double(m_mudCost);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<bool> flag = parseBool(value);
        const std::optional<float> number = parseFloat(value);
        if (name == "restart" && flag && *flag) {
            restart();
        } else if (name == "patrol" && flag) {
            m_patrol = *flag;
        } else if (name == "obstacles" && flag) {
            m_moveObstacles = *flag;
        } else if (name == "navmesh" && flag) {
            m_showMesh = *flag;
        } else if (name == "paths" && flag) {
            m_showPaths = *flag;
        } else if (name == "group" && number && *number >= 0.0f && *number <= float(kGroups)) {
            m_selected = int(*number);
        } else if (name == "count" && number) {
            setCount(std::clamp(int(*number), 1, kMaxPerGroup));
        } else if (name == "mud_cost" && number && *number >= 1.0f) {
            m_mudCost = *number;
            applyFilter();
        } else if (name == "scatter" && flag && *flag) {
            scatter();
        } else {
            return false;
        }
        return true;
    }

    std::string perfDetails() const override {
        if (!m_mesh) return {};
        return std::format("nav_agents={} nav_crowd_ms={:.3f} nav_polygons={}", m_walkers.size(), m_crowdMs,
                           m_mesh->stats().polygons);
    }

    void shutdown(Context& c) override {
        (void)c;
        for (Walker& w : m_walkers) removeWalker(w);
        m_walkers.clear();
        m_model = {};
        m_skeleton = {};
        m_clips = {};
        m_graph = {};
        m_anim.reset();
        m_assets.reset();
        m_manager.reset();
        m_crowd.reset();
        m_query.reset();
        m_mesh.reset();
        m_pipelines.clear();
        m_vertices = {};
        m_transfer = {};
        m_scene.reset();
    }

private:
    bool selected(int group) const { return m_selected == kGroups || m_selected == group; }

    const Walker* firstSelected() const {
        for (const Walker& w : m_walkers)
            if (selected(w.group)) return &w;
        return nullptr;
    }

    Walker* walkerOf(nav::AgentId agent) {
        for (Walker& w : m_walkers)
            if (w.agent == agent) return &w;
        return nullptr;
    }

    bool ready() const {
        if (m_spawnPending || !m_mesh->ready() || !m_model.ready()) return false;
        const animation::AnimationStats& stats = m_anim->stats();
        return stats.bound == stats.characters &&
               std::ranges::all_of(m_walkers, [](const Walker& w) { return w.skin >= 0; });
    }

    void applyFilter() {
        m_filter = nav::Filter::of(*m_mesh);
        m_filter.costs[kMud] = m_mudCost;
        m_crowd->setFilter(0, m_filter);
        m_mudSteps = 0;
    }

    // The obstacles at the obstacle clock's time; the navmesh follows once one has moved kObstacleStep.
    void placeObstacles(bool force) {
        for (std::size_t i = 0; i < kObstacles.size(); ++i) {
            const MovingObstacle& m = kObstacles[i];
            nav::Obstacle o = m.shape;
            o.position += m.travel * std::sin(m.rate * float(m_obstacleTime) + m.phase);
            m_obstaclePositions[i] = o.position;
            if (force || glm::distance(o.position, m_obstacleApplied[i]) > kObstacleStep) {
                m_mesh->setObstacle(m_obstacleIds[i], o);
                m_obstacleApplied[i] = o.position;
            }
        }
    }

    // Everything back to its start: the obstacles at their clock's start, the agents made again (at their start
    // waypoints, once the navmesh is up to date) with fresh characters.
    void restart() {
        for (Walker& w : m_walkers) removeWalker(w);
        m_walkers.clear();
        m_obstacleTime = 0.0;
        placeObstacles(true);
        for (Group& g : m_groups) g = Group{.count = g.count};
        m_spawnPending = true;
        m_linkUses = 0;
        m_landings = 0;
        m_minGap = std::numeric_limits<float>::max();
        m_mudSteps = 0;
    }

    void spawn() {
        m_spawnPending = false;
        cameraCut = true; // the frames from here on do not depend on how long the navmesh took
        for (int g = 0; g < kGroups; ++g) {
            const std::vector<int>& round = kRounds[std::size_t(g)];
            const glm::vec3 start = kWaypoints[std::size_t(round.back())].position;
            for (int k = 0; k < m_groups[std::size_t(g)].count; ++k) addWalker(g, slot(start, k));
            if (m_patrol) nextLeg(g);
        }
    }

    // Slot k of a formation around a point of the mesh: as far toward it as the floor goes.
    glm::vec3 slot(glm::vec3 center, int k) {
        const Result<nav::NavPoint> c = m_query->nearestPoint(center, &m_filter);
        if (!c) return center;
        const Result<nav::RaycastHit> ray = m_query->raycast(c->position, c->position + formation(k), &m_filter);
        return ray ? ray->position : c->position;
    }

    void addWalker(int group, glm::vec3 position) {
        Walker w;
        w.group = group;
        const nav::AgentParams params{.radius = kAgentRadius,
                                      .height = kAgentHeight,
                                      .maxSpeed = kGroupSpeeds[std::size_t(group)],
                                      .maxAcceleration = 6.0f,
                                      .separation = 0.5f,
                                      .avoidance = nav::Avoidance::Good,
                                      .arrivalDistance = kArrival};
        const Result<nav::AgentId> agent = m_crowd->addAgent(position, params);
        if (!agent) {
            log::warning("demo", "navigation: {}", agent.error());
            return;
        }
        w.agent = *agent;
        w.state = m_crowd->state(w.agent);
        w.position = w.previous = w.state.position;
        w.transform = yawed(w.position, w.yaw);
        animation::CharacterDesc desc;
        desc.skeleton = m_skeleton;
        desc.clips.push_back(m_clips);
        desc.graph = m_graph;
        desc.transform = w.transform;
        w.character = m_anim->addCharacter(std::move(desc));
        w.model = m_assets->addModel(m_model, w.transform);
        m_walkers.push_back(w);
    }

    void removeWalker(Walker& w) {
        if (m_crowd && w.agent.valid()) m_crowd->removeAgent(w.agent);
        if (w.character) m_anim->removeCharacter(w.character);
        if (w.model) m_assets->removeModel(w.model);
        w = {};
    }

    // Skins need the model's joint names and inverse bind matrices: added once it is ready.
    void bindSkins() {
        if (!m_model.ready() || m_model->skins().empty()) return;
        const assets::ModelSkin& skin = m_model->skins()[0];
        for (Walker& w : m_walkers)
            if (w.skin < 0 && w.character) w.skin = m_anim->addSkin(w.character, skin.joints, skin.inverseBindMatrices);
    }

    // Agents per group: more come at the group's target (or its start), fewer leave from the end.
    void setCount(int count) {
        for (int g = 0; g < kGroups; ++g) {
            Group& group = m_groups[std::size_t(g)];
            group.count = count;
            if (m_spawnPending) continue;
            int have = 0;
            for (const Walker& w : m_walkers) have += w.group == g;
            for (auto it = m_walkers.end(); have > count && it != m_walkers.begin();) {
                --it;
                if (it->group != g) continue;
                removeWalker(*it);
                it = m_walkers.erase(it);
                --have;
            }
            const glm::vec3 center =
                group.hasTarget ? group.target : kWaypoints[std::size_t(kRounds[std::size_t(g)].back())].position;
            for (; have < count; ++have) {
                addWalker(g, slot(center, have));
                if (group.hasTarget) m_crowd->setTarget(m_walkers.back().agent, slot(group.target, have));
            }
        }
    }

    // Sends the walkers `which` picks to slots around a point, in their order.
    template <class Which>
    void send(glm::vec3 point, Which which) {
        const Result<nav::NavPoint> center = m_query->nearestPoint(point, &m_filter);
        if (!center) return;
        int k = 0;
        for (const Walker& w : m_walkers)
            if (which(w)) m_crowd->setTarget(w.agent, slot(center->position, k++));
        for (int g = 0; g < kGroups; ++g) {
            if (!std::ranges::any_of(m_walkers, [&](const Walker& w) { return w.group == g && which(w); })) continue;
            Group& group = m_groups[std::size_t(g)];
            group.target = center->position;
            group.hasTarget = true;
            group.reached = false;
            group.legTime = group.waitTime = 0.0;
        }
    }

    void nextLeg(int g) {
        Group& group = m_groups[std::size_t(g)];
        const std::vector<int>& round = kRounds[std::size_t(g)];
        group.leg = (group.leg + 1) % int(round.size());
        send(kWaypoints[std::size_t(round[std::size_t(group.leg)])].position,
             [g](const Walker& w) { return w.group == g; });
    }

    // A group has reached its target when every agent arrived or, after a few seconds, stopped (agents the others
    // crowd out of their slots); it walks on to its next waypoint after a pause while patrolling.
    void patrol(double dt) {
        for (int g = 0; g < kGroups; ++g) {
            Group& group = m_groups[std::size_t(g)];
            if (!group.hasTarget) {
                if (m_patrol) nextLeg(g);
                continue;
            }
            group.legTime += dt;
            bool arrived = true, still = true;
            for (const Walker& w : m_walkers) {
                if (w.group != g) continue;
                arrived = arrived && w.state.arrived;
                still = still && w.state.move != nav::AgentMove::OnLink && glm::length(w.state.velocity) < 0.1f;
            }
            const bool done = arrived || (still && group.legTime > 4.0) || group.legTime > 40.0;
            if (!done) {
                group.waitTime = 0.0;
                continue;
            }
            if (!group.reached) ++group.legsDone;
            group.reached = true;
            group.waitTime += dt;
            if (m_patrol && group.waitTime >= 1.5) nextLeg(g);
        }
    }

    // A random point of the mesh for each selected agent; patrols go on once they have arrived.
    void scatter() {
        if (m_spawnPending) return;
        for (const Walker& w : m_walkers) {
            if (!selected(w.group)) continue;
            if (const Result<nav::NavPoint> p = m_query->randomPoint(m_seed, &m_filter))
                m_crowd->setTarget(w.agent, p->position);
        }
        for (int g = 0; g < kGroups; ++g) {
            if (!selected(g)) continue;
            Group& group = m_groups[std::size_t(g)];
            group.hasTarget = true;
            group.reached = false;
            group.legTime = group.waitTime = 0.0;
        }
    }

    // The closest two agents on the same floor have come since the restart (centers, in x and z), and the steps of
    // agents in the mud.
    void measure() {
        for (std::size_t i = 0; i < m_walkers.size(); ++i) {
            const nav::AgentState& a = m_walkers[i].state;
            if (a.move != nav::AgentMove::Walking) continue;
            for (std::size_t j = i + 1; j < m_walkers.size(); ++j) {
                const nav::AgentState& b = m_walkers[j].state;
                if (b.move != nav::AgentMove::Walking || std::abs(a.position.y - b.position.y) > 1.0f) continue;
                m_minGap = std::min(m_minGap,
                                    glm::length(glm::vec2(a.position.x - b.position.x, a.position.z - b.position.z)));
            }
            // Well inside it: at its edge the others may press an agent in.
            const glm::vec3 p = a.position;
            if (p.x > kMudMin.x + 0.3f && p.x < kMudMax.x - 0.3f && p.z > kMudMin.z + 0.3f && p.z < kMudMax.z - 0.3f &&
                p.y < kMudMax.y)
                ++m_mudSteps;
        }
    }

    // The path across the mud field with the current cost: how much of it is in the mud.
    void probeMud() {
        m_probeRevision = m_mesh->revision();
        m_probeCost = m_mudCost;
        m_probeMud = m_probeLength = 0.0f;
        const Result<nav::Path> path = m_query->findPath(kProbeStart, kProbeEnd, &m_filter);
        if (!path) return;
        m_probeLength = path->length();
        for (std::size_t i = 1; i < path->points.size(); ++i) {
            const glm::vec3 a = path->points[i - 1].position, b = path->points[i].position;
            const int steps = std::max(1, int(glm::distance(a, b) / 0.05f));
            for (int k = 0; k < steps; ++k) {
                const glm::vec3 p = glm::mix(a, b, (float(k) + 0.5f) / float(steps));
                if (p.x > kMudMin.x && p.x < kMudMax.x && p.z > kMudMin.z && p.z < kMudMax.z)
                    m_probeMud += glm::distance(a, b) / float(steps);
            }
        }
    }

    // The navmesh (made again when its tiles change), the obstacles, each agent's way ahead and the agents.
    void updateDebug() {
        m_debug.clear();
        if (m_showMesh) {
            if (m_meshDebugRevision != m_mesh->revision()) {
                m_meshDebug.clear();
                nav::addNavMesh(m_meshDebug, *m_mesh, 0.03f);
                for (nav::DebugVertex& v : m_meshDebug.triangles) v.color.a *= 0.7f;
                m_meshDebugRevision = m_mesh->revision();
            }
            m_debug.triangles = m_meshDebug.triangles;
            m_debug.lines = m_meshDebug.lines;
            nav::addObstacles(m_debug, *m_mesh, glm::vec4(1.0f, 0.15f, 0.1f, 0.9f));
        }
        if (m_spawnPending) return;
        std::vector<nav::AgentId> agents;
        for (int g = 0; g < kGroups; ++g) {
            const glm::vec4 color(kGroupColors[std::size_t(g)], 1.0f);
            agents.clear();
            for (const Walker& w : m_walkers) {
                if (w.group != g) continue;
                agents.push_back(w.agent);
                if (m_showPaths && w.state.hasTarget && !w.state.arrived)
                    nav::addPath(m_debug, m_crowd->route(w.agent), glm::vec4(kGroupColors[std::size_t(g)], 0.85f));
            }
            nav::addAgents(m_debug, *m_crowd, agents, color, 0.06f);
        }
        // A marker over each head in its group's color, from two crossed triangles.
        for (const Walker& w : m_walkers) {
            const glm::vec3 tip = glm::vec3(w.transform[3]) + glm::vec3(0.0f, 2.05f, 0.0f);
            const glm::vec4 color(kGroupColors[std::size_t(w.group)], 1.0f);
            for (const glm::vec3 side : {glm::vec3(0.16f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.16f)})
                for (const glm::vec3 p :
                     {tip, tip + glm::vec3(0.0f, 0.3f, 0.0f) - side, tip + glm::vec3(0.0f, 0.3f, 0.0f) + side})
                    m_debug.triangles.push_back({p, color});
        }
    }

    void drawDebug(PassContext& context) {
        if (!context.pass || (m_drawTriangles == 0 && m_drawLines == 0)) return;
        const std::array<gpu::GraphicsPipeline, 2>* pipelines = debugPipelines(context);
        if (!pipelines) return;
        if (m_drawTriangles > 0) {
            context.pass->bindPipeline((*pipelines)[0]);
            context.pass->bindVertexBuffer(0, {m_vertices, 0});
            context.pass->draw(m_drawTriangles);
        }
        if (m_drawLines > 0) {
            context.pass->bindPipeline((*pipelines)[1]);
            context.pass->bindVertexBuffer(0, {m_vertices, 0});
            context.pass->draw(m_drawLines, 1, m_drawTriangles);
        }
    }

    // Triangles and lines, blended over the scene, depth tested and not written.
    const std::array<gpu::GraphicsPipeline, 2>* debugPipelines(const PassContext& context) {
        const auto key = std::pair(context.colorFormat, context.depthFormat);
        if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
            return it->second[0] ? &it->second : nullptr;
        std::array<gpu::GraphicsPipeline, 2>& slot = m_pipelines[key];
        const gpu::VertexBufferLayout layout{.slot = 0, .stride = sizeof(nav::DebugVertex)};
        const std::array<gpu::VertexAttribute, 2> attributes{{
            {.location = 0, .slot = 0, .format = gpu::VertexFormat::Float3, .offset = 0},
            {.location = 1, .slot = 0, .format = gpu::VertexFormat::Float4, .offset = 12},
        }};
        const gpu::ColorTargetDesc color{.format = context.colorFormat, .blend = gpu::BlendState::alpha()};
        for (const gpu::PrimitiveType primitive : {gpu::PrimitiveType::TriangleList, gpu::PrimitiveType::LineList}) {
            Result<gpu::GraphicsPipeline> pipeline = m_device->createPipeline({
                .vertexShader = shaders::demo_debug.find("debug_vs"),
                .fragmentShader = shaders::demo_debug.find("debug_fs"),
                .vertexBuffers = {&layout, 1},
                .vertexAttributes = attributes,
                .primitive = primitive,
                .depth = {.test = true, .write = false},
                .colorTargets = {&color, 1},
                .depthFormat = context.depthFormat,
                .name = primitive == gpu::PrimitiveType::LineList ? "demo debug lines" : "demo debug triangles",
            });
            if (!pipeline) {
                log::error("demo", "debug pipeline: {}", pipeline.error());
                slot = {};
                return nullptr;
            }
            slot[primitive == gpu::PrimitiveType::LineList ? 1 : 0] = std::move(*pipeline);
        }
        return &slot;
    }

    gpu::Device* m_device = nullptr;
    std::optional<Scene> m_scene;
    nav::Geometry m_pickGeometry; // the level's triangles, for clicks
    std::vector<std::pair<InstanceId, InstanceId>> m_obstacleInstances;
    std::optional<nav::NavMesh> m_mesh;
    std::optional<nav::Crowd> m_crowd;
    std::optional<nav::NavQuery> m_query;
    nav::Filter m_filter;
    std::vector<nav::ObstacleId> m_obstacleIds;
    std::vector<glm::vec3> m_obstacleApplied;   // where the navmesh has them
    std::vector<glm::vec3> m_obstaclePositions; // where they are drawn
    double m_obstacleTime = 0.0;

    std::unique_ptr<assets::AssetManager> m_manager;
    std::optional<SceneAssets> m_assets;
    std::unique_ptr<AnimationSystem> m_anim;
    assets::AssetRef<ModelAsset> m_model;
    assets::AssetRef<animation::SkeletonAsset> m_skeleton;
    assets::AssetRef<animation::ClipSetAsset> m_clips;
    assets::AssetRef<animation::GraphAsset> m_graph;

    std::vector<Walker> m_walkers;
    std::array<Group, kGroups> m_groups{};
    bool m_spawnPending = true;
    int m_selected = 0; // a group, or kGroups: both
    bool m_patrol = true, m_moveObstacles = true, m_showMesh = true, m_showPaths = true;
    float m_mudCost = 5.0f;
    std::uint64_t m_seed = 1;

    int m_linkUses = 0, m_landings = 0, m_clicks = 0;
    float m_minGap = std::numeric_limits<float>::max();
    int m_mudSteps = 0; // agent steps in the mud since the restart or the last cost change
    double m_crowdMs = 0.0;
    std::uint64_t m_crowdSteps = 0;
    double m_clickPathUs = -1.0;
    std::uint64_t m_probeRevision = ~0ull;
    float m_probeCost = -1.0f, m_probeMud = 0.0f, m_probeLength = 0.0f;

    nav::DebugGeometry m_debug, m_meshDebug;
    std::uint64_t m_meshDebugRevision = ~0ull;
    gpu::Buffer m_vertices;
    gpu::TransferBuffer m_transfer;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_drawTriangles = 0, m_drawLines = 0; // vertices uploaded for this frame
    std::map<std::pair<gpu::TextureFormat, gpu::TextureFormat>, std::array<gpu::GraphicsPipeline, 2>> m_pipelines;
    PassId m_pass;
};

} // namespace

std::unique_ptr<Section> makeNavigationSection() { return std::make_unique<NavigationSection>(); }

} // namespace demo
