#pragma once

// Animated characters: a skeleton, clip sets and a graph (state machine) per character, updated together once per
// frame on the job system (characters in parallel; with zero worker threads on the calling thread). An update
// advances the state machines, samples and blends the clips, applies the layers, IK (look-at, foot placement on the
// ground), and computes the model-space joint matrices, the skinning palettes, the events passed and the root motion.
// With animation LOD (AnimationLod) characters small on the screen update every 2nd or 4th frame and leave out IK.
//
// Skeletons (.qskel), clip sets (.qanim) and graphs (.json) load through the asset manager as the types "skeleton",
// "animation" and "animgraph". Until a character's skeleton is ready its palettes are identity (the mesh in its bind
// pose); until its clips and graph are ready, and when they fail, it holds the skeleton's rest pose. Every failure is
// one log line. A reload (hot reload) rebinds the characters that use the asset.
//
// Main thread only, except the ground query, which runs on the job system's threads.

#include <quench/animation/Clip.h>
#include <quench/animation/Graph.h>
#include <quench/animation/Skeleton.h>
#include <quench/assets/AssetManager.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Jobs.h>
#include <quench/core/Result.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::animation {

class AnimationSystem;

class SkeletonAsset final : public assets::Asset {
public:
    // Empty while loading and after a failure; replaced by a reload.
    const std::shared_ptr<const Skeleton>& data() const { return m_data; }

private:
    friend class AnimationSystem;
    SkeletonAsset() = default;
    Result<void> decode(fs::Bytes bytes) override;
    std::uint64_t uploadBytes() const override { return 0; }
    Result<void> finish() override;
    void unload() override { m_data.reset(); }
    assets::AssetMemory residentMemory() const override { return {m_data ? m_data->memoryBytes() : 0, 0}; }

    std::shared_ptr<const Skeleton> m_data, m_pending;
};

class ClipSetAsset final : public assets::Asset {
public:
    const std::shared_ptr<const ClipSet>& data() const { return m_data; }

private:
    friend class AnimationSystem;
    ClipSetAsset() = default;
    Result<void> decode(fs::Bytes bytes) override;
    std::uint64_t uploadBytes() const override { return 0; }
    Result<void> finish() override;
    void unload() override { m_data.reset(); }
    assets::AssetMemory residentMemory() const override { return {m_data ? m_data->memoryBytes() : 0, 0}; }

    std::shared_ptr<const ClipSet> m_data, m_pending;
};

class GraphAsset final : public assets::Asset {
public:
    const std::shared_ptr<const GraphDef>& data() const { return m_data; }

private:
    friend class AnimationSystem;
    GraphAsset() = default;
    Result<void> decode(fs::Bytes bytes) override;
    std::uint64_t uploadBytes() const override { return 0; }
    Result<void> finish() override;
    void unload() override { m_data.reset(); }
    assets::AssetMemory residentMemory() const override { return {m_bytes, 0}; }

    std::shared_ptr<const GraphDef> m_data, m_pending;
    std::uint64_t m_bytes = 0, m_pendingBytes = 0;
};

struct CharacterId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0; // 0: null

    explicit operator bool() const { return generation != 0; }
    friend bool operator==(CharacterId, CharacterId) = default;
};

// A leg for foot placement: three joints in a chain (hip, knee, ankle).
struct LegDesc {
    std::string hip, knee, ankle;
    // The axis in the knee's joint space a positive rotation around which straightens the leg: (-1, 0, 0) for
    // joints oriented like the model (rest rotations of identity), facing +Z with the knees bending forward.
    glm::vec3 kneeAxis{-1.0f, 0.0f, 0.0f};
};

struct FootPlacementDesc {
    std::vector<LegDesc> legs;
    std::string pelvis;         // moved down (or up) so that the lower foot reaches the ground
    float maxStepUp = 0.45f;    // m: ground higher than this under a foot is not reached for
    float maxStepDown = 0.6f;   // m
    float rayHeight = 0.8f;     // m: the ground ray starts this far above the character's base
    float smoothing = 12.0f;    // 1/s: how fast offsets follow the ground (exponential)
    float alignToGround = 1.0f; // 0..1: the feet turn to the ground's slope
    float soften = 1.0f;        // TwoBoneIk::soften of the legs (1: none, exact on flat ground)
};

// The head (and the joints below it) turn toward a target: ozz-animation's aim IK down a chain.
struct LookAtDesc {
    std::vector<std::string> joints;     // the head first, then down the spine
    glm::vec3 forward{0.0f, 0.0f, 1.0f}; // the character's facing in model space, in its rest pose
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 offset{0.0f};   // the eyes relative to the first joint, in model space at rest
    float chainWeight = 0.5f; // share of every joint but the last, which turns the rest of the way to the target
    float maxAngle = 1.4f;    // radians from the facing; targets further aside are not followed
};

struct CharacterDesc {
    assets::AssetRef<SkeletonAsset> skeleton;
    std::vector<assets::AssetRef<ClipSetAsset>> clips; // clips are found by name, the first set first
    assets::AssetRef<GraphAsset> graph;
    glm::mat4 transform{1.0f};             // model -> world
    std::optional<FootPlacementDesc> feet; // needs AnimationSystem::setGroundQuery
    std::optional<LookAtDesc> lookAt;
    bool events = true;  // collect the clip events this character passes
    float height = 1.8f; // m above its origin: its size on the screen for animation LOD
};

// One event a character passed during the last update.
struct Event {
    CharacterId character;
    std::string_view name; // the clip's event name (valid until the next update)
    std::string_view clip;
    float value = 0.0f;
    float weight = 0.0f;     // of the clip in the pose
    std::uint32_t count = 1; // more than one: an update longer than the clip passed the marker that often
    int layer = 0;
};

struct GroundHit {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};
// Casts a ray straight down from `from` (world space) at most `length` meters. Called from job threads, several at
// once: it must be safe to call concurrently (a physics world's queries while it is not stepping).
using GroundQuery = std::function<std::optional<GroundHit>(const glm::vec3& from, float length)>;

// Motion the clips moved the character during the last update, in its model space: for the game to apply to its
// transform (a character that moves by its clips' root motion does not slide its feet).
struct RootMotion {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct LegState {
    bool reached = true;   // the two-bone IK reached its target
    bool grounded = false; // the ground query hit below the foot
    float offset = 0.0f;   // m the foot moved to the ground (smoothed)
};

struct CharacterState {
    bool bound = false;          // skeleton, clips and graph ready and checked
    std::string_view layerState; // the first layer's current state (empty before binding)
    std::string_view nextState;  // its transition target; empty when not in a transition
    float transition = 0.0f;     // 0..1 through the transition
    float normalizedTime = 0.0f; // the current state's time / duration (keeps counting over loops)
    float pelvisOffset = 0.0f;   // foot placement
    std::span<const LegState> legs;
    bool lookAtReached = false;
    int lodRate = 1;         // animation LOD: the clips are sampled every lodRate-th frame
    bool lodIk = true;       // animation LOD: foot placement and look-at run (when the character has them)
    bool poseChanged = true; // the last update changed the joint matrices (false: held, the palettes as before)
};

struct AnimationStats {
    std::uint32_t characters = 0;
    std::uint32_t bound = 0;
    std::uint32_t joints = 0;       // joints posed in the last update
    std::uint32_t clipsSampled = 0; // clip samplings in the last update
    std::uint32_t groundQueries = 0;
    std::uint32_t events = 0;
    double updateMs = 0.0; // wall time of the last update (sampling, blending, IK, palettes)
    int threads = 1;
    // Animation LOD: characters whose clips were sampled in the last update (the others interpolated or held their
    // pose), and the characters at each rate (every frame, every 2nd, every 4th).
    std::uint32_t evaluated = 0;
    std::uint32_t lodRates[3] = {};
};

// Animation LOD: a character's height on the screen (a fraction of the view's height, from setLodView) picks how
// often its clips are sampled: every frame, every 2nd or every 4th, the characters at one rate staggered over the
// frames. Between its updates a character's pose is interpolated (each update is computed one period ahead, so the
// pose shown is not late) or held (the palettes do not change, so the renderer reuses the skinned vertices). Below
// `ik` foot placement and look-at are off. Root motion is spread over the frames of a period, so a character moved
// by it does not jump; events come with the updates (in interpolation up to a period early). A size is left for a
// finer level only past its threshold by `hysteresis` (relative), so a character on a threshold does not flicker.
// With the defaults and a 50 degree view a 1.8 m character updates every frame within 12.9 m, every 4th frame past
// 32 m, and leaves out IK past 19 m.
struct AnimationLod {
    bool enabled = false;
    float half = 0.15f;    // below: every 2nd frame
    float quarter = 0.06f; // below: every 4th frame
    float ik = 0.1f;       // below: no foot placement or look-at
    float hysteresis = 0.15f;
    bool interpolate = true; // false: held between updates
};

struct AnimationSettings {
    std::uint32_t charactersPerJob = 4; // the smallest range of characters one job updates
    float eventWeight = 0.5f;           // a state delivers events from this weight on (one of a cross-fade's two)
    assets::AssetMemory skeletonBudget = assets::AssetManager::kUnlimited;
    assets::AssetMemory clipBudget = assets::AssetManager::kUnlimited;
    assets::AssetMemory graphBudget = assets::AssetManager::kUnlimited;
    AnimationLod lod; // off: every character every frame
};

class AnimationSystem {
public:
    // Registers the asset types; unregisters them when destroyed (references to them must be gone by then).
    AnimationSystem(assets::AssetManager& assets, JobSystem& jobs, const AnimationSettings& settings = {});
    ~AnimationSystem();
    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    assets::AssetRef<SkeletonAsset> loadSkeleton(std::string_view path);
    assets::AssetRef<ClipSetAsset> loadClips(std::string_view path);
    assets::AssetRef<GraphAsset> loadGraph(std::string_view path);

    CharacterId addCharacter(CharacterDesc desc);
    void removeCharacter(CharacterId character);
    bool contains(CharacterId character) const;
    std::uint32_t characterCount() const;

    void setTransform(CharacterId character, const glm::mat4& transform);
    const glm::mat4& transform(CharacterId character) const;
    void setGroundQuery(GroundQuery query);
    // Foot placement and look-at on or off (with a desc).
    void setFootPlacement(CharacterId character, bool enabled);
    // nullopt: look ahead again (the turn fades out).
    void setLookAt(CharacterId character, std::optional<glm::vec3> worldTarget, float weight = 1.0f);
    // Multiplies the update's dt for this character (0 freezes it).
    void setTimeScale(CharacterId character, float scale);

    // Animation LOD (AnimationSettings::lod): the settings, and the camera the characters' screen sizes are measured
    // from (fovY: the vertical field of view in radians). Without a view, or with LOD off, every character updates
    // every frame with its IK.
    void setLod(const AnimationLod& lod);
    const AnimationLod& lod() const { return m_settings.lod; }
    void setLodView(const glm::vec3& eye, float fovY);
    void clearLodView();
    // Moves the first layer's current state to a normalized time (spreads a crowd's phases); takes effect once bound.
    void setPhase(CharacterId character, float normalizedTime);

    // Graph parameters by name; set before the graph is bound, they apply when it is. An unknown name or a wrong type
    // (a trigger set as a float) is logged and ignored: graphs are data and can change under a running game.
    void setFloat(CharacterId character, std::string_view parameter, float value);
    void setBool(CharacterId character, std::string_view parameter, bool value);
    // Set until a transition consumes it (or resetTrigger()).
    void trigger(CharacterId character, std::string_view parameter);
    void resetTrigger(CharacterId character, std::string_view parameter);
    float parameter(CharacterId character, std::string_view parameter) const;
    // Jumps the layer to a state without a cross-fade (a respawn); false: no such layer or state.
    bool setState(CharacterId character, std::string_view layer, std::string_view state);

    // Binds characters whose assets became ready or changed, then updates every character by `dt` seconds on the
    // job system and waits for it.
    void update(float dt);

    // After update(): valid until the next update or removal.
    CharacterState state(CharacterId character) const;
    std::span<const glm::mat4> modelMatrices(CharacterId character) const; // empty without a skeleton
    // A joint in world space (the transform times its model matrix): attachments (a held object). The identity
    // matrix for an unknown joint or before the skeleton is ready.
    glm::mat4 jointWorld(CharacterId character, int joint) const;
    int findJoint(CharacterId character, std::string_view name) const; // -1 before the skeleton is ready
    RootMotion rootMotion(CharacterId character) const;
    std::span<const Event> events() const { return m_events; }

    // Skins: a mesh's joints by name and their inverse bind matrices (render::SceneAssets has them for cooked
    // models). The palette (model matrix times inverse bind matrix per skin joint) is computed in update(); identity
    // matrices until the skeleton is ready and for joints the skeleton does not have.
    int addSkin(CharacterId character, std::span<const std::string> joints,
                std::span<const glm::mat4> inverseBindMatrices);
    std::span<const glm::mat4> skinPalette(CharacterId character, int skin) const;

    const AnimationStats& stats() const { return m_stats; }

    struct Character; // internal (src/animation/Character.h)

private:
    Character* get(CharacterId character);
    const Character* get(CharacterId character) const;
    void bind(Character& character);

    assets::AssetManager& m_assets;
    JobSystem& m_jobs;
    AnimationSettings m_settings;
    GroundQuery m_ground;
    struct Slot {
        std::unique_ptr<Character> character;
        std::uint32_t generation = 1;
    };
    std::vector<Slot> m_slots;
    std::vector<std::uint32_t> m_free;
    std::vector<Character*> m_active; // the update's order (slot order)
    std::vector<Event> m_events;
    AnimationStats m_stats;
    struct LodView {
        glm::vec3 eye{0.0f};
        float fovY = 1.0f;
    };
    std::optional<LodView> m_lodView;
    std::uint64_t m_frame = 0;
};

} // namespace quench::animation
