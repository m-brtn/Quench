// Animation: a jointed mannequin cooked from a procedural glTF (tools/assets/make_demo_character.py: one skin of 20
// joints and the clips idle, walk, run, jump, wave and lean) and animated by animation::AnimationSystem. The hero
// walks a circle by its clips' root motion: a blend tree over its speed (idle, walk, run), a jump and a wave by
// trigger with cross-fades (the wave on a layer masked to the right arm), an additive lean, its head turned toward
// the camera, a torch held in its left hand and a footstep sound at each contact event. Two characters stand on a
// slope and across a step, their feet placed on the ground by a ray query into a physics world. Behind them a crowd
// walks in rings, updated in parallel on the job system and skinned by a compute pass; with animation LOD (off by
// default) the characters small on the screen update every 2nd or 4th frame. A showcase away from them
// (the views other than the hero's) has a character steered in 2D through a blend space over its velocity (idle,
// walk, run, walk back, strafes), facing one way while it walks in every direction. Another one, on the other side,
// has two standing characters whose left forearms the section turns by 170 degrees, skinned linearly (the elbow
// collapses) and by dual quaternions (it keeps its volume); the other characters' skinning is a choice. Behind the
// hero's camera the mannequin walks beside a second character of other joint names, proportions and rest pose
// (tools/assets/make_demo_strider.py), which plays the same walk retargeted onto it at cook time. In its own view a
// swaying jelly creature (tools/assets/make_demo_blob.py) is reshaped by four blend shapes, applied before its
// skinning; it is drawn in that view only.

#include "Demo.h"

#include <quench/animation/AnimationSystem.h>
#include <quench/assets/AssetManager.h>
#include <quench/audio/AudioSystem.h>
#include <quench/physics/PhysicsWorld.h>
#include <quench/render/SceneAssets.h>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
using animation::AnimationSystem;
using animation::CharacterId;
using app::Binding;
using app::GamepadButton;

constexpr const char* kStep = "sounds/step.wav";

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kMaxSpeed = 2.7f;   // m/s: the run clip's speed (the graph's blend points are in m/s)
constexpr float kHeroRadius = 2.4f; // m: the circle the hero walks around kHeroCenter
const glm::vec2 kHeroCenter(1.6f, -1.4f);
constexpr float kHeroStart = -0.7f;  // radians on the circle; 0 is its point nearest +Z
constexpr float kGroundHalf = 28.0f; // m
const glm::vec2 kCrowdCenter(1.5f, -11.5f);
constexpr float kCrowdFirstRing = 2.6f; // m
constexpr float kCrowdRingStep = 1.25f; // m between rings
constexpr float kCrowdSpacing = 1.3f;   // m between characters on a ring
constexpr int kMaxCrowd = 512;
constexpr std::size_t kRecentFrames = 240; // the perf line's update and skinning times: 2 s at 120 Hz
// The foot placement stand, left of the hero's circle: a ramp rising toward +X from the floor, and a step.
const glm::vec2 kSlopeAt(-2.6f, 0.0f);
const glm::vec3 kStepAt(-1.2f, 0.0f, 2.5f);
constexpr float kSlopeHalf = 1.2f; // m: half the ramp's length
constexpr float kStepHeight = 0.2f;
// The showcase, out of the hero's view: the 2D blend space's character walks inside a circle there.
const glm::vec2 kSpaceCenter(-18.0f, 0.0f);
constexpr float kSpaceRadius = 2.4f; // m: velocity out of the circle fades over its last kSpaceEdge
constexpr float kSpaceEdge = 0.6f;
constexpr float kSteerSpeed = 0.8f; // m/s of the steering by itself, whose direction turns at kSteerTurn
constexpr float kSteerTurn = 0.45f; // rad/s
const glm::vec2 kSteerMin(-0.75f, -0.9f), kSteerMax(0.75f, 2.7f); // the blend space's extent (m/s)
// The skinning showcase, out of the hero's view on the other side: two standing characters whose left forearms the
// section turns about their length, skinned linearly (left) and by dual quaternions (right).
const glm::vec3 kTwistAt(18.0f, 0.0f, 0.0f);
constexpr float kTwistApart = 0.8f;                  // m between the two
constexpr float kTwistAngle = 170.0f * kPi / 180.0f; // radians
// The retargeting pair, behind the hero's camera: the mannequin and the strider walk the same clip on circles around
// kPairCenter. The strider's clips move it kStriderScale times as far (its legs are that much longer), so on a circle
// that much wider the two stay abreast.
const glm::vec2 kPairCenter(0.0f, 18.0f);
constexpr float kPairRadius = 5.0f;    // m: the mannequin's circle
constexpr float kStriderScale = 1.15f; // make_demo_strider.py's LEG
constexpr float kPairStart = kPi;      // radians: the circles' points nearest the hero's stage
// The blend shape creature, behind the skinning showcase (and hidden outside its view), and its shapes' names.
const glm::vec3 kBlobAt(18.0f, 0.0f, -7.0f);
constexpr std::array<std::string_view, 4> kBlobShapes{"squash", "stretch", "puff", "spikes"};

// The camera's places: the hero's stage and the showcase's spots (`set view ID`). In the retargeting view the camera
// follows the pair and turns with it.
struct View {
    const char* id;
    const char* label;
    glm::vec3 target;
    float yaw, pitch, distance; // degrees, m
};
const std::array kViews{
    View{"hero", "Hero", {0.0f, 0.5f, -0.6f}, 2.0f, 22.0f, 9.0f},
    View{"blend2d", "2D blend space", {-18.0f, 0.6f, 0.0f}, 18.0f, 26.0f, 7.5f},
    View{"skinning", "Skinning", {18.0f, 1.1f, 0.0f}, 0.0f, 6.0f, 2.1f},
    View{"retarget", "Retargeting", {0.0f, 1.0f, 18.0f - 1.075f * kPairRadius}, -120.0f, 10.0f, 4.6f},
    View{"blendshapes", "Blend shapes", {18.0f, 0.45f, -7.0f}, 20.0f, 12.0f, 2.2f},
};

glm::mat4 yawed(const glm::vec3& position, float yaw) {
    return glm::rotate(glm::translate(glm::mat4(1.0f), position), yaw, glm::vec3(0.0f, 1.0f, 0.0f));
}

JPH::Quat toJolt(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }

// One animated character and its model in the scene.
struct Walker {
    CharacterId id;
    ModelInstanceId model;
    int skin = -1; // AnimationSystem::addSkin, once the model is ready
    glm::vec3 position{0.0f};
    float yaw = 0.0f; // about +Y; 0 faces +Z
    // A circle walked by root motion (radius 0: stands): the angle on it follows the distance the clips moved.
    glm::vec2 center{0.0f};
    float radius = 0.0f;
    float angle = 0.0f;
    float turn = 1.0f; // +1: the angle grows (counterclockwise seen from below), -1: it shrinks
    glm::mat4 transform{1.0f};
    std::optional<Skinning> skinning; // unset: the section's choice
    float twist = 0.0f;               // radians the left forearm is turned about its length (the showcase)
    bool strider = false;             // the second character (its own model, skeleton and retargeted clips)
};

class AnimationSection final : public Section {
public:
    AnimationSection() {
        camera.target = glm::vec3(0.0f, 0.5f, -0.6f);
        camera.distance = 9.0f;
        camera.yaw = glm::radians(2.0f);
        camera.pitch = glm::radians(22.0f);
        camera.maxDistance = 70.0f;
    }

    std::string_view id() const override { return "animation"; }
    std::string_view title() const override { return "Animation"; }
    std::string_view about() const override {
        return "Cooked glTF clips on quench::animation: speed blend, jump and wave with cross-fades, a masked arm "
               "layer, an additive lean, root motion, footstep events with sound, look-at, foot placement on a slope "
               "and a step (ray queries into Jolt), a held torch; a crowd updated on jobs and skinned on the GPU; a "
               "character steered through a 2D blend space, a forearm twisted under linear and dual quaternion "
               "skinning, a walk retargeted at cook time onto a character of other proportions, and a creature "
               "reshaped by blend shapes before its skinning (View).";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(kGroundHalf), 1.0f));
        const Result<MeshId> slope = addMesh(s, boxMesh(glm::vec3(kSlopeHalf, 0.25f, 1.0f)));
        const Result<MeshId> step = addMesh(s, boxMesh(glm::vec3(0.5f, kStepHeight * 0.5f, 0.8f)));
        const Result<MeshId> stick = addMesh(s, boxMesh(glm::vec3(0.014f, 0.014f, 0.22f)));
        const Result<MeshId> flame = addMesh(s, sphereMesh(0.045f, 10, 16));
        const Result<MeshId> path = addMesh(s, torusMesh(kHeroRadius, 0.03f, 96, 8));
        for (const Result<MeshId>* m : {&ground, &slope, &step, &stick, &flame, &path})
            if (!*m) return Error{m->error()};
        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.45f, 0.46f, 0.47f),
                                                      glm::vec3(0.36f, 0.37f, 0.39f), 1.0f)});
        const MaterialId stone =
            s.addMaterial({.baseColor = glm::vec4(0.42f, 0.4f, 0.37f, 1.0f), .roughness = 0.8f, .name = "stone"});
        m_slopeInstance = s.addInstance({.mesh = *slope, .material = stone});
        s.addInstance({.mesh = *step,
                       .material = stone,
                       .transform = placed(kStepAt + glm::vec3(0.52f, kStepHeight * 0.5f, 0.0f))});
        s.addInstance({.mesh = *path,
                       .material = s.addMaterial({.baseColor = glm::vec4(0.25f, 0.27f, 0.3f, 1.0f), .roughness = 0.6f}),
                       .transform = placed(glm::vec3(kHeroCenter.x, 0.005f, kHeroCenter.y)),
                       .castsShadows = false});
        static_assert(kSpaceRadius == kHeroRadius, "the steered character's circle uses the hero's path mesh");
        s.addInstance({.mesh = *path,
                       .material = s.addMaterial({.baseColor = glm::vec4(0.2f, 0.3f, 0.45f, 1.0f), .roughness = 0.6f}),
                       .transform = placed(glm::vec3(kSpaceCenter.x, 0.005f, kSpaceCenter.y)),
                       .castsShadows = false});
        m_stick = s.addInstance(
            {.mesh = *stick,
             .material = s.addMaterial({.baseColor = glm::vec4(0.35f, 0.22f, 0.12f, 1.0f), .roughness = 0.7f})});
        m_flame = s.addInstance({.mesh = *flame,
                                 .material = s.addMaterial({.baseColor = glm::vec4(1.0f, 0.55f, 0.15f, 1.0f),
                                                            .emissive = glm::vec3(6.0f, 2.4f, 0.5f),
                                                            .name = "flame"}),
                                 .castsShadows = false});
        setDaylight(s, s.addLight({.type = LightType::Directional}), 38.0f, 40.0f);

        // The ground the feet are placed on: the floor, the slope and the step as static bodies; the characters'
        // rays query it from the job threads while it is not stepping (it never steps here).
        m_world = std::make_unique<PhysicsWorld>(&c.app.jobs());
        JPH::BodyInterface& bodies = m_world->bodies();
        bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(kGroundHalf, 0.5f, kGroundHalf)),
                                                          JPH::RVec3(0.0f, -0.5f, 0.0f), JPH::Quat::sIdentity(),
                                                          JPH::EMotionType::Static, PhysicsLayers::kStatic),
                                JPH::EActivation::DontActivate);
        bodies.CreateAndAddBody(JPH::BodyCreationSettings(new JPH::BoxShape(JPH::Vec3(0.5f, kStepHeight * 0.5f, 0.8f)),
                                                          JPH::RVec3(kStepAt.x + 0.52f, kStepHeight * 0.5f, kStepAt.z),
                                                          JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                                          PhysicsLayers::kStatic),
                                JPH::EActivation::DontActivate);
        m_slopeShape = new JPH::BoxShape(JPH::Vec3(kSlopeHalf, 0.25f, 1.0f));
        placeSlope();
        m_world->optimizeBroadPhase();

        // The section's own asset manager: SceneAssets registers its asset types once per manager, and the Assets
        // section's scene has the app's.
        m_manager = std::make_unique<assets::AssetManager>(c.app.jobs(), c.app.assets().settings());
        m_assets.emplace(s, *m_manager);
        m_anim = std::make_unique<AnimationSystem>(*m_manager, c.app.jobs());
        m_anim->setGroundQuery(
            [world = m_world.get()](const glm::vec3& from, float length) { return groundHit(*world, from, length); });
        m_model = m_assets->loadModel(mannequin::kModel);
        m_skeleton = m_anim->loadSkeleton(mannequin::kSkeleton);
        m_clips = m_anim->loadClips(mannequin::kClips);
        m_graph = m_anim->loadGraph(mannequin::kGraph);
        m_graph2d = m_anim->loadGraph(mannequin::kGraph2d);
        m_walkGraph = m_anim->loadGraph(mannequin::kWalkGraph);
        m_striderModel = m_assets->loadModel(strider::kModel);
        m_striderSkeleton = m_anim->loadSkeleton(strider::kSkeleton);
        m_striderClips = m_anim->loadClips(strider::kClips);
        m_blobModel = m_assets->loadModel(blob::kModel);
        m_blobSkeleton = m_anim->loadSkeleton(blob::kSkeleton);
        m_blobClips = m_anim->loadClips(blob::kClips);
        m_blobGraph = m_anim->loadGraph(blob::kGraph);
        m_step = c.app.audio().load(kStep);

        app::Input& input = c.app.input();
        m_jumpAction = action(input, "anim_jump", {*Binding::key("J"), Binding::gamepad(GamepadButton::South)});
        m_waveAction = action(input, "anim_wave", {*Binding::key("K"), Binding::gamepad(GamepadButton::West)});
        restart();
        return {};
    }

    void enter(Context& c) override { c.app.audio().setListener(listener()); }

    void update(Context& c, double frameDt) override {
        const float dt = std::min(float(frameDt), 0.1f);
        const app::Input& input = c.app.input();
        if (c.sceneInput && input.pressed(m_jumpAction)) m_anim->trigger(m_hero.id, "jump");
        if (c.sceneInput && input.pressed(m_waveAction)) m_anim->trigger(m_hero.id, "wave");
        m_anim->setFloat(m_hero.id, "speed", m_speed);
        m_anim->setFloat(m_hero.id, "lean", m_lean);
        m_anim->setLookAt(m_hero.id, m_lookAt ? std::optional<glm::vec3>(camera.eye()) : std::nullopt);
        for (const CharacterId id : {m_hero.id, m_slopeStand.id, m_stepStand.id, m_strafer.id})
            m_anim->setFootPlacement(id, m_feet);
        steer(dt);

        bindSkins();
        m_manager->update();
        m_assets->update();
        const glm::vec2 window = windowSize(c);
        m_anim->setLod({.enabled = m_lod, .interpolate = m_lodInterpolate});
        m_anim->setLodView(camera.eye(), camera.fovFor(window.x / std::max(window.y, 1.0f)));
        m_anim->update(dt);
        m_skinnedReused = c.renderer.stats().skinnedReused; // of the frame before
        m_recentUpdateMs[m_recentNext] = float(m_anim->stats().updateMs);
        m_recentSkinningMs[m_recentNext] = c.renderer.stats().skinningGpuMs;
        m_recentEvaluated[m_recentNext] = float(m_anim->stats().evaluated);
        m_recentNext = (m_recentNext + 1) % kRecentFrames;
        m_recentCount = std::min(m_recentCount + 1, kRecentFrames);
        footsteps(c);

        // Draw at the transform the update used (IK, look-at and the attachments match it), then move on by the
        // root motion for the next update.
        const float pairAngle = m_pairMannequin.angle;
        forEachWalker([&](Walker& w) {
            if (w.skin >= 0) m_assets->setJointPalette(w.model, 0, palette(w), m_teleport);
            m_assets->setTransform(w.model, w.transform, m_teleport);
            walk(w);
        });
        if (std::string_view(kViews[std::size_t(m_view)].id) == "retarget") followPair(pairAngle);
        updateBlob();
        moveStrafer(dt);
        m_teleport = false;
        const glm::mat4 hand = m_anim->jointWorld(m_hero.id, m_anim->findJoint(m_hero.id, "hand_l"));
        const bool holding = m_anim->findJoint(m_hero.id, "hand_l") >= 0;
        m_scene->setVisible(m_stick, holding);
        m_scene->setVisible(m_flame, holding);
        m_stickTransform = hand * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.075f, 0.06f));
        m_scene->setTransform(m_stick, m_stickTransform);
        m_scene->setTransform(m_flame, hand * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.075f, 0.29f)));
        c.app.audio().setListener(listener());
    }

    void controls(Context& c) override {
        (void)c;
        const animation::CharacterState hero = m_anim->state(m_hero.id);
        if (!hero.bound)
            ImGui::TextUnformatted(failedAssets() > 0 ? "Character assets failed (see the log)" : "Loading...");
        else if (hero.nextState.empty())
            ImGui::Text("Hero: %.*s", int(hero.layerState.size()), hero.layerState.data());
        else
            ImGui::Text("Hero: %.*s -> %.*s %.0f%%", int(hero.layerState.size()), hero.layerState.data(),
                        int(hero.nextState.size()), hero.nextState.data(), hero.transition * 100.0f);
        ImGui::SliderFloat("Speed", &m_speed, 0.0f, kMaxSpeed, "%.2f m/s");
        ImGui::SliderFloat("Lean", &m_lean, 0.0f, 1.0f, "%.2f");
        if (ImGui::Button("Jump (J)")) m_anim->trigger(m_hero.id, "jump");
        ImGui::SameLine();
        if (ImGui::Button("Wave (K)")) m_anim->trigger(m_hero.id, "wave");
        ImGui::SameLine();
        if (ImGui::Button("Restart")) restart();
        ImGui::Checkbox("Foot IK", &m_feet);
        ImGui::SameLine();
        ImGui::Checkbox("Look at camera", &m_lookAt);
        ImGui::Checkbox("Footstep sound", &m_footstepSound);
        ImGui::SameLine();
        ImGui::Text("%d steps", m_footsteps);
        float slope = m_slopeDegrees;
        if (ImGui::SliderFloat("Slope", &slope, 0.0f, 30.0f, "%.0f deg")) setSlope(slope);
        int crowd = m_crowdSize;
        if (ImGui::SliderInt("Crowd", &crowd, 0, kMaxCrowd)) setCrowd(crowd);
        if (ImGui::BeginCombo("View", kViews[std::size_t(m_view)].label)) {
            for (std::size_t i = 0; i < kViews.size(); ++i)
                if (ImGui::Selectable(kViews[i].label, int(i) == m_view)) setView(int(i));
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Animation LOD", &m_lod);
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_lod);
        ImGui::Checkbox("Interpolate", &m_lodInterpolate);
        ImGui::EndDisabled();
        bool dualQuaternion = m_skinning == Skinning::DualQuaternion;
        if (ImGui::Checkbox("Dual quaternion skinning", &dualQuaternion))
            setSkinning(dualQuaternion ? Skinning::DualQuaternion : Skinning::Linear);
        ImGui::SeparatorText("2D blend space");
        ImGui::Checkbox("Steer by itself", &m_autoSteer);
        ImGui::BeginDisabled(m_autoSteer);
        ImGui::SliderFloat("To the left", &m_steerVelocity.x, kSteerMin.x, kSteerMax.x, "%.2f m/s");
        ImGui::SliderFloat("Ahead", &m_steerVelocity.y, kSteerMin.y, kSteerMax.y, "%.2f m/s");
        ImGui::EndDisabled();
        ImGui::SeparatorText("Blend shapes");
        for (std::size_t k = 0; k < kBlobShapes.size(); ++k) {
            const std::string label = std::format("{}##blob", kBlobShapes[k]);
            ImGui::SliderFloat(label.c_str(), &m_blobWeights[k], 0.0f, 1.0f, "%.2f");
        }
        if (ImGui::Button("Reset shapes")) m_blobWeights.fill(0.0f);
    }

    void stats(Context& c) override {
        const animation::AnimationStats& stats = m_anim->stats();
        const RenderStats& r = c.renderer.stats();
        ImGui::Text("Characters %u, joints %u", stats.characters, stats.joints);
        ImGui::Text("Samples %u, rays %u, events %u", stats.clipsSampled, stats.groundQueries, stats.events);
        ImGui::Text("Skinned %u (%u vertices), %u reused", r.skinnedInstances, r.skinnedVertices, r.skinnedReused);
        ImGui::Text("Updated %u; every frame %u, 2nd %u, 4th %u", stats.evaluated, stats.lodRates[0], stats.lodRates[1],
                    stats.lodRates[2]);
        ImGui::Text("Steered: asked %.2f %.2f, moves %.2f %.2f m/s", m_steerInput.x, m_steerInput.y, m_strafeMotion.x,
                    m_strafeMotion.y);
        if (c.app.deterministic()) {
            ImGui::TextUnformatted("Times hidden (fixed clock)");
        } else {
            ImGui::Text("Update %.3f ms, %d threads", stats.updateMs, stats.threads);
            if (r.skinningGpuMs >= 0.0f)
                ImGui::Text("Skinning GPU %.3f ms", double(r.skinningGpuMs));
            else
                ImGui::TextUnformatted("Skinning GPU time n/a");
        }
    }

    Scene& scene() override { return *m_scene; }

    // Over the last kRecentFrames frames: the mean and largest update time, the mean number of characters updated
    // (animation LOD), the mean skinning GPU time (frames without a measurement left out).
    std::string perfDetails() const override {
        if (!m_anim || m_recentCount == 0) return {};
        float updateSum = 0.0f, updateMax = 0.0f, skinningSum = 0.0f, evaluatedSum = 0.0f;
        int skinningFrames = 0;
        for (std::size_t i = 0; i < m_recentCount; ++i) {
            evaluatedSum += m_recentEvaluated[i];
            updateSum += m_recentUpdateMs[i];
            updateMax = std::max(updateMax, m_recentUpdateMs[i]);
            if (m_recentSkinningMs[i] < 0.0f) continue;
            skinningSum += m_recentSkinningMs[i];
            ++skinningFrames;
        }
        return std::format("anim_frames={} anim_update_ms={:.3f} anim_update_max_ms={:.3f} anim_threads={} "
                           "anim_lod={} anim_evaluated={:.1f} skinning_gpu_ms={:.3f}",
                           m_recentCount, updateSum / float(m_recentCount), updateMax, m_anim->stats().threads,
                           m_lod ? (m_lodInterpolate ? "interpolate" : "hold") : "off",
                           evaluatedSum / float(m_recentCount),
                           skinningFrames > 0 ? skinningSum / float(skinningFrames) : -1.0f);
    }

    std::optional<double> value(std::string_view name) const override {
        if (!m_anim) return std::nullopt;
        const animation::AnimationStats& stats = m_anim->stats();
        const animation::CharacterState hero = m_anim->state(m_hero.id);
        if (name == "anim_ready")
            return m_model.ready() && m_striderModel.ready() && m_blobModel.ready() && m_step.ready() &&
                           stats.bound == stats.characters && allSkinned() && m_blob.skin >= 0
                       ? 1.0
                       : 0.0;
        if (name == "anim_failed") return double(failedAssets());
        if (name == "anim_characters") return double(stats.characters);
        if (name == "anim_bound") return double(stats.bound);
        if (name == "anim_ground_rays") return double(stats.groundQueries);
        if (name == "hero_state") return hero.layerState == "jump" ? 1.0 : 0.0;
        if (name == "hero_transition") return hero.nextState.empty() ? 0.0 : 1.0;
        if (name == "hero_look_at") return hero.lookAtReached ? 1.0 : 0.0;
        if (name == "hero_angle") return double(glm::degrees(m_hero.angle));
        if (name == "footsteps") return double(m_footsteps);
        if (name == "landings") return double(m_landings);
        if (name == "crowd") return double(m_crowd.size());
        // Animation LOD: the characters updated in the last update, at each rate, and the skinned instances the
        // renderer reused in the frame before.
        if (name == "anim_evaluated") return double(stats.evaluated);
        if (name == "anim_lod_rate1") return double(stats.lodRates[0]);
        if (name == "anim_lod_rate2") return double(stats.lodRates[1]);
        if (name == "anim_lod_rate4") return double(stats.lodRates[2]);
        if (name == "skinned_reused") return double(m_skinnedReused);
        if (name == "dual_quaternion") return m_skinning == Skinning::DualQuaternion ? 1.0 : 0.0;
        // The retargeting pair: the strider's angle on its circle ahead of the mannequin's (degrees; 0 while they
        // walk abreast) and its hips' height over the mannequin's pelvis' (1.15 from its proportions).
        if (name == "pair_gap") {
            const float gap = std::remainder(m_pairStrider.angle - m_pairMannequin.angle, 2.0f * kPi);
            return double(glm::degrees(gap));
        }
        if (name == "pair_height_ratio") {
            const int pelvis = m_anim->findJoint(m_pairMannequin.id, "pelvis");
            const int hips = m_anim->findJoint(m_pairStrider.id, "Hips");
            if (pelvis < 0 || hips < 0) return 0.0;
            return double(m_anim->modelMatrices(m_pairStrider.id)[std::size_t(hips)][3].y /
                          m_anim->modelMatrices(m_pairMannequin.id)[std::size_t(pelvis)][3].y);
        }
        if (name == "held_distance") {
            // The torch's grip from the hand joint (m): 0.096 when attached.
            const glm::mat4 hand = m_anim->jointWorld(m_hero.id, m_anim->findJoint(m_hero.id, "hand_l"));
            return double(glm::length(glm::vec3(m_stickTransform[3]) - glm::vec3(hand[3])));
        }
        if (name == "view") return double(m_view);
        // The blend shape creature: its model's blend shapes and the weight of one by name (blob_weight_squash).
        if (name == "blob_shapes") return m_blobModel.ready() ? double(m_blobModel->morphTargets().size()) : 0.0;
        if (name.starts_with("blob_weight_")) {
            const auto it = std::ranges::find(kBlobShapes, name.substr(12));
            if (it == kBlobShapes.end()) return std::nullopt;
            return double(m_blobWeights[std::size_t(it - kBlobShapes.begin())]);
        }
        // The steered character: the velocity asked for (after the circle's edge), the one its clips move it by
        // (smoothed over 0.2 s), the angle between the two (degrees) and its distance from the circle's center.
        if (name == "steer_vx") return double(m_steerInput.x);
        if (name == "steer_vz") return double(m_steerInput.y);
        if (name == "strafer_vx") return double(m_strafeMotion.x);
        if (name == "strafer_vz") return double(m_strafeMotion.y);
        if (name == "strafer_heading_error") {
            if (glm::length(m_steerInput) < 0.2f || glm::length(m_strafeMotion) < 0.1f) return 0.0;
            const float c = glm::dot(glm::normalize(m_steerInput), glm::normalize(m_strafeMotion));
            return double(glm::degrees(std::acos(std::clamp(c, -1.0f, 1.0f))));
        }
        if (name == "strafer_radius")
            return double(glm::length(glm::vec2(m_strafer.position.x, m_strafer.position.z) - kSpaceCenter));
        if (name == "slope_pelvis") return double(m_anim->state(m_slopeStand.id).pelvisOffset);
        if (name.starts_with("slope_leg") || name.starts_with("step_leg")) {
            // slope_leg0_offset, step_leg1_reached, ...: the stands' legs (0 left, 1 right).
            const bool slope = name.starts_with("slope");
            const std::string_view rest = name.substr(slope ? 9 : 8);
            const std::size_t leg = rest.starts_with("1") ? 1 : 0;
            const animation::CharacterState st = m_anim->state(slope ? m_slopeStand.id : m_stepStand.id);
            if (leg >= st.legs.size()) return 0.0;
            if (rest.ends_with("_offset")) return double(st.legs[leg].offset);
            if (rest.ends_with("_reached")) return st.legs[leg].reached ? 1.0 : 0.0;
            if (rest.ends_with("_grounded")) return st.legs[leg].grounded ? 1.0 : 0.0;
        }
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "restart" && flag) {
            if (*flag) restart();
        } else if (name == "speed" && number) {
            m_speed = std::clamp(*number, 0.0f, kMaxSpeed);
        } else if (name == "lean" && number) {
            m_lean = std::clamp(*number, 0.0f, 1.0f);
        } else if ((name == "jump" || name == "wave") && flag) {
            if (*flag) m_anim->trigger(m_hero.id, name);
        } else if (name == "foot_ik" && flag) {
            m_feet = *flag;
        } else if (name == "look_at" && flag) {
            m_lookAt = *flag;
        } else if (name == "footstep_sound" && flag) {
            m_footstepSound = *flag;
        } else if (name == "slope" && number) {
            setSlope(std::clamp(*number, 0.0f, 30.0f));
        } else if (name == "crowd" && number) {
            setCrowd(std::clamp(int(*number), 0, kMaxCrowd));
        } else if (name == "lod" && flag) {
            m_lod = *flag;
        } else if (name == "lod_interpolate" && flag) {
            m_lodInterpolate = *flag;
        } else if (name == "dual_quaternion" && flag) {
            setSkinning(*flag ? Skinning::DualQuaternion : Skinning::Linear);
        } else if (name == "view") {
            const auto it = std::ranges::find_if(kViews, [&](const View& v) { return v.id == value; });
            if (it == kViews.end()) return false;
            setView(int(it - kViews.begin()));
        } else if (name == "steer_auto" && flag) {
            m_autoSteer = *flag;
        } else if (name == "steer_vx" && number) {
            m_steerVelocity.x = std::clamp(*number, kSteerMin.x, kSteerMax.x);
        } else if (name == "steer_vz" && number) {
            m_steerVelocity.y = std::clamp(*number, kSteerMin.y, kSteerMax.y);
        } else if (name.starts_with("blob_") && number) {
            // blob_squash 0.8: a blend shape's weight (any value, also outside 0..1).
            const auto it = std::ranges::find(kBlobShapes, name.substr(5));
            if (it == kBlobShapes.end()) return false;
            m_blobWeights[std::size_t(it - kBlobShapes.begin())] = *number;
        } else {
            return false;
        }
        return true;
    }

    std::optional<glm::vec2> point(const Context& c, std::string_view name) const override {
        if (name != "hero") return std::nullopt;
        const glm::vec2 window = windowSize(c);
        return camera.project(m_hero.position + glm::vec3(0.0f, 1.0f, 0.0f), window);
    }

    void shutdown(Context& c) override {
        (void)c;
        forEachWalker([&](Walker& w) { m_assets->removeModel(w.model); });
        if (m_blob.model) m_assets->removeModel(m_blob.model);
        m_blob = {};
        m_crowd.clear();
        m_model = {};
        m_skeleton = {};
        m_clips = {};
        m_graph = {};
        m_graph2d = {};
        m_walkGraph = {};
        m_striderModel = {};
        m_striderSkeleton = {};
        m_striderClips = {};
        m_blobModel = {};
        m_blobSkeleton = {};
        m_blobClips = {};
        m_blobGraph = {};
        m_step = {};
        m_anim.reset();
        m_assets.reset();
        m_manager.reset();
        m_world.reset();
        m_scene.reset();
    }

private:
    static std::optional<animation::GroundHit> groundHit(const PhysicsWorld& world, const glm::vec3& from,
                                                         float length) {
        const JPH::RRayCast ray{JPH::RVec3(from.x, from.y, from.z), JPH::Vec3(0.0f, -length, 0.0f)};
        JPH::RayCastResult hit;
        if (!world.system().GetNarrowPhaseQuery().CastRay(ray, hit)) return std::nullopt;
        const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
        // No body changes while the characters update: the lock-free interface is safe from several threads.
        const JPH::BodyLockRead lock(world.system().GetBodyLockInterfaceNoLock(), hit.mBodyID);
        if (!lock.Succeeded()) return std::nullopt;
        const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point);
        return animation::GroundHit{.position =
                                        glm::vec3(float(point.GetX()), float(point.GetY()), float(point.GetZ())),
                                    .normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ())};
    }

    // The ramp: a box rotated about Z so that its top rises toward +X, the top's lower edge on the floor.
    glm::quat slopeRotation() const {
        return glm::angleAxis(glm::radians(m_slopeDegrees), glm::vec3(0.0f, 0.0f, 1.0f));
    }
    glm::vec3 slopeTop() const {
        return glm::vec3(kSlopeAt.x, kSlopeHalf * std::sin(glm::radians(m_slopeDegrees)), kSlopeAt.y);
    }
    glm::vec3 slopeCenter() const { return slopeTop() - slopeRotation() * glm::vec3(0.0f, 0.25f, 0.0f); }

    void placeSlope() {
        const glm::vec3 p = slopeCenter();
        const glm::quat q = slopeRotation();
        JPH::BodyInterface& bodies = m_world->bodies();
        if (!m_slopeBody.IsInvalid()) {
            bodies.RemoveBody(m_slopeBody);
            bodies.DestroyBody(m_slopeBody);
        }
        m_slopeBody =
            bodies.CreateAndAddBody(JPH::BodyCreationSettings(m_slopeShape, JPH::RVec3(p.x, p.y, p.z), toJolt(q),
                                                              JPH::EMotionType::Static, PhysicsLayers::kStatic),
                                    JPH::EActivation::DontActivate);
        m_scene->setTransform(m_slopeInstance, glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q));
    }

    void setSlope(float degrees) {
        m_slopeDegrees = degrees;
        placeSlope();
        m_slopeStand.position = slopeTop();
        m_slopeStand.transform = yawed(m_slopeStand.position, m_slopeStand.yaw);
        m_anim->setTransform(m_slopeStand.id, m_slopeStand.transform);
    }

    audio::Listener listener() const {
        const glm::vec3 eye = camera.eye();
        return {.position = eye, .forward = glm::normalize(camera.target - eye)};
    }

    // The characters other than the crowd.
    template <class Self>
    static auto singles(Self& self) {
        return std::array{&self.m_hero,        &self.m_slopeStand, &self.m_stepStand,     &self.m_strafer,
                          &self.m_twistLinear, &self.m_twistDual,  &self.m_pairMannequin, &self.m_pairStrider};
    }

    template <class F>
    void forEachWalker(F&& f) {
        for (Walker* w : singles(*this)) f(*w);
        for (Walker& w : m_crowd) f(w);
    }

    bool allSkinned() const {
        for (const Walker* w : singles(*this))
            if (w->skin < 0) return false;
        return std::ranges::all_of(m_crowd, [](const Walker& w) { return w.skin >= 0; });
    }

    int failedAssets() const {
        int n = 0;
        for (const assets::AssetState s :
             {m_model.state(), m_skeleton.state(), m_clips.state(), m_graph.state(), m_graph2d.state(),
              m_walkGraph.state(), m_striderModel.state(), m_striderSkeleton.state(), m_striderClips.state(),
              m_blobModel.state(), m_blobSkeleton.state(), m_blobClips.state(), m_blobGraph.state(), m_step.state()})
            n += s == assets::AssetState::Failed ? 1 : 0;
        return n;
    }

    // The hero also turns its head and reports its events (only its footsteps are heard).
    CharacterId addCharacter(const glm::mat4& transform, bool feet, bool hero,
                             const assets::AssetRef<animation::GraphAsset>& graph) {
        animation::CharacterDesc desc;
        desc.skeleton = m_skeleton;
        desc.clips.push_back(m_clips);
        desc.graph = graph;
        desc.transform = transform;
        if (feet) {
            animation::FootPlacementDesc f;
            f.legs = {{"thigh_l", "shin_l", "foot_l"}, {"thigh_r", "shin_r", "foot_r"}};
            f.pelvis = "pelvis";
            desc.feet = f;
        }
        if (hero)
            desc.lookAt = animation::LookAtDesc{
                .joints = {"head", "neck", "chest"}, .offset = glm::vec3(0.0f, 0.11f, 0.1f), .maxAngle = 1.2f};
        desc.events = hero;
        return m_anim->addCharacter(std::move(desc));
    }

    Walker makeWalker(const glm::vec3& position, float yaw, bool feet, bool hero) {
        return makeWalker(position, yaw, feet, hero, m_graph);
    }
    Walker makeWalker(const glm::vec3& position, float yaw, bool feet, bool hero,
                      const assets::AssetRef<animation::GraphAsset>& graph) {
        Walker w;
        w.position = position;
        w.yaw = yaw;
        w.transform = yawed(position, yaw);
        w.id = addCharacter(w.transform, feet, hero, graph);
        w.model = m_assets->addModel(m_model, w.transform);
        if (m_skinning != Skinning::Linear) m_assets->setSkinning(w.model, 0, m_skinning);
        return w;
    }

    // A showcase character: stands with its left forearm turned by kTwistAngle, skinned one way whatever the
    // section's choice.
    Walker makeTwisted(const glm::vec3& position, Skinning skinning) {
        Walker w = makeWalker(position, 0.0f, false, false);
        w.skinning = skinning;
        w.twist = kTwistAngle;
        m_assets->setSkinning(w.model, 0, skinning);
        return w;
    }

    void setSkinning(Skinning skinning) {
        m_skinning = skinning;
        forEachWalker([&](Walker& w) {
            if (!w.skinning) m_assets->setSkinning(w.model, 0, skinning);
        });
    }

    // The palette a walker is drawn with: its skin's, and for the showcase the forearm's joints (lowerarm_l and
    // hand_l, which follows it) turned about the line from the elbow to the wrist.
    std::span<const glm::mat4> palette(const Walker& w) {
        const std::span<const glm::mat4> palette = m_anim->skinPalette(w.id, w.skin);
        if (w.twist == 0.0f || m_forearmJoints.empty()) return palette;
        const int elbow = m_anim->findJoint(w.id, "lowerarm_l"), wrist = m_anim->findJoint(w.id, "hand_l");
        if (elbow < 0 || wrist < 0) return palette;
        const std::span<const glm::mat4> models = m_anim->modelMatrices(w.id);
        const glm::vec3 e(models[std::size_t(elbow)][3]), h(models[std::size_t(wrist)][3]);
        const glm::mat4 turn = glm::translate(glm::mat4(1.0f), e) *
                               glm::rotate(glm::mat4(1.0f), w.twist, glm::normalize(h - e)) *
                               glm::translate(glm::mat4(1.0f), -e);
        m_twisted.assign(palette.begin(), palette.end());
        for (const std::size_t k : m_forearmJoints)
            if (k < m_twisted.size()) m_twisted[k] = turn * m_twisted[k];
        return m_twisted;
    }

    Walker makeCircleWalker(const glm::vec2& center, float radius, float angle, float turn, bool feet, bool hero) {
        const glm::vec3 p(center.x + radius * std::sin(angle), 0.0f, center.y + radius * std::cos(angle));
        Walker w = makeWalker(p, angle + turn * 0.5f * kPi, feet, hero);
        w.center = center;
        w.radius = radius;
        w.angle = angle;
        w.turn = turn;
        return w;
    }

    void removeWalker(Walker& w) {
        if (w.id) m_anim->removeCharacter(w.id);
        if (w.model) m_assets->removeModel(w.model);
        w = {};
    }

    // One of the retargeting pair, on its circle at kPairStart: the mannequin or the strider, walking only.
    Walker makePairWalker(bool strider) {
        Walker w;
        w.center = kPairCenter;
        w.radius = strider ? kStriderScale * kPairRadius : kPairRadius;
        w.angle = kPairStart;
        w.position =
            glm::vec3(kPairCenter.x + w.radius * std::sin(w.angle), 0.0f, kPairCenter.y + w.radius * std::cos(w.angle));
        w.yaw = w.angle + 0.5f * kPi;
        w.transform = yawed(w.position, w.yaw);
        w.strider = strider;
        animation::CharacterDesc desc;
        desc.skeleton = strider ? m_striderSkeleton : m_skeleton;
        desc.clips.push_back(strider ? m_striderClips : m_clips);
        desc.graph = m_walkGraph;
        desc.transform = w.transform;
        w.id = m_anim->addCharacter(std::move(desc));
        w.model = m_assets->addModel(strider ? m_striderModel : m_model, w.transform);
        if (m_skinning != Skinning::Linear) m_assets->setSkinning(w.model, 0, m_skinning);
        return w;
    }

    // The camera of the retargeting view on the pair's midpoint, turned as far as the pair turned on its circles
    // (from `angle`, the mannequin's angle before its last move).
    void followPair(float angle) {
        if (!m_pairMannequin.id) return;
        const glm::vec3 mid = 0.5f * (m_pairMannequin.position + m_pairStrider.position);
        camera.target = glm::vec3(mid.x, camera.target.y, mid.z);
        camera.yaw += std::remainder(m_pairMannequin.angle - angle, 2.0f * kPi);
    }

    // Every character from the start: new state machines, phases and foot placement, the same on every run.
    void restart() {
        for (Walker* w : singles(*this)) removeWalker(*w);
        m_hero = makeCircleWalker(kHeroCenter, kHeroRadius, kHeroStart, 1.0f, true, true);
        m_slopeStand = makeWalker(slopeTop(), 0.0f, true, false);
        // Across the step's edge: the left foot (+X) above the step, the right one on the floor.
        m_stepStand = makeWalker(kStepAt, 0.0f, true, false);
        m_anim->setPhase(m_slopeStand.id, 0.3f);
        m_anim->setPhase(m_stepStand.id, 0.65f);
        // Steering by itself walks a circle of radius kSteerSpeed / kSteerTurn, first ahead (+Z) and then to the left.
        const glm::vec2 start = kSpaceCenter - glm::vec2(kSteerSpeed / kSteerTurn, 0.0f);
        m_strafer = makeWalker(glm::vec3(start.x, 0.0f, start.y), 0.0f, true, false, m_graph2d);
        m_steerAngle = 0.0f;
        m_steerInput = m_strafeMotion = glm::vec2(0.0f);
        m_twistLinear = makeTwisted(kTwistAt - glm::vec3(0.5f * kTwistApart, 0.0f, 0.0f), Skinning::Linear);
        m_twistDual = makeTwisted(kTwistAt + glm::vec3(0.5f * kTwistApart, 0.0f, 0.0f), Skinning::DualQuaternion);
        m_pairMannequin = makePairWalker(false);
        m_pairStrider = makePairWalker(true);
        removeWalker(m_blob);
        animation::CharacterDesc blob;
        blob.skeleton = m_blobSkeleton;
        blob.clips.push_back(m_blobClips);
        blob.graph = m_blobGraph;
        blob.transform = placed(kBlobAt);
        blob.events = false;
        blob.height = 1.0f;
        m_blob.id = m_anim->addCharacter(std::move(blob));
        m_blob.transform = placed(kBlobAt);
        m_blob.model = m_assets->addModel(m_blobModel, m_blob.transform, false);
        if (std::string_view(kViews[std::size_t(m_view)].id) == "retarget") setView(m_view);
        setCrowd(m_crowdSize);
        m_footsteps = 0;
        m_landings = 0;
        m_teleport = true;
    }

    // Rings around kCrowdCenter, filled from the inside; a ring walks at one speed (walk, between, run), so its
    // characters keep their spacing.
    void setCrowd(int count) {
        for (Walker& w : m_crowd) removeWalker(w);
        m_crowd.clear();
        m_crowdSize = count;
        Random random;
        for (int ring = 0; int(m_crowd.size()) < count; ++ring) {
            const float radius = kCrowdFirstRing + kCrowdRingStep * float(ring);
            const int slots = std::max(1, int(2.0f * kPi * radius / kCrowdSpacing));
            const float speed = std::array{1.1f, 1.8f, 2.7f}[std::size_t(ring % 3)];
            const float turn = ring % 2 == 0 ? 1.0f : -1.0f;
            for (int i = 0; i < slots && int(m_crowd.size()) < count; ++i) {
                Walker w =
                    makeCircleWalker(kCrowdCenter, radius, 2.0f * kPi * float(i) / float(slots), turn, false, false);
                m_anim->setFloat(w.id, "speed", speed);
                m_anim->setPhase(w.id, random.uniform());
                m_crowd.push_back(w);
            }
        }
        m_teleport = true;
    }

    // Skins need the model's joint names and inverse bind matrices: added once it is ready.
    void bindSkins() {
        const auto skinOf = [](const assets::AssetRef<ModelAsset>& model) -> const assets::ModelSkin* {
            return model.ready() && !model->skins().empty() ? &model->skins()[0] : nullptr;
        };
        const assets::ModelSkin* mannequinSkin = skinOf(m_model);
        const assets::ModelSkin* striderSkin = skinOf(m_striderModel);
        if (mannequinSkin && m_forearmJoints.empty())
            for (std::size_t k = 0; k < mannequinSkin->joints.size(); ++k)
                if (mannequinSkin->joints[k] == "lowerarm_l" || mannequinSkin->joints[k] == "hand_l")
                    m_forearmJoints.push_back(k);
        forEachWalker([&](Walker& w) {
            const assets::ModelSkin* skin = w.strider ? striderSkin : mannequinSkin;
            if (w.skin < 0 && w.id && skin) w.skin = m_anim->addSkin(w.id, skin->joints, skin->inverseBindMatrices);
        });
        if (const assets::ModelSkin* skin = skinOf(m_blobModel); skin && m_blob.skin < 0 && m_blob.id)
            m_blob.skin = m_anim->addSkin(m_blob.id, skin->joints, skin->inverseBindMatrices);
    }

    // The blob: drawn in its view only, posed by its clip, its blend shapes' weights from the sliders (by name, in
    // its model's order).
    void updateBlob() {
        if (!m_blob.model) return;
        m_assets->setVisible(m_blob.model, std::string_view(kViews[std::size_t(m_view)].id) == "blendshapes");
        if (m_blob.skin >= 0) m_assets->setJointPalette(m_blob.model, 0, m_anim->skinPalette(m_blob.id, m_blob.skin));
        if (!m_blobModel.ready()) return;
        m_blobModelWeights.clear();
        for (const std::string& name : m_blobModel->morphTargets()) {
            const auto it = std::ranges::find(kBlobShapes, name);
            m_blobModelWeights.push_back(
                it == kBlobShapes.end() ? 0.0f : m_blobWeights[std::size_t(it - kBlobShapes.begin())]);
        }
        m_assets->setMorphWeights(m_blob.model, m_blobModelWeights);
    }

    // Moves a walker on by the root motion of the last update.
    void walk(Walker& w) {
        if (w.radius <= 0.0f) return;
        const animation::RootMotion motion = m_anim->rootMotion(w.id);
        const float distance = glm::length(glm::vec2(motion.translation.x, motion.translation.z));
        w.angle = std::fmod(w.angle + w.turn * distance / w.radius, 2.0f * kPi);
        w.position =
            glm::vec3(w.center.x + w.radius * std::sin(w.angle), 0.0f, w.center.y + w.radius * std::cos(w.angle));
        w.yaw = w.angle + w.turn * 0.5f * kPi;
        w.transform = yawed(w.position, w.yaw);
        m_anim->setTransform(w.id, w.transform);
    }

    void setView(int view) {
        m_view = view;
        const View& v = kViews[std::size_t(view)];
        camera.target = v.target;
        camera.yaw = glm::radians(v.yaw);
        camera.pitch = glm::radians(v.pitch);
        camera.distance = v.distance;
        cameraCut = true;
    }

    // The 2D blend space's parameters: the velocity in the character's frame (it faces +Z, its left is +X), by
    // itself a direction that turns around the circle. The part leaving the circle fades out at its edge.
    void steer(float dt) {
        glm::vec2 v = m_steerVelocity;
        if (m_autoSteer) {
            v = kSteerSpeed * glm::vec2(std::sin(m_steerAngle), std::cos(m_steerAngle));
            m_steerAngle = std::fmod(m_steerAngle + kSteerTurn * dt, 2.0f * kPi);
        }
        const glm::vec2 from = glm::vec2(m_strafer.position.x, m_strafer.position.z) - kSpaceCenter;
        const float r = glm::length(from);
        if (r > 1e-4f) {
            const glm::vec2 out = from / r;
            const float leaving = glm::dot(v, out);
            const float edge = glm::smoothstep(kSpaceRadius - kSpaceEdge, kSpaceRadius, r);
            if (leaving > 0.0f) v -= out * leaving * edge;
        }
        m_steerInput = v;
        m_anim->setFloat(m_strafer.id, "vx", v.x);
        m_anim->setFloat(m_strafer.id, "vz", v.y);
    }

    // Moves the steered character on by the root motion of the last update, its facing unchanged.
    void moveStrafer(float dt) {
        if (!m_strafer.id) return;
        const glm::vec3 t = glm::mat3(m_strafer.transform) * m_anim->rootMotion(m_strafer.id).translation;
        m_strafer.position += glm::vec3(t.x, 0.0f, t.z);
        m_strafer.transform = yawed(m_strafer.position, m_strafer.yaw);
        m_anim->setTransform(m_strafer.id, m_strafer.transform);
        if (dt > 0.0f) {
            const float k = 1.0f - std::exp(-dt / 0.2f);
            m_strafeMotion = glm::mix(m_strafeMotion, glm::vec2(t.x, t.z) / dt, k);
        }
    }

    void footsteps(Context& c) {
        audio::AudioSystem& a = c.app.audio();
        for (const animation::Event& e : m_anim->events()) {
            if (e.character != m_hero.id) continue;
            const bool land = e.name == "land";
            if (!land && e.name != "footstep") continue;
            (land ? m_landings : m_footsteps) += 1;
            if (!m_footstepSound || !m_step.ready()) continue;
            const char* foot = e.value < 0.5f ? "toe_l" : "toe_r";
            const glm::vec3 at(m_anim->jointWorld(m_hero.id, m_anim->findJoint(m_hero.id, foot))[3]);
            const float loud = land ? 1.0f : 0.45f + 0.35f * std::clamp(m_speed / kMaxSpeed, 0.0f, 1.0f);
            a.play(m_step, {.bus = a.bus("effects"),
                            .volume = loud * std::min(e.weight * 1.5f, 1.0f),
                            .pitch = 0.92f + 0.16f * m_random.uniform(),
                            .spatial = true,
                            .emitter = {.position = at, .minDistance = 2.0f, .maxDistance = 40.0f}});
        }
    }

    std::optional<Scene> m_scene;
    std::unique_ptr<PhysicsWorld> m_world;
    JPH::Ref<JPH::Shape> m_slopeShape;
    JPH::BodyID m_slopeBody;
    InstanceId m_slopeInstance, m_stick, m_flame;
    glm::mat4 m_stickTransform{1.0f};
    std::unique_ptr<assets::AssetManager> m_manager;
    std::optional<SceneAssets> m_assets;
    std::unique_ptr<AnimationSystem> m_anim;
    assets::AssetRef<ModelAsset> m_model;
    assets::AssetRef<animation::SkeletonAsset> m_skeleton;
    assets::AssetRef<animation::ClipSetAsset> m_clips;
    assets::AssetRef<animation::GraphAsset> m_graph, m_graph2d, m_walkGraph;
    assets::AssetRef<ModelAsset> m_striderModel;
    assets::AssetRef<animation::SkeletonAsset> m_striderSkeleton;
    assets::AssetRef<animation::ClipSetAsset> m_striderClips;
    assets::AssetRef<ModelAsset> m_blobModel;
    assets::AssetRef<animation::SkeletonAsset> m_blobSkeleton;
    assets::AssetRef<animation::ClipSetAsset> m_blobClips;
    assets::AssetRef<animation::GraphAsset> m_blobGraph;
    assets::AssetRef<audio::SoundAsset> m_step;
    Walker m_hero, m_slopeStand, m_stepStand, m_strafer, m_twistLinear, m_twistDual, m_pairMannequin, m_pairStrider;
    std::vector<Walker> m_crowd;
    Walker m_blob;
    std::array<float, kBlobShapes.size()> m_blobWeights{};
    std::vector<float> m_blobModelWeights;
    std::vector<std::size_t> m_forearmJoints; // in the model's skin
    std::vector<glm::mat4> m_twisted;
    app::ActionId m_jumpAction = 0, m_waveAction = 0;
    float m_speed = 1.1f, m_lean = 0.0f, m_slopeDegrees = 16.0f;
    int m_crowdSize = 48;
    bool m_feet = true, m_lookAt = true, m_footstepSound = true, m_teleport = true;
    int m_footsteps = 0, m_landings = 0;
    int m_view = 0;
    bool m_autoSteer = true;
    bool m_lod = false, m_lodInterpolate = true;
    Skinning m_skinning = Skinning::Linear;
    std::uint32_t m_skinnedReused = 0;
    float m_steerAngle = 0.0f;
    glm::vec2 m_steerVelocity{0.0f}, m_steerInput{0.0f}, m_strafeMotion{0.0f};
    std::array<float, kRecentFrames> m_recentUpdateMs{}, m_recentSkinningMs{}, m_recentEvaluated{};
    std::size_t m_recentNext = 0, m_recentCount = 0;
    Random m_random;
};

} // namespace

std::unique_ptr<Section> makeAnimationSection() { return std::make_unique<AnimationSection>(); }

} // namespace demo
