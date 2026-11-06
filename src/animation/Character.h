#pragma once

// A character's runtime data (AnimationSystem::Character): what binding resolved from its assets, the state of its
// layers, the ozz buffers of one update, and the update's outputs. Character.cpp evaluates it, Ik.cpp adds the IK.

#include <quench/animation/AnimationSystem.h>

#include "OzzImpl.h"

#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quench::animation {

struct AnimationSystem::Character {
    // A clip a state plays, with its own sampling context (ozz keeps the keyframe cursor in it).
    struct ClipSlot {
        const Clip* clip = nullptr; // nullptr: missing from the clip sets (the rest pose instead)
        float at = 0.0f;            // blend trees: the parameter value
        int context = 0;
    };
    struct StateRuntime {
        const StateDef* def = nullptr;
        std::vector<ClipSlot> clips; // one, a blend tree's points, or none (empty state)
        bool loop = true;
    };
    struct LayerRuntime {
        const LayerDef* def = nullptr;
        std::vector<StateRuntime> states;
        ozz::vector<ozz::math::SimdFloat4> mask; // per SoA joint, 0..1; empty: every joint
        int current = 0;
        float time = 0.0f; // normalized time of the current state (counts on over loops)
        bool entered = true;
        int next = -1; // transition target
        float nextTime = 0.0f;
        bool nextEntered = true;
        float fade = 0.0f, fadeDuration = 0.0f; // seconds
    };
    struct EventRecord {
        const ClipEvent* event = nullptr;
        const Clip* clip = nullptr;
        float weight = 0.0f;
        std::uint32_t count = 1;
        int layer = 0;
    };
    struct Skin {
        std::vector<std::string> joints;
        std::vector<glm::mat4> inverseBind;
        std::vector<int> map; // skin joint -> skeleton joint (-1: none)
        std::vector<glm::mat4> palette;
    };
    struct Leg {
        int hip = -1, knee = -1, ankle = -1;
        glm::vec3 kneeAxis{1.0f, 0.0f, 0.0f};
        float offset = 0.0f; // smoothed ground offset (m, world up)
        LegState state;
    };
    struct LookChainJoint {
        int joint = -1;
        glm::vec3 forward{0.0f, 0.0f, 1.0f}, up{0.0f, 1.0f, 0.0f}, offset{0.0f}; // joint space
    };

    CharacterDesc desc;
    CharacterId id;
    float timeScale = 1.0f;
    float pendingPhase = -1.0f;
    bool feetEnabled = true;
    std::optional<glm::vec3> lookTarget;
    float lookWeight = 0.0f; // wanted
    float lookBlend = 0.0f;  // smoothed

    // Parameters by the graph's index; values set before the graph was ready wait in `pending`.
    std::vector<float> params;
    std::vector<std::pair<std::string, float>> pending;
    std::vector<std::string> unknownParameters; // logged once each

    // Bound data (nullptr / empty before binding).
    std::shared_ptr<const Skeleton> skeleton;
    std::vector<std::shared_ptr<const ClipSet>> clipSets;
    std::shared_ptr<const GraphDef> graph;
    bool bound = false;
    bool loggedFailure = false;
    std::vector<LayerRuntime> layers;
    std::vector<std::unique_ptr<ozz::animation::SamplingJob::Context>> contexts;
    std::vector<Leg> legs;
    int pelvis = -1;
    float pelvisOffset = 0.0f;
    bool groundSnapped = false; // the first update snaps the offsets instead of smoothing them
    std::vector<LookChainJoint> lookChain;
    bool lookReached = false;
    glm::vec3 lastLookTarget{0.0f}; // model space: the turn fades out toward it after the target is cleared

    // Pose buffers of one update (SoA, local space) and the model-space matrices.
    ozz::vector<ozz::math::SoaTransform> sample[2], statePose[2], layerPose, pose, scratch;
    ozz::vector<ozz::math::SimdFloat4> weightsA, weightsB;
    ozz::vector<ozz::math::Float4x4> models;
    std::vector<glm::mat4> modelMatrices;
    std::vector<Skin> skins;

    // Outputs of the last update.
    std::vector<EventRecord> events;
    RootMotion rootMotion;
    std::vector<LegState> legStates;
    std::uint32_t clipsSampled = 0, groundQueries = 0;
};

namespace detail {

// One update of a bound or unbound character (job threads; touches only the character and immutable assets).
void evaluate(AnimationSystem::Character& c, float dt, const AnimationSettings& settings, const GroundQuery& ground);
// Ik.cpp, called by evaluate() after the layers, with `models` up to date.
void placeFeet(AnimationSystem::Character& c, float dt, const GroundQuery& ground);
void lookAt(AnimationSystem::Character& c, float dt);
// Recomputes the model-space matrices of `from` and the joints below it (every joint for -1).
void updateModels(AnimationSystem::Character& c, int from = -1);

} // namespace detail

} // namespace quench::animation
