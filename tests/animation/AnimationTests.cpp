#include "Test.h"

#include <quench/animation/AnimationSystem.h>
#include <quench/animation/Ik.h>
#include <quench/assets/AssetManager.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Jobs.h>
#include <quench/core/Log.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace quench;
using namespace quench::animation;

namespace {

// A small figure: root, pelvis 1 m up, spine and head above it, two legs with slightly bent knees (ankles 5 cm above
// the ground). Joints have the model's orientation (identity rest rotations), facing +Z.
SkeletonDesc figure() {
    SkeletonDesc d;
    const auto add = [&](const char* name, int parent, glm::vec3 t) {
        SkeletonDesc::Joint j;
        j.name = name;
        j.parent = parent;
        j.rest.translation = t;
        d.joints.push_back(j);
    };
    add("root", -1, {0.0f, 0.0f, 0.0f});
    add("pelvis", 0, {0.0f, 1.0f, 0.0f});
    add("spine", 1, {0.0f, 0.3f, 0.0f});
    add("head", 2, {0.0f, 0.3f, 0.0f});
    add("thigh_l", 1, {0.1f, -0.05f, 0.0f});
    add("knee_l", 4, {0.0f, -0.45f, 0.02f});
    add("ankle_l", 5, {0.0f, -0.45f, -0.02f});
    add("thigh_r", 1, {-0.1f, -0.05f, 0.0f});
    add("knee_r", 7, {0.0f, -0.45f, 0.02f});
    add("ankle_r", 8, {0.0f, -0.45f, -0.02f});
    return d;
}
enum Joint { kRoot, kPelvis, kSpine, kHead };

ClipDesc clip(const char* name, float duration, bool loop = true) {
    ClipDesc c;
    c.name = name;
    c.duration = duration;
    c.loop = loop;
    c.tracks.resize(figure().joints.size());
    return c;
}

void translate(ClipDesc& c, int joint, std::vector<Key<glm::vec3>> keys) {
    c.tracks[std::size_t(joint)].translations = keys;
}

// The clips of most cases. Translations are stored as half floats: values are checked to a few millimeters.
std::vector<ClipDesc> testClips() {
    std::vector<ClipDesc> clips;
    ClipDesc keys = clip("keys", 1.0f, false); // pelvis x: 0, 2, 1 at 0, 0.5, 1 s
    translate(keys, kPelvis, {{0.0f, {0, 1, 0}}, {0.5f, {2, 1, 0}}, {1.0f, {1, 1, 0}}});
    clips.push_back(keys);
    clips.push_back(clip("a", 1.0f)); // the rest pose
    ClipDesc b = clip("b", 2.0f);     // pelvis 2 m to the side
    translate(b, kPelvis, {{0.0f, {2, 1, 0}}});
    clips.push_back(b);
    ClipDesc walk = clip("walk", 1.0f);
    walk.events = {{0.75f, "foot_r", 2.0f}, {0.25f, "foot_l", 1.0f}};
    clips.push_back(walk);
    ClipDesc jump = clip("jump", 0.5f, false);
    jump.events = {{0.5f, "land", 0.0f}};
    translate(jump, kPelvis, {{0.0f, {0, 1, 0}}, {0.25f, {0, 1.5f, 0}}, {0.5f, {0, 1, 0}}});
    clips.push_back(jump);
    ClipDesc move = clip("move", 1.0f); // 1 m forward per loop, extracted as root motion
    translate(move, kRoot, {{0.0f, {0, 0, 0}}, {1.0f, {0, 0, 1}}});
    move.rootMotion = RootMotionDesc{"root", false};
    clips.push_back(move);
    ClipDesc upper = clip("upper", 1.0f); // spine 0.5 m aside, pelvis 3 m aside
    translate(upper, kSpine, {{0.0f, {0.5f, 0.3f, 0}}});
    translate(upper, kPelvis, {{0.0f, {3, 1, 0}}});
    clips.push_back(upper);
    ClipDesc nod = clip("nod", 1.0f); // additive: the head 0.2 m forward over the clip
    translate(nod, kHead, {{0.0f, {0, 0.3f, 0}}, {1.0f, {0, 0.3f, 0.2f}}});
    nod.additive = true;
    clips.push_back(nod);
    return clips;
}

ClipBuildOptions exact() {
    ClipBuildOptions o;
    o.optimize = false;
    return o;
}

struct Built {
    std::shared_ptr<const Skeleton> skeleton;
    std::vector<std::uint8_t> skeletonFile, clipsFile;
};

Built build(const SkeletonDesc& desc, const std::vector<ClipDesc>& clips) {
    Built out;
    Result<std::shared_ptr<const Skeleton>> skeleton = buildSkeleton(desc);
    if (!skeleton) return out;
    out.skeleton = *skeleton;
    out.skeletonFile = writeSkeleton(**skeleton);
    Result<std::shared_ptr<const ClipSet>> set = buildClipSet(**skeleton, desc, clips, exact());
    if (set) out.clipsFile = writeClipSet(**set);
    return out;
}

// Asset files for one case, read through quench::fs; restores the default backend at the end.
struct AssetDir {
    explicit AssetDir(const char* name) : dir(std::filesystem::path(quench::test::tmpDir()) / name) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        fs::setBackend(fs::makeStdBackend(dir, {}));
    }
    ~AssetDir() { fs::setBackend(nullptr); }
    void write(const std::string& path, std::span<const std::uint8_t> bytes) const {
        std::ofstream(dir / path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    void writeText(const std::string& path, std::string_view text) const {
        std::ofstream(dir / path, std::ios::binary).write(text.data(), std::streamsize(text.size()));
    }

    std::filesystem::path dir;
};

// Counts the animation module's warnings while it lives.
struct WarningCounter {
    WarningCounter()
        : id(log::addSink([this](log::Level level, std::string_view category, std::string_view) {
              if (level == log::Level::Warning && category == "animation") ++count;
          })) {}
    ~WarningCounter() { log::removeSink(id); }
    std::atomic<int> count{0};
    int id;
};

struct Rig {
    explicit Rig(const char* name, int workers = 0)
        : dir(name), jobs(JobSystemDesc{.workerThreads = workers}), assets(jobs), anim(assets, jobs) {}

    void writeData(const std::vector<ClipDesc>& clips = testClips()) {
        const Built data = build(figure(), clips);
        dir.write("figure.qskel", data.skeletonFile);
        dir.write("figure.qanim", data.clipsFile);
    }
    CharacterId add(std::string_view graph, CharacterDesc desc = {}) {
        const std::string path = std::format("graph{}.json", graphs++);
        dir.writeText(path, graph);
        if (!desc.skeleton) desc.skeleton = anim.loadSkeleton("figure.qskel");
        if (desc.clips.empty()) desc.clips.push_back(anim.loadClips("figure.qanim"));
        desc.graph = anim.loadGraph(path);
        return anim.addCharacter(std::move(desc));
    }
    // Loads finish on the workers in their own time: wait for them by the clock, not by a number of updates.
    void load() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!assets.idle() && std::chrono::steady_clock::now() < deadline) {
            assets.update();
            std::this_thread::yield();
        }
    }
    void step(float dt) {
        assets.update();
        anim.update(dt);
    }
    glm::vec3 joint(CharacterId id, const char* name) const {
        const int j = anim.findJoint(id, name);
        if (j < 0) return glm::vec3(NAN);
        return glm::vec3(anim.modelMatrices(id)[std::size_t(j)][3]);
    }

    AssetDir dir;
    JobSystem jobs;
    assets::AssetManager assets;
    AnimationSystem anim;
    int graphs = 0;
};

std::string oneState(const char* clipName, const char* extra = "") {
    return std::format(R"({{"layers": [{{"name": "base", "states": [{{"name": "s", "clip": "{}"{}}}]}}]}})", clipName,
                       extra);
}

bool near(float a, float b, float tolerance = 3e-3f) { return std::abs(a - b) <= tolerance; }
bool near(const glm::vec3& a, const glm::vec3& b, float tolerance = 3e-3f) { return glm::length(a - b) <= tolerance; }

} // namespace

// ---- data

QUENCH_TEST(skeleton_build_and_round_trip) {
    const SkeletonDesc desc = figure();
    Result<std::shared_ptr<const Skeleton>> built = buildSkeleton(desc);
    REQUIRE(built, "build: %s", built ? "" : built.error().c_str());
    const Skeleton& s = **built;
    REQUIRE(s.jointCount() == 10, "joints %d", s.jointCount());
    CHECK(s.findJoint("knee_r") >= 0 && s.parent(s.findJoint("knee_r")) == s.findJoint("thigh_r"), "parents");
    CHECK(s.findJoint("nope") == -1, "unknown joint");
    CHECK(near(glm::vec3(s.restModelMatrices()[std::size_t(s.findJoint("head"))][3]), {0.0f, 1.6f, 0.0f}, 1e-5f),
          "rest model matrix of the head");
    CHECK(s.subtree(s.findJoint("thigh_l")).size() == 3, "subtree of thigh_l: %zu joints",
          s.subtree(s.findJoint("thigh_l")).size());

    const std::vector<std::uint8_t> file = writeSkeleton(s);
    Result<std::shared_ptr<const Skeleton>> read = readSkeleton(file);
    REQUIRE(read, "read: %s", read ? "" : read.error().c_str());
    CHECK((*read)->signature() == s.signature(), "signature after a round trip");
    CHECK(writeSkeleton(**read) == file, "the same bytes written again");
    for (int j = 0; j < s.jointCount(); ++j)
        CHECK((*read)->jointName(j) == s.jointName(j) && (*read)->parent(j) == s.parent(j), "joint %d", j);

    std::vector<std::uint8_t> corrupt = file;
    corrupt[corrupt.size() / 2] ^= 0x40;
    CHECK(!readSkeleton(corrupt), "a changed byte fails the checksum");
    CHECK(!readSkeleton(std::span(file).first(file.size() - 3)), "a truncated file");
    CHECK(!readSkeleton(std::span(file).first(10)), "a file shorter than the header");
    std::vector<std::uint8_t> magic = file;
    magic[0] = 'X';
    CHECK(!readSkeleton(magic), "a wrong magic");

    SkeletonDesc twice = desc;
    twice.joints[3].name = "spine";
    CHECK(!buildSkeleton(twice), "two joints with one name");
    SkeletonDesc order = desc;
    order.joints[1].parent = 2;
    CHECK(!buildSkeleton(order), "a parent after its child");
    CHECK(!buildSkeleton(SkeletonDesc{}), "no joints");
}

QUENCH_TEST(clip_set_round_trip_is_deterministic) {
    const SkeletonDesc desc = figure();
    const Built a = build(desc, testClips()), b = build(desc, testClips());
    REQUIRE(!a.clipsFile.empty(), "clips built");
    CHECK(a.clipsFile == b.clipsFile && a.skeletonFile == b.skeletonFile, "two builds write the same bytes");
    Result<std::shared_ptr<const ClipSet>> set = readClipSet(a.clipsFile);
    REQUIRE(set, "read: %s", set ? "" : set.error().c_str());
    CHECK((*set)->clips().size() == testClips().size(), "%zu clips", (*set)->clips().size());
    CHECK((*set)->skeletonSignature() == a.skeleton->signature(), "made for the skeleton");
    const Clip* walk = (*set)->find("walk");
    REQUIRE(walk, "walk");
    CHECK(near(walk->duration(), 1.0f, 1e-6f) && walk->loops() && !walk->additive(), "walk's flags");
    REQUIRE(walk->events().size() == 2, "events");
    CHECK(walk->events()[0].name == "foot_l" && walk->events()[1].name == "foot_r", "events sorted by time");
    CHECK((*set)->find("move")->hasRootMotion() && (*set)->find("nod")->additive(), "root motion and additive flags");
    CHECK(!(*set)->find("jump")->loops(), "jump does not loop");
    CHECK(writeClipSet(**set) == a.clipsFile, "the same bytes written again");

    std::vector<std::uint8_t> corrupt = a.clipsFile;
    corrupt[corrupt.size() - 5] ^= 1;
    CHECK(!readClipSet(corrupt), "a changed byte");
    CHECK(!readClipSet(std::span(a.clipsFile).first(a.clipsFile.size() / 2)), "half a file");
    CHECK(!readClipSet(a.skeletonFile), "a skeleton is not a clip set");

    std::vector<ClipDesc> dup = testClips();
    dup[1].name = "keys";
    CHECK(!buildClipSet(*a.skeleton, desc, dup, exact()), "two clips with one name");
    std::vector<ClipDesc> zero = {clip("zero", 0.0f)};
    CHECK(!buildClipSet(*a.skeleton, desc, zero, exact()), "a clip without duration");
}

QUENCH_TEST(graph_parse_errors) {
    CHECK(parseGraph(oneState("a")), "one state");
    CHECK(!parseGraph("{"), "broken JSON");
    CHECK(!parseGraph(R"({"layers": []})"), "no layers");
    CHECK(!parseGraph(R"({"layers": [{"blend": "additive", "states": [{"name": "s"}]}]})"), "additive first layer");
    const char* noCondition = R"({"layers": [{"states": [{"name": "a"}, {"name": "b"}],
        "transitions": [{"from": "a", "to": "b"}]}]})";
    CHECK(!parseGraph(noCondition), "a transition without trigger, exit time or condition");
    const char* unknownState = R"({"layers": [{"states": [{"name": "a"}], "transitions": [{"from": "a", "to": "x",
        "exitTime": 1}]}]})";
    CHECK(!parseGraph(unknownState), "a transition to an unknown state");
    const char* unknownParameter = R"({"layers": [{"states": [{"name": "a", "blend": {"parameter": "speed",
        "clips": [{"clip": "a", "at": 0}]}}]}]})";
    CHECK(!parseGraph(unknownParameter), "a blend over an unknown parameter");
    const char* boolCompare = R"({"parameters": {"on": false}, "layers": [{"states": [{"name": "a"}, {"name": "b"}],
        "transitions": [{"from": "a", "to": "b", "conditions": [{"parameter": "on", "op": ">", "value": true}]}]}]})";
    CHECK(!parseGraph(boolCompare), "bools compare only for equality");
    const char* good = R"({"parameters": {"speed": 0, "on": true, "jump": "trigger"}, "layers": [
        {"name": "base", "states": [{"name": "move", "blend": {"parameter": "speed",
            "clips": [{"clip": "run", "at": 4}, {"clip": "idle", "at": 0}]}}, {"name": "jump", "clip": "jump"}],
         "transitions": [{"from": "*", "to": "jump", "trigger": "jump", "duration": 0.1},
                         {"from": "jump", "to": "move", "exitTime": 1}]},
        {"name": "upper", "mask": ["spine"], "weightParameter": "speed", "states": [{"name": "wave", "clip": "w"}]}]})";
    Result<GraphDef> g = parseGraph(good);
    REQUIRE(g, "parse: %s", g ? "" : g.error().c_str());
    CHECK(g->layers.size() == 2 && g->layers[0].states[0].blend[0].clip == "idle", "blend points sorted");
    CHECK(g->layers[0].transitions[0].from == -1 && g->layers[0].transitions[1].exitTime == 1.0f, "transitions");
    CHECK(g->clipNames() == (std::vector<std::string>{"idle", "jump", "run", "w"}), "clip names");
}

// ---- sampling and blending

QUENCH_TEST(sampling_at_and_between_keys) {
    Rig rig("anim_sampling");
    rig.writeData();
    const CharacterId id = rig.add(oneState("keys", R"(, "loop": false)"));
    rig.load();
    const float phases[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const float expected[] = {0.0f, 1.0f, 2.0f, 1.5f, 1.0f};
    for (int i = 0; i < 5; ++i) {
        rig.anim.setPhase(id, phases[i]);
        rig.step(0.0f);
        REQUIRE(rig.anim.state(id).bound, "bound");
        const glm::vec3 p = rig.joint(id, "pelvis");
        CHECK(near(p.x, expected[i]) && near(p.y, 1.0f), "at %.2f: pelvis x %.4f, expected %.4f", phases[i], p.x,
              expected[i]);
        CHECK(near(rig.joint(id, "head").x, expected[i]), "children follow");
    }
}

QUENCH_TEST(blend_tree_weights_and_phase_sync) {
    Rig rig("anim_blend");
    rig.writeData();
    const CharacterId id = rig.add(R"({"parameters": {"speed": 0}, "layers": [{"states": [{"name": "move",
        "blend": {"parameter": "speed", "clips": [{"clip": "a", "at": 0}, {"clip": "b", "at": 2}]}}]}]})");
    rig.load();
    const float speeds[] = {-1.0f, 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 5.0f};
    const float expected[] = {0.0f, 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 2.0f};
    for (int i = 0; i < 7; ++i) {
        rig.anim.setFloat(id, "speed", speeds[i]);
        rig.step(0.0f);
        CHECK(near(rig.joint(id, "pelvis").x, expected[i]), "speed %.1f: pelvis x %.4f, expected %.4f", speeds[i],
              rig.joint(id, "pelvis").x, expected[i]);
    }
    // Phase sync: a (1 s) and b (2 s) at equal weights play as one clip of 1.5 s.
    rig.anim.setFloat(id, "speed", 1.0f);
    rig.anim.setPhase(id, 0.0f);
    rig.step(0.0f);
    rig.step(0.75f);
    CHECK(near(rig.anim.state(id).normalizedTime, 0.5f, 1e-5f), "normalized time %.5f after 0.75 s",
          rig.anim.state(id).normalizedTime);
    CHECK(rig.anim.stats().clipsSampled == 2, "two clips sampled: %u", rig.anim.stats().clipsSampled);
}

QUENCH_TEST(masked_override_and_additive_layers) {
    Rig rig("anim_layers");
    rig.writeData();
    const CharacterId id = rig.add(R"({"parameters": {"upper": 1, "nod": 0}, "layers": [
        {"name": "base", "states": [{"name": "idle", "clip": "a"}]},
        {"name": "upper", "mask": ["spine"], "weightParameter": "upper", "states": [{"name": "s", "clip": "upper"}]},
        {"name": "nod", "blend": "additive", "weightParameter": "nod", "states": [{"name": "s", "clip": "nod"}]}]})");
    rig.load();
    rig.step(0.0f);
    CHECK(near(rig.joint(id, "pelvis").x, 0.0f), "the mask keeps the pelvis: x %.4f", rig.joint(id, "pelvis").x);
    CHECK(near(rig.joint(id, "spine").x, 0.5f), "the masked layer moves the spine: x %.4f", rig.joint(id, "spine").x);
    CHECK(near(rig.joint(id, "head").x, 0.5f), "and the joints below it");
    rig.anim.setFloat(id, "upper", 0.5f);
    rig.step(0.0f);
    CHECK(near(rig.joint(id, "spine").x, 0.25f), "half the layer: spine x %.4f", rig.joint(id, "spine").x);

    rig.anim.setFloat(id, "upper", 0.0f);
    rig.anim.setFloat(id, "nod", 1.0f);
    rig.step(0.5f); // the nod layer's clip at its middle: +0.1 m
    CHECK(near(rig.joint(id, "head").z, 0.1f), "additive layer: head z %.4f", rig.joint(id, "head").z);
    CHECK(near(rig.joint(id, "head").y, 1.6f), "additive layer keeps the base: head y %.4f", rig.joint(id, "head").y);
}

// ---- state machines

QUENCH_TEST(state_machine_conditions_and_cross_fade) {
    Rig rig("anim_transitions");
    rig.writeData();
    const CharacterId id = rig.add(R"({"parameters": {"speed": 0}, "layers": [{"states": [
        {"name": "idle", "clip": "a"}, {"name": "run", "clip": "b"}], "start": "idle", "transitions": [
        {"from": "idle", "to": "run", "duration": 0.5, "conditions": [{"parameter": "speed", "op": ">", "value": 1}]},
        {"from": "run", "to": "idle", "duration": 0, "conditions": [{"parameter": "speed", "op": "<=", "value": 0.5}]}
        ]}]})");
    rig.load();
    rig.step(0.1f);
    CHECK(rig.anim.state(id).layerState == "idle", "starts idle");
    rig.anim.setFloat(id, "speed", 2.0f);
    rig.step(0.1f); // the condition holds at the end of this update: the cross-fade starts
    CHECK(rig.anim.state(id).nextState == "run", "fading to run");
    CHECK(near(rig.joint(id, "pelvis").x, 0.0f), "still idle's pose");
    rig.step(0.25f); // half way: smoothstep(0.5) = 0.5
    CHECK(near(rig.anim.state(id).transition, 0.5f, 1e-4f), "transition %.4f", rig.anim.state(id).transition);
    CHECK(near(rig.joint(id, "pelvis").x, 1.0f), "half way pelvis x %.4f", rig.joint(id, "pelvis").x);
    rig.step(0.25f);
    CHECK(rig.anim.state(id).layerState == "run" && rig.anim.state(id).nextState.empty(), "in run");
    CHECK(near(rig.joint(id, "pelvis").x, 2.0f), "run's pose: x %.4f", rig.joint(id, "pelvis").x);
    rig.anim.setFloat(id, "speed", 0.0f);
    rig.step(0.1f); // a transition of 0 s switches at the end of the update
    CHECK(rig.anim.state(id).layerState == "idle", "back to idle at once");
    rig.step(0.0f);
    CHECK(near(rig.joint(id, "pelvis").x, 0.0f), "idle's pose: x %.4f", rig.joint(id, "pelvis").x);
}

QUENCH_TEST(state_machine_trigger_and_exit_time) {
    Rig rig("anim_trigger");
    rig.writeData();
    const CharacterId id = rig.add(R"({"parameters": {"jump": "trigger"}, "layers": [{"states": [
        {"name": "idle", "clip": "a"}, {"name": "jump", "clip": "jump"}], "transitions": [
        {"from": "*", "to": "jump", "trigger": "jump", "duration": 0},
        {"from": "jump", "to": "idle", "exitTime": 1, "duration": 0}]}]})");
    rig.load();
    rig.step(0.1f);
    rig.anim.trigger(id, "jump");
    CHECK(rig.anim.parameter(id, "jump") == 1.0f, "the trigger is set");
    rig.step(0.1f);
    CHECK(rig.anim.state(id).layerState == "jump", "jumped");
    CHECK(rig.anim.parameter(id, "jump") == 0.0f, "the transition consumed the trigger");
    rig.step(0.25f); // the jump at its top
    CHECK(near(rig.joint(id, "pelvis").y, 1.5f), "top of the jump: pelvis y %.4f", rig.joint(id, "pelvis").y);
    rig.step(0.2f);
    CHECK(rig.anim.state(id).layerState == "jump", "not yet at the exit time");
    rig.step(0.1f);
    CHECK(rig.anim.state(id).layerState == "idle", "back at the exit time");
    rig.anim.trigger(id, "jump");
    rig.anim.resetTrigger(id, "jump");
    rig.step(0.1f);
    CHECK(rig.anim.state(id).layerState == "idle", "a reset trigger does nothing");
    CHECK(rig.anim.setState(id, "layer0", "jump") && rig.anim.state(id).layerState == "jump", "setState");
    CHECK(!rig.anim.setState(id, "layer0", "fly"), "setState of an unknown state");
}

// ---- events and root motion

QUENCH_TEST(events_in_order_and_looping) {
    Rig rig("anim_events");
    rig.writeData();
    const CharacterId id = rig.add(oneState("walk"));
    rig.load();
    std::vector<std::string> names;
    for (int i = 0; i < 20; ++i) {
        rig.step(0.1f);
        for (const Event& e : rig.anim.events()) {
            CHECK(e.character == id && e.clip == "walk" && e.count == 1, "event of the walk");
            CHECK(near(e.weight, 1.0f, 1e-6f), "full weight");
            names.emplace_back(e.name);
        }
    }
    // 2 s of a 1 s loop: foot_l at 0.25 and 1.25, foot_r at 0.75 and 1.75.
    CHECK((names == std::vector<std::string>{"foot_l", "foot_r", "foot_l", "foot_r"}), "%zu events", names.size());
}

QUENCH_TEST(events_with_a_large_dt) {
    Rig rig("anim_events_dt");
    rig.writeData();
    const CharacterId walker = rig.add(oneState("walk"));
    const CharacterId jumper = rig.add(oneState("jump"));
    rig.load();
    rig.step(3.6f); // 3.6 loops: foot_l at 0.25, 1.25, 2.25, 3.25; foot_r at 0.75, 1.75, 2.75
    std::uint32_t left = 0, right = 0, land = 0;
    for (const Event& e : rig.anim.events()) {
        if (e.character == walker && e.name == "foot_l") left += e.count;
        if (e.character == walker && e.name == "foot_r") right += e.count;
        if (e.character == jumper && e.name == "land") land += e.count;
        CHECK(e.name != "foot_l" || e.value == 1.0f, "the event's value");
    }
    CHECK(left == 4 && right == 3, "foot_l %u, foot_r %u", left, right);
    CHECK(land == 1, "a clip that does not loop passes its end once: %u", land);
    rig.step(1.0f);
    for (const Event& e : rig.anim.events()) CHECK(e.character != jumper, "no event after the end");
}

QUENCH_TEST(root_motion_extracted_and_looped) {
    Rig rig("anim_root_motion");
    rig.writeData();
    const CharacterId id = rig.add(oneState("move"));
    rig.load();
    rig.step(0.5f);
    CHECK(near(rig.anim.rootMotion(id).translation, {0.0f, 0.0f, 0.5f}, 1e-4f), "half a loop: z %.5f",
          rig.anim.rootMotion(id).translation.z);
    CHECK(near(rig.joint(id, "root"), glm::vec3(0.0f), 1e-4f), "the root stays in place (the motion is extracted)");
    rig.step(0.3f);
    rig.step(0.4f); // 0.8 -> 1.2 over the loop's end
    CHECK(near(rig.anim.rootMotion(id).translation, {0.0f, 0.0f, 0.4f}, 1e-4f), "over the loop: z %.5f",
          rig.anim.rootMotion(id).translation.z);
    rig.step(2.5f);
    CHECK(near(rig.anim.rootMotion(id).translation.z, 2.5f, 1e-3f), "several loops: z %.5f",
          rig.anim.rootMotion(id).translation.z);
    CHECK(std::abs(rig.anim.rootMotion(id).rotation.w) > 0.99999f, "no rotation");
}

// ---- IK

QUENCH_TEST(two_bone_ik_reach_and_unreachable) {
    // A straight arm along +X: shoulder at 0, elbow at 1, hand at 2; it bends around Z.
    const glm::mat4 shoulder(1.0f);
    const glm::mat4 elbow = glm::translate(glm::mat4(1.0f), {1, 0, 0});
    const glm::mat4 hand = glm::translate(glm::mat4(1.0f), {2, 0, 0});
    const auto solveAndApply = [&](const TwoBoneIk& ik, glm::vec3& handOut) {
        const TwoBoneResult r = solveTwoBone(shoulder, elbow, hand, ik);
        // Local transforms of the chain: the corrections multiply the local rotations.
        const glm::mat4 s = shoulder * glm::mat4_cast(r.start);
        const glm::mat4 e = s * glm::translate(glm::mat4(1.0f), {1, 0, 0}) * glm::mat4_cast(r.mid);
        handOut = glm::vec3(e * glm::vec4(1, 0, 0, 1));
        return r.reached;
    };
    TwoBoneIk ik;
    ik.midAxis = {0, 0, 1};
    ik.pole = {0, 1, 0};
    ik.target = {1.0f, 1.0f, 0.0f};
    // ozz-animation's IK uses estimated reciprocals (on every platform): about 0.1% of the chain's length.
    glm::vec3 reachedHand;
    CHECK(solveAndApply(ik, reachedHand), "a target in reach is reached");
    CHECK(near(reachedHand, ik.target, 3e-3f), "hand at (%.4f %.4f %.4f)", reachedHand.x, reachedHand.y, reachedHand.z);

    ik.target = {0.0f, 3.0f, 0.0f};
    glm::vec3 stretched;
    CHECK(!solveAndApply(ik, stretched), "a target 3 m away is out of reach");
    CHECK(near(stretched, {0.0f, 2.0f, 0.0f}, 1e-3f), "the arm points at it, stretched: (%.4f %.4f %.4f)", stretched.x,
          stretched.y, stretched.z);

    ik.target = {1.0f, 1.0f, 0.0f};
    ik.weight = 0.0f;
    glm::vec3 unchanged;
    solveAndApply(ik, unchanged);
    CHECK(near(unchanged, {2, 0, 0}, 1e-5f), "weight 0 changes nothing");
}

QUENCH_TEST(aim_ik_turns_the_joint) {
    AimIk ik;
    ik.target = {5.0f, 0.0f, 0.0f};
    const AimResult r = solveAim(glm::mat4(1.0f), ik);
    CHECK(r.reached, "reached");
    const glm::vec3 forward = r.correction * glm::vec3(0, 0, 1);
    CHECK(near(forward, {1, 0, 0}, 1e-4f), "forward (%.4f %.4f %.4f)", forward.x, forward.y, forward.z);
    const glm::vec3 up = r.correction * glm::vec3(0, 1, 0);
    CHECK(near(up, {0, 1, 0}, 1e-3f), "up stays toward the pole: (%.4f %.4f %.4f)", up.x, up.y, up.z);
}

namespace {

FootPlacementDesc feet() {
    FootPlacementDesc f;
    f.legs = {{"thigh_l", "knee_l", "ankle_l"}, {"thigh_r", "knee_r", "ankle_r"}};
    f.pelvis = "pelvis";
    return f;
}

} // namespace

QUENCH_TEST(foot_placement_on_a_step) {
    Rig rig("anim_feet_step");
    rig.writeData();
    CharacterDesc desc;
    desc.feet = feet();
    const CharacterId id = rig.add(oneState("a"), std::move(desc));
    // A step 0.2 m high under x > 0 (the left foot).
    std::atomic<int> queries{0};
    rig.anim.setGroundQuery([&](const glm::vec3& from, float length) -> std::optional<GroundHit> {
        ++queries;
        const float h = from.x > 0.0f ? 0.2f : 0.0f;
        if (from.y - length > h || from.y < h) return std::nullopt;
        return GroundHit{{from.x, h, from.z}, {0, 1, 0}};
    });
    rig.load();
    rig.step(0.1f);
    const CharacterState s = rig.anim.state(id);
    REQUIRE(s.legs.size() == 2, "two legs");
    CHECK(s.legs[0].grounded && s.legs[1].grounded && s.legs[0].reached && s.legs[1].reached, "grounded, reached");
    CHECK(near(s.legs[0].offset, 0.2f, 1e-5f) && near(s.legs[1].offset, 0.0f, 1e-5f), "offsets %.4f %.4f",
          s.legs[0].offset, s.legs[1].offset);
    CHECK(near(s.pelvisOffset, 0.0f, 1e-5f), "the pelvis stays (the lower foot is at its height)");
    CHECK(near(rig.joint(id, "ankle_l"), {0.1f, 0.25f, 0.0f}, 1e-3f), "left ankle on the step: y %.4f",
          rig.joint(id, "ankle_l").y);
    CHECK(near(rig.joint(id, "ankle_r"), {-0.1f, 0.05f, 0.0f}, 1e-3f), "right ankle unchanged: y %.4f",
          rig.joint(id, "ankle_r").y);
    CHECK(rig.joint(id, "knee_l").z > 0.05f, "the left knee bends forward: z %.4f", rig.joint(id, "knee_l").z);
    CHECK(queries == 2 && rig.anim.stats().groundQueries == 2, "one ground query per leg");

    rig.anim.setFootPlacement(id, false);
    rig.step(0.1f);
    CHECK(near(rig.joint(id, "ankle_l").y, 0.05f, 1e-3f), "off: the animated pose: y %.4f", rig.joint(id, "ankle_l").y);
}

QUENCH_TEST(foot_placement_lowers_the_pelvis_and_tilts_feet) {
    Rig rig("anim_feet_slope");
    rig.writeData();
    CharacterDesc desc;
    desc.feet = feet();
    const CharacterId id = rig.add(oneState("a"), std::move(desc));
    const glm::vec3 normal = glm::normalize(glm::vec3(0.3f, 1.0f, 0.0f));
    rig.anim.setGroundQuery([&](const glm::vec3& from, float) -> std::optional<GroundHit> {
        return GroundHit{{from.x, from.x > 0.0f ? -0.2f : 0.0f, from.z}, normal};
    });
    rig.load();
    rig.step(0.1f);
    const CharacterState s = rig.anim.state(id);
    CHECK(near(s.pelvisOffset, -0.2f, 1e-5f), "the pelvis goes down to the lower foot: %.4f", s.pelvisOffset);
    CHECK(near(rig.joint(id, "pelvis").y, 0.8f, 1e-4f), "pelvis y %.4f", rig.joint(id, "pelvis").y);
    CHECK(near(rig.joint(id, "ankle_l").y, -0.15f, 2e-3f), "left ankle y %.4f", rig.joint(id, "ankle_l").y);
    CHECK(near(rig.joint(id, "ankle_r").y, 0.05f, 2e-3f), "right ankle y %.4f", rig.joint(id, "ankle_r").y);
    const int ankle = rig.anim.findJoint(id, "ankle_r");
    const glm::vec3 up = glm::normalize(glm::vec3(rig.anim.modelMatrices(id)[std::size_t(ankle)][1]));
    CHECK(glm::dot(up, normal) > 0.9999f, "the foot is turned to the slope: dot %.5f", glm::dot(up, normal));

    // Smoothing: a new ground height is followed over time, not at once.
    rig.anim.setGroundQuery([](const glm::vec3& from, float) -> std::optional<GroundHit> {
        return GroundHit{{from.x, 0.0f, from.z}, {0, 1, 0}};
    });
    rig.step(1.0f / 60.0f);
    const float pelvis = rig.anim.state(id).pelvisOffset;
    CHECK(pelvis < -0.05f && pelvis > -0.2f, "smoothed: %.4f", pelvis);
    for (int i = 0; i < 120; ++i) rig.step(1.0f / 60.0f);
    CHECK(near(rig.anim.state(id).pelvisOffset, 0.0f, 1e-4f), "settled: %.5f", rig.anim.state(id).pelvisOffset);
}

QUENCH_TEST(look_at_turns_the_head) {
    Rig rig("anim_look_at");
    rig.writeData();
    CharacterDesc desc;
    LookAtDesc look;
    look.joints = {"head", "spine"};
    look.offset = {0.0f, 0.1f, 0.1f};
    desc.lookAt = look;
    const CharacterId id = rig.add(oneState("a"), std::move(desc));
    rig.load();
    const glm::vec3 target(2.0f, 1.7f, 2.0f);
    rig.anim.setLookAt(id, target);
    for (int i = 0; i < 120; ++i) rig.step(1.0f / 60.0f); // the weight follows at 6/s: 1 - e^-12 after 2 s
    const int head = rig.anim.findJoint(id, "head");
    const glm::mat4 m = rig.anim.modelMatrices(id)[std::size_t(head)];
    const glm::vec3 eyes = glm::vec3(m * glm::vec4(look.offset, 1.0f));
    const glm::vec3 forward = glm::normalize(glm::vec3(m * glm::vec4(0, 0, 1, 0)));
    CHECK(glm::dot(forward, glm::normalize(target - eyes)) > 0.999f, "the head faces the target: %.5f",
          glm::dot(forward, glm::normalize(target - eyes)));
    CHECK(rig.anim.state(id).lookAtReached, "reached");
    const glm::vec3 spineForward = glm::normalize(
        glm::vec3(rig.anim.modelMatrices(id)[std::size_t(rig.anim.findJoint(id, "spine"))] * glm::vec4(0, 0, 1, 0)));
    CHECK(spineForward.x > 0.05f, "the spine turns a part of the way: x %.4f", spineForward.x);

    rig.anim.setLookAt(id, glm::vec3(0.0f, 1.7f, -3.0f)); // behind: beyond maxAngle, not followed
    for (int i = 0; i < 120; ++i) rig.step(1.0f / 60.0f);
    const glm::vec3 back =
        glm::normalize(glm::vec3(rig.anim.modelMatrices(id)[std::size_t(head)] * glm::vec4(0, 0, 1, 0)));
    CHECK(back.z > 0.999f, "a target behind: the head looks ahead again (z %.5f)", back.z);
}

// ---- failures and fallbacks

QUENCH_TEST(missing_and_corrupt_assets_hold_the_rest_pose) {
    WarningCounter warnings;
    Rig rig("anim_missing");
    rig.writeData();
    CharacterDesc missing;
    missing.clips.push_back(rig.anim.loadClips("nothing.qanim"));
    const CharacterId a = rig.add(oneState("b"), std::move(missing));
    rig.dir.writeText("broken.qanim", "QANM but not really");
    CharacterDesc broken;
    broken.clips.push_back(rig.anim.loadClips("broken.qanim"));
    const CharacterId b = rig.add(oneState("b"), std::move(broken));
    CharacterDesc noSkeleton;
    noSkeleton.skeleton = rig.anim.loadSkeleton("nothing.qskel");
    const CharacterId c = rig.add(oneState("b"), std::move(noSkeleton));
    rig.load();
    for (int i = 0; i < 3; ++i) rig.step(0.1f);
    for (const CharacterId id : {a, b}) {
        CHECK(!rig.anim.state(id).bound, "not bound");
        CHECK(near(rig.joint(id, "pelvis"), {0, 1, 0}, 1e-6f), "the rest pose");
    }
    CHECK(rig.anim.modelMatrices(c).empty() && rig.anim.findJoint(c, "pelvis") == -1, "no skeleton, no matrices");
    const std::string names[] = {"head"};
    const glm::mat4 ibm[] = {glm::mat4(1.0f)};
    const int skin = rig.anim.addSkin(c, names, ibm);
    rig.step(0.1f);
    CHECK(rig.anim.skinPalette(c, skin)[0] == glm::mat4(1.0f), "identity palette without a skeleton");
    CHECK(warnings.count == 3, "one warning per character: %d", warnings.count.load());
}

QUENCH_TEST(clips_for_another_skeleton_and_unknown_names) {
    WarningCounter warnings;
    Rig rig("anim_mismatch");
    rig.writeData();
    SkeletonDesc other = figure();
    other.joints[3].name = "skull";
    const Built otherData = build(other, {clip("b", 1.0f)});
    rig.dir.write("other.qanim", otherData.clipsFile);
    CharacterDesc desc;
    desc.clips.push_back(rig.anim.loadClips("other.qanim"));
    desc.clips.push_back(rig.anim.loadClips("figure.qanim"));
    const CharacterId id = rig.add(R"({"parameters": {"speed": 0}, "layers": [{"states": [{"name": "s",
        "clip": "b"}, {"name": "t", "clip": "missing"}]}]})",
                                   std::move(desc));
    rig.anim.setFloat(id, "speed", 3.0f); // before binding: kept
    rig.anim.setFloat(id, "spead", 1.0f); // unknown: a warning at binding
    rig.load();
    rig.step(0.0f);
    CHECK(rig.anim.state(id).bound, "bound");
    CHECK(near(rig.joint(id, "pelvis").x, 2.0f), "the other skeleton's clips are skipped; b from the second set");
    CHECK(rig.anim.parameter(id, "speed") == 3.0f, "a parameter set before binding");
    const int before = warnings.count;
    CHECK(before == 3, "warnings: other skeleton, missing clip, unknown parameter: %d", before);
    for (int i = 0; i < 10; ++i) rig.anim.setFloat(id, "spead", 1.0f); // warned about at binding
    for (int i = 0; i < 10; ++i) rig.anim.trigger(id, "speed");        // not a trigger
    CHECK(warnings.count == before + 1, "a wrong name warns once: %d more", warnings.count.load() - before);
    CHECK(rig.anim.setState(id, "layer0", "t"), "the state with a missing clip");
    rig.step(0.1f);
    CHECK(near(rig.joint(id, "pelvis"), {0, 1, 0}, 1e-6f), "holds the rest pose");
}

QUENCH_TEST(reload_rebinds_and_keeps_parameters) {
    Rig rig("anim_reload");
    rig.writeData();
    const CharacterId id = rig.add(R"({"parameters": {"speed": 0}, "layers": [{"states": [{"name": "s",
        "blend": {"parameter": "speed", "clips": [{"clip": "a", "at": 0}, {"clip": "b", "at": 1}]}}]}]})");
    rig.load();
    rig.anim.setFloat(id, "speed", 1.0f);
    rig.step(0.0f);
    CHECK(near(rig.joint(id, "pelvis").x, 2.0f), "b before the reload");
    std::vector<ClipDesc> clips = testClips();
    clips[2].tracks[kPelvis].translations = {{0.0f, {-1, 1, 0}}};
    rig.writeData(clips);
    const assets::AssetRef<ClipSetAsset> ref = rig.anim.loadClips("figure.qanim");
    rig.assets.reload(ref->id());
    rig.load();
    rig.step(0.0f);
    CHECK(near(rig.joint(id, "pelvis").x, -1.0f), "the reloaded b: x %.4f", rig.joint(id, "pelvis").x);
    CHECK(rig.anim.parameter(id, "speed") == 1.0f, "the parameter survives the rebind");
}

// ---- many characters, skins

namespace {

std::vector<glm::mat4> crowd(const char* dir, int workers, int count) {
    Rig rig(dir, workers);
    rig.writeData();
    const char* graph = R"({"parameters": {"speed": 0}, "layers": [{"states": [{"name": "move",
        "blend": {"parameter": "speed", "clips": [{"clip": "a", "at": 0}, {"clip": "walk", "at": 1},
        {"clip": "keys", "at": 2}]}}]}]})";
    std::vector<CharacterId> ids;
    for (int i = 0; i < count; ++i) {
        CharacterDesc desc;
        desc.feet = feet();
        ids.push_back(rig.add(graph, std::move(desc)));
        rig.anim.setFloat(ids.back(), "speed", float(i % 5) * 0.5f);
        rig.anim.setPhase(ids.back(), float(i) / float(count));
    }
    rig.anim.setGroundQuery([](const glm::vec3& from, float) -> std::optional<GroundHit> {
        return GroundHit{{from.x, std::sin(from.x * 3.0f) * 0.1f, from.z}, {0, 1, 0}};
    });
    rig.load();
    std::vector<glm::mat4> out;
    for (int frame = 0; frame < 10; ++frame) rig.step(1.0f / 60.0f);
    CHECK(rig.anim.stats().bound == std::uint32_t(count), "%u of %d characters bound", rig.anim.stats().bound, count);
    for (const CharacterId id : ids) {
        const std::span<const glm::mat4> m = rig.anim.modelMatrices(id);
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

} // namespace

QUENCH_TEST(parallel_update_matches_one_thread) {
    {
        JobSystem probe(JobSystemDesc{.workerThreads = 3});
        if (probe.workerCount() == 0) QUENCH_SKIP("no worker threads in this build");
    }
    const std::vector<glm::mat4> one = crowd("anim_crowd_one", 0, 48), many = crowd("anim_crowd_many", 3, 48);
    REQUIRE(one.size() == 48u * 10u && one.size() == many.size(), "%zu and %zu matrices", one.size(), many.size());
    const auto* a = reinterpret_cast<const std::uint32_t*>(one.data());
    const auto* b = reinterpret_cast<const std::uint32_t*>(many.data());
    const std::size_t floats = one.size() * 16;
    const std::size_t first = std::size_t(std::mismatch(a, a + floats, b).first - a);
    CHECK(first == floats, "the same bits on 1 and 4 threads (first difference: matrix %zu, element %zu: %.9g, %.9g)",
          first / 16, first % 16, first < floats ? double(reinterpret_cast<const float*>(a)[first]) : 0.0,
          first < floats ? double(reinterpret_cast<const float*>(b)[first]) : 0.0);
}

QUENCH_TEST(one_thread_updates_a_crowd) {
    const std::vector<glm::mat4> one = crowd("anim_crowd_single", 0, 16);
    REQUIRE(one.size() == 16u * 10u, "%zu matrices", one.size());
    for (const glm::mat4& m : one)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) REQUIRE(std::isfinite(m[c][r]), "finite matrices");
}

QUENCH_TEST(skin_palettes) {
    Rig rig("anim_skin");
    rig.writeData();
    const CharacterId id = rig.add(oneState("b"));
    const std::string names[] = {"head", "tail"};
    const Built data = build(figure(), testClips());
    const glm::mat4 ibm[] = {glm::inverse(data.skeleton->restModelMatrices()[kHead]), glm::mat4(1.0f)};
    const int skin = rig.anim.addSkin(id, names, ibm);
    CHECK(rig.anim.skinPalette(id, skin).size() == 2, "a matrix per skin joint");
    rig.load();
    rig.step(0.0f);
    const std::span<const glm::mat4> palette = rig.anim.skinPalette(id, skin);
    const glm::vec3 moved = glm::vec3(palette[0] * glm::vec4(0, 1.6f, 0, 1));
    CHECK(near(moved, {2.0f, 1.6f, 0.0f}), "a vertex at the head moves with it: (%.4f %.4f %.4f)", moved.x, moved.y,
          moved.z);
    CHECK(palette[1] == glm::mat4(1.0f), "a joint the skeleton lacks: identity");
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), {10, 0, 0});
    rig.anim.setTransform(id, world);
    CHECK(near(glm::vec3(rig.anim.jointWorld(id, rig.anim.findJoint(id, "head"))[3]), {12.0f, 1.6f, 0.0f}),
          "jointWorld: the transform times the model matrix");
    rig.anim.removeCharacter(id);
    CHECK(!rig.anim.contains(id) && rig.anim.characterCount() == 0, "removed");
}
