#include <quench/animation/AnimationSystem.h>

#include "Character.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace quench::animation {

using Character = AnimationSystem::Character;

namespace {

template <class A>
auto dataOf(const assets::AssetRef<A>& ref) -> std::remove_cvref_t<decltype(ref->data())> {
    if (!ref || ref->state() != assets::AssetState::Ready) return {};
    return ref->data();
}

glm::quat rotationOf(const glm::mat4& m) {
    const glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])),
                      glm::normalize(glm::vec3(m[2])));
    return glm::normalize(glm::quat_cast(r));
}

void resolveSkin(const Skeleton* skeleton, Character::Skin& skin) {
    skin.map.assign(skin.joints.size(), -1);
    if (!skeleton) return;
    for (std::size_t i = 0; i < skin.joints.size(); ++i) skin.map[i] = skeleton->findJoint(skin.joints[i]);
}

} // namespace

AnimationSystem::AnimationSystem(assets::AssetManager& assets, JobSystem& jobs, const AnimationSettings& settings)
    : m_assets(assets), m_jobs(jobs), m_settings(settings) {
    m_assets.registerType<SkeletonAsset>(
        "skeleton", [](std::string_view) { return std::unique_ptr<SkeletonAsset>(new SkeletonAsset()); },
        settings.skeletonBudget);
    m_assets.registerType<ClipSetAsset>(
        "animation", [](std::string_view) { return std::unique_ptr<ClipSetAsset>(new ClipSetAsset()); },
        settings.clipBudget);
    m_assets.registerType<GraphAsset>(
        "animgraph", [](std::string_view) { return std::unique_ptr<GraphAsset>(new GraphAsset()); },
        settings.graphBudget);
}

AnimationSystem::~AnimationSystem() {
    m_active.clear();
    m_slots.clear(); // the characters' asset references go before the types
    m_assets.unregisterType<GraphAsset>();
    m_assets.unregisterType<ClipSetAsset>();
    m_assets.unregisterType<SkeletonAsset>();
}

assets::AssetRef<SkeletonAsset> AnimationSystem::loadSkeleton(std::string_view path) {
    return m_assets.load<SkeletonAsset>(path);
}
assets::AssetRef<ClipSetAsset> AnimationSystem::loadClips(std::string_view path) {
    return m_assets.load<ClipSetAsset>(path);
}
assets::AssetRef<GraphAsset> AnimationSystem::loadGraph(std::string_view path) {
    return m_assets.load<GraphAsset>(path);
}

// ---- characters

Character* AnimationSystem::get(CharacterId id) {
    if (id.index >= m_slots.size()) return nullptr;
    Slot& slot = m_slots[id.index];
    return slot.character && slot.generation == id.generation ? slot.character.get() : nullptr;
}

const Character* AnimationSystem::get(CharacterId id) const { return const_cast<AnimationSystem*>(this)->get(id); }

CharacterId AnimationSystem::addCharacter(CharacterDesc desc) {
    std::uint32_t index;
    if (!m_free.empty()) {
        index = m_free.back();
        m_free.pop_back();
    } else {
        index = std::uint32_t(m_slots.size());
        m_slots.emplace_back();
    }
    Slot& slot = m_slots[index];
    slot.character = std::make_unique<Character>();
    Character& c = *slot.character;
    c.id = {index, slot.generation};
    c.feetEnabled = desc.feet.has_value();
    c.desc = std::move(desc);
    return c.id;
}

void AnimationSystem::removeCharacter(CharacterId id) {
    QUENCH_ASSERT(get(id), "removeCharacter: stale or null character");
    if (!get(id)) return;
    Slot& slot = m_slots[id.index];
    slot.character.reset();
    slot.generation = slot.generation == 0xffffffffu ? 1u : slot.generation + 1u;
    m_free.push_back(id.index);
}

bool AnimationSystem::contains(CharacterId id) const { return get(id) != nullptr; }

std::uint32_t AnimationSystem::characterCount() const { return std::uint32_t(m_slots.size() - m_free.size()); }

#define QUENCH_CHARACTER(id, ...)                                                                                      \
    Character* c = get(id);                                                                                            \
    QUENCH_ASSERT(c, "stale or null character");                                                                       \
    if (!c) return __VA_ARGS__

void AnimationSystem::setTransform(CharacterId id, const glm::mat4& transform) {
    QUENCH_CHARACTER(id);
    c->desc.transform = transform;
}

const glm::mat4& AnimationSystem::transform(CharacterId id) const {
    static const glm::mat4 kIdentity(1.0f);
    const Character* c = get(id);
    QUENCH_ASSERT(c, "stale or null character");
    return c ? c->desc.transform : kIdentity;
}

void AnimationSystem::setGroundQuery(GroundQuery query) { m_ground = std::move(query); }

void AnimationSystem::setFootPlacement(CharacterId id, bool enabled) {
    QUENCH_CHARACTER(id);
    c->feetEnabled = enabled && c->desc.feet.has_value();
    if (!c->feetEnabled) {
        c->pelvisOffset = 0.0f;
        for (Character::Leg& leg : c->legs) leg.offset = 0.0f;
        c->groundSnapped = false;
    }
}

void AnimationSystem::setLookAt(CharacterId id, std::optional<glm::vec3> worldTarget, float weight) {
    QUENCH_CHARACTER(id);
    c->lookTarget = worldTarget;
    c->lookWeight = worldTarget ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
}

void AnimationSystem::setTimeScale(CharacterId id, float scale) {
    QUENCH_CHARACTER(id);
    c->timeScale = std::max(0.0f, scale);
}

void AnimationSystem::setLod(const AnimationLod& lod) { m_settings.lod = lod; }

void AnimationSystem::setLodView(const glm::vec3& eye, float fovY) { m_lodView = LodView{eye, fovY}; }

void AnimationSystem::clearLodView() { m_lodView.reset(); }

void AnimationSystem::setPhase(CharacterId id, float normalizedTime) {
    QUENCH_CHARACTER(id);
    c->pendingPhase = std::max(0.0f, normalizedTime);
}

namespace {

// The parameter's index, or -1 (logged once per name) when the bound graph has no such parameter of that type.
int parameterIndex(Character& c, std::string_view name, bool trigger) {
    const int index = c.graph->findParameter(name);
    const bool isTrigger = index >= 0 && c.graph->parameters[std::size_t(index)].type == ParameterType::Trigger;
    if (index < 0 || isTrigger != trigger) {
        if (std::ranges::find(c.unknownParameters, name) == c.unknownParameters.end()) {
            c.unknownParameters.emplace_back(name);
            log::warning("animation", "character {}: the graph has no {} '{}'; ignored", c.id.index,
                         trigger ? "trigger" : "float or bool parameter", name);
        }
        return -1;
    }
    return index;
}

void setParameter(Character& c, std::string_view name, float value, bool trigger) {
    if (!c.bound) {
        const auto it = std::ranges::find_if(c.pending, [name](const auto& p) { return p.first == name; });
        if (it != c.pending.end())
            it->second = value;
        else
            c.pending.emplace_back(std::string(name), value);
        return;
    }
    if (const int i = parameterIndex(c, name, trigger); i >= 0) c.params[std::size_t(i)] = value;
}

} // namespace

void AnimationSystem::setFloat(CharacterId id, std::string_view parameter, float value) {
    QUENCH_CHARACTER(id);
    setParameter(*c, parameter, value, false);
}

void AnimationSystem::setBool(CharacterId id, std::string_view parameter, bool value) {
    QUENCH_CHARACTER(id);
    setParameter(*c, parameter, value ? 1.0f : 0.0f, false);
}

void AnimationSystem::trigger(CharacterId id, std::string_view parameter) {
    QUENCH_CHARACTER(id);
    setParameter(*c, parameter, 1.0f, true);
}

void AnimationSystem::resetTrigger(CharacterId id, std::string_view parameter) {
    QUENCH_CHARACTER(id);
    setParameter(*c, parameter, 0.0f, true);
}

float AnimationSystem::parameter(CharacterId id, std::string_view name) const {
    const Character* c = get(id);
    QUENCH_ASSERT(c, "stale or null character");
    if (!c) return 0.0f;
    if (!c->bound) {
        const auto it = std::ranges::find_if(c->pending, [name](const auto& p) { return p.first == name; });
        return it != c->pending.end() ? it->second : 0.0f;
    }
    const int index = c->graph->findParameter(name);
    return index >= 0 ? c->params[std::size_t(index)] : 0.0f;
}

bool AnimationSystem::setState(CharacterId id, std::string_view layerName, std::string_view stateName) {
    QUENCH_CHARACTER(id, false);
    if (!c->bound) return false;
    for (Character::LayerRuntime& layer : c->layers) {
        if (layer.def->name != layerName) continue;
        for (std::size_t s = 0; s < layer.states.size(); ++s) {
            if (layer.states[s].def->name != stateName) continue;
            layer.current = int(s);
            layer.time = 0.0f;
            layer.entered = true;
            layer.next = -1;
            return true;
        }
    }
    return false;
}

// ---- binding

void AnimationSystem::bind(Character& c) {
    std::shared_ptr<const Skeleton> skeleton = dataOf(c.desc.skeleton);
    std::shared_ptr<const GraphDef> graph = dataOf(c.desc.graph);
    std::vector<std::shared_ptr<const ClipSet>> clipSets;
    for (const assets::AssetRef<ClipSetAsset>& ref : c.desc.clips) clipSets.push_back(dataOf(ref));

    const auto logFailure = [&] {
        if (c.loggedFailure) return;
        const assets::Asset* failed = nullptr;
        if (c.desc.skeleton.state() == assets::AssetState::Failed) failed = c.desc.skeleton.get();
        if (c.desc.graph.state() == assets::AssetState::Failed) failed = c.desc.graph.get();
        for (const auto& ref : c.desc.clips)
            if (ref.state() == assets::AssetState::Failed) failed = ref.get();
        if (!failed && (!c.desc.skeleton || !c.desc.graph)) {
            c.loggedFailure = true;
            log::warning("animation", "character {}: no {}; the character holds its rest pose", c.id.index,
                         c.desc.skeleton ? "graph" : "skeleton");
        } else if (failed) {
            c.loggedFailure = true;
            log::warning("animation", "character {}: '{}' failed to load; the character holds its rest pose",
                         c.id.index, failed->path());
        }
    };
    if (skeleton == c.skeleton && graph == c.graph && clipSets == c.clipSets) {
        if (!c.bound) logFailure(); // a load that failed after the last change
        return;
    }

    if (skeleton != c.skeleton) {
        c.skeleton = skeleton;
        c.groundSnapped = false;
        c.legs.clear();
        c.pelvis = -1;
        c.lookChain.clear();
        for (Character::Skin& skin : c.skins) {
            resolveSkin(skeleton.get(), skin);
            std::fill(skin.palette.begin(), skin.palette.end(), glm::mat4(1.0f));
        }
        if (skeleton) {
            const auto soa = std::size_t(skeleton->impl().skeleton.num_soa_joints());
            for (auto* pose :
                 {&c.statePose[0], &c.statePose[1], &c.layerPose, &c.pose, &c.scratch, &c.lodFrom, &c.lodTo})
                pose->resize(soa);
            for (auto& pose : c.samples) pose.resize(soa);
            c.weightsA.resize(soa);
            c.weightsB.resize(soa);
            c.models.resize(std::size_t(skeleton->jointCount()));
            c.modelMatrices.assign(skeleton->restModelMatrices().begin(), skeleton->restModelMatrices().end());
            if (c.desc.feet) {
                bool ok = true;
                for (const LegDesc& leg : c.desc.feet->legs) {
                    Character::Leg l;
                    l.hip = skeleton->findJoint(leg.hip);
                    l.knee = skeleton->findJoint(leg.knee);
                    l.ankle = skeleton->findJoint(leg.ankle);
                    l.kneeAxis = leg.kneeAxis;
                    if (l.hip < 0 || l.knee < 0 || l.ankle < 0) ok = false;
                    c.legs.push_back(l);
                }
                c.pelvis = skeleton->findJoint(c.desc.feet->pelvis);
                if (!ok || c.pelvis < 0) {
                    log::warning("animation",
                                 "character {}: the skeleton lacks a foot placement joint; no foot placement",
                                 c.id.index);
                    c.legs.clear();
                }
            }
            if (c.desc.lookAt) {
                const LookAtDesc& look = *c.desc.lookAt;
                for (std::size_t i = 0; i < look.joints.size(); ++i) {
                    const int joint = skeleton->findJoint(look.joints[i]);
                    if (joint < 0) {
                        log::warning("animation", "character {}: no look-at joint '{}'; no look-at", c.id.index,
                                     look.joints[i]);
                        c.lookChain.clear();
                        break;
                    }
                    const glm::quat toJoint =
                        glm::inverse(rotationOf(skeleton->restModelMatrices()[std::size_t(joint)]));
                    c.lookChain.push_back({joint, glm::normalize(toJoint * look.forward),
                                           glm::normalize(toJoint * look.up),
                                           i == 0 ? toJoint * look.offset : glm::vec3(0.0f)});
                }
            }
        } else {
            c.models.clear();
            c.modelMatrices.clear();
        }
    }

    // Parameter values survive a rebind by name.
    if (c.bound && c.graph) {
        for (std::size_t i = 0; i < c.graph->parameters.size() && i < c.params.size(); ++i) {
            const std::string& name = c.graph->parameters[i].name;
            const auto it = std::ranges::find_if(c.pending, [&](const auto& p) { return p.first == name; });
            if (it == c.pending.end()) c.pending.emplace_back(name, c.params[i]);
        }
    }
    c.graph = graph;
    c.clipSets = clipSets;
    c.bound = false;
    c.lodLag = 0.0f; // an interpolation in progress ends; the pose buffers may have changed size
    c.lodBlending = false;
    c.layers.clear();
    c.contexts.clear();
    if (!skeleton || !graph || std::ranges::any_of(clipSets, [](const auto& s) { return !s; })) {
        logFailure();
        return;
    }
    c.loggedFailure = false;

    std::vector<const ClipSet*> sets;
    for (std::size_t i = 0; i < clipSets.size(); ++i) {
        if (clipSets[i]->skeletonSignature() != skeleton->signature() ||
            clipSets[i]->jointCount() != skeleton->jointCount()) {
            log::warning("animation",
                         "character {}: '{}' was made for another skeleton than '{}'; its clips are not used",
                         c.id.index, c.desc.clips[i]->path(), c.desc.skeleton->path());
            continue;
        }
        sets.push_back(clipSets[i].get());
    }
    const auto findClip = [&](const std::string& name) -> const Clip* {
        for (const ClipSet* set : sets)
            if (const Clip* clip = set->find(name)) return clip;
        log::warning("animation", "character {}: no clip '{}' in its clip sets; its state holds the rest pose",
                     c.id.index, name);
        return nullptr;
    };

    c.params.resize(graph->parameters.size());
    for (std::size_t i = 0; i < graph->parameters.size(); ++i) c.params[i] = graph->parameters[i].initial;
    c.bound = true;
    c.lodShown = false; // the pose shown until now (the rest pose, another skeleton's) is not interpolated from
    c.unknownParameters.clear();
    for (const auto& [name, value] : c.pending) {
        const int index = graph->findParameter(name);
        if (index < 0) {
            log::warning("animation", "character {}: the graph has no parameter '{}'; ignored", c.id.index, name);
            c.unknownParameters.push_back(name);
            continue;
        }
        c.params[std::size_t(index)] = value;
    }
    c.pending.clear();

    const int jointCount = skeleton->jointCount();
    const auto soa = std::size_t(skeleton->impl().skeleton.num_soa_joints());
    for (const LayerDef& def : graph->layers) {
        Character::LayerRuntime layer;
        layer.def = &def;
        layer.current = def.start;
        for (const StateDef& state : def.states) {
            Character::StateRuntime s;
            s.def = &state;
            const auto addSlot = [&](const std::string& clipName, float at, float phase) {
                const Clip* clip = findClip(clipName);
                s.clips.push_back({clip, at, phase, int(c.contexts.size())});
                c.contexts.push_back(std::make_unique<ozz::animation::SamplingJob::Context>(jointCount));
                if (clip && (def.blend == LayerBlend::Additive) != clip->additive())
                    log::warning("animation", "character {}: clip '{}' is {}additive in the {} layer '{}'", c.id.index,
                                 clipName, clip->additive() ? "" : "not ",
                                 def.blend == LayerBlend::Additive ? "additive" : "override", def.name);
            };
            if (!state.clip.empty()) addSlot(state.clip, 0.0f, 0.0f);
            for (const BlendPoint& p : state.blend) addSlot(p.clip, p.at, p.phase);
            for (const BlendPoint2D& p : state.blend2d) {
                addSlot(p.clip, 0.0f, p.phase);
                s.points.push_back(p.at);
            }
            if (s.clips.size() > 1 && c.samples.size() < s.clips.size())
                c.samples.resize(s.clips.size(), ozz::vector<ozz::math::SoaTransform>(soa));
            bool loops = !s.clips.empty();
            for (const Character::ClipSlot& slot : s.clips) loops = loops && (!slot.clip || slot.clip->loops());
            s.loop = state.loop.value_or(s.clips.empty() || loops);
            layer.states.push_back(std::move(s));
        }
        if (!def.mask.empty()) {
            std::vector<float> weights(soa * 4, 0.0f);
            for (const std::string& name : def.mask) {
                const int joint = skeleton->findJoint(name);
                if (joint < 0) {
                    log::warning("animation", "character {}: no mask joint '{}' in layer '{}'", c.id.index, name,
                                 def.name);
                    continue;
                }
                for (const int j : skeleton->subtree(joint)) weights[std::size_t(j)] = 1.0f;
            }
            layer.mask.resize(soa);
            for (std::size_t i = 0; i < soa; ++i) layer.mask[i] = ozz::math::simd_float4::LoadPtrU(&weights[i * 4]);
        }
        c.layers.push_back(std::move(layer));
    }
}

int AnimationSystem::addSkin(CharacterId id, std::span<const std::string> joints,
                             std::span<const glm::mat4> inverseBindMatrices) {
    QUENCH_CHARACTER(id, -1);
    QUENCH_ASSERT(joints.size() == inverseBindMatrices.size(), "one inverse bind matrix per skin joint");
    Character::Skin skin;
    skin.joints.assign(joints.begin(), joints.end());
    skin.inverseBind.assign(inverseBindMatrices.begin(), inverseBindMatrices.end());
    skin.inverseBind.resize(skin.joints.size(), glm::mat4(1.0f));
    skin.palette.assign(skin.joints.size(), glm::mat4(1.0f));
    resolveSkin(c->skeleton.get(), skin);
    c->skins.push_back(std::move(skin));
    return int(c->skins.size()) - 1;
}

// ---- update

void AnimationSystem::update(float dt) {
    const auto start = std::chrono::steady_clock::now();
    m_active.clear();
    for (Slot& slot : m_slots) {
        if (!slot.character) continue;
        bind(*slot.character);
        m_active.push_back(slot.character.get());
    }
    ++m_frame;
    const AnimationLod& lod = m_settings.lod;
    const bool lodOn = lod.enabled && m_lodView;
    const float viewScale = lodOn ? 2.0f * std::tan(std::max(m_lodView->fovY, 1e-3f) * 0.5f) : 1.0f;
    for (std::size_t i = 0; i < m_active.size(); ++i) {
        Character& c = *m_active[i];
        // The screen size picks the level; a finer level only past its threshold by the hysteresis.
        int level = 0;
        bool ik = true;
        if (lodOn && c.bound && c.lodShown) {
            const glm::vec3 center(c.desc.transform * glm::vec4(0.0f, c.desc.height * 0.5f, 0.0f, 1.0f));
            const float distance = glm::length(center - m_lodView->eye);
            const float size = distance > 1e-3f ? c.desc.height / (distance * viewScale) : 1e9f;
            const auto levelAt = [&](float scale) { return size < lod.quarter * scale ? 2 : size < lod.half * scale; };
            level = int(levelAt(1.0f));
            if (level < c.lodLevel) level = std::min(c.lodLevel, int(levelAt(1.0f + lod.hysteresis)));
            ik = size >= lod.ik * (c.lodIk ? 1.0f : 1.0f + lod.hysteresis);
        }
        c.lodLevel = level;
        c.lodIk = ik;
        // An evaluation on the character's frame of its rate (staggered by its slot) or when its level changed, once
        // the pose shown has caught up with the last one; with interpolation it is computed for the frame before the
        // next, so the pose shown between them is on time.
        const int rate = 1 << level;
        const std::uint64_t phase = (m_frame + i) % std::uint64_t(rate);
        const int frames = phase == 0 ? rate : rate - int(phase);
        const float want = lod.interpolate ? float(frames - 1) * dt : 0.0f;
        c.lodLag -= dt;
        c.lodEvaluate = (phase == 0 || level != c.lodEvaluatedLevel) && c.lodLag <= want + 1e-6f;
        if (c.lodEvaluate) {
            c.lodDt = want - c.lodLag;
            c.lodSpan = want + dt;
            c.lodLag = want;
            c.lodFrames = frames;
            c.lodEvaluatedLevel = level;
        }
    }
    if (!m_active.empty()) {
        const Job job = m_jobs.parallelFor(std::uint32_t(m_active.size()), std::max(1u, m_settings.charactersPerJob),
                                           [this](std::uint32_t begin, std::uint32_t end) {
                                               for (std::uint32_t i = begin; i < end; ++i)
                                                   detail::update(*m_active[i], m_settings, m_ground);
                                           });
        m_jobs.wait(job);
    }

    m_events.clear();
    m_stats = {};
    m_stats.threads = m_jobs.threadCount();
    for (Character* c : m_active) {
        ++m_stats.characters;
        if (c->bound) ++m_stats.bound;
        if (c->skeleton) m_stats.joints += std::uint32_t(c->skeleton->jointCount());
        m_stats.clipsSampled += c->clipsSampled;
        if (c->lodEvaluate) ++m_stats.evaluated;
        ++m_stats.lodRates[std::size_t(c->lodLevel)];
        m_stats.groundQueries += c->groundQueries;
        for (const Character::EventRecord& e : c->events)
            m_events.push_back({c->id, e.event->name, e.clip->name(), e.event->value, e.weight, e.count, e.layer});
    }
    m_stats.events = std::uint32_t(m_events.size());
    m_stats.updateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ---- outputs

CharacterState AnimationSystem::state(CharacterId id) const {
    const Character* c = get(id);
    QUENCH_ASSERT(c, "stale or null character");
    CharacterState s;
    if (!c) return s;
    s.bound = c->bound;
    if (c->bound && !c->layers.empty()) {
        const Character::LayerRuntime& layer = c->layers[0];
        s.layerState = layer.states[std::size_t(layer.current)].def->name;
        if (layer.next >= 0) {
            s.nextState = layer.states[std::size_t(layer.next)].def->name;
            s.transition = layer.fadeDuration > 0.0f ? std::clamp(layer.fade / layer.fadeDuration, 0.0f, 1.0f) : 1.0f;
        }
        s.normalizedTime = layer.time;
    }
    s.pelvisOffset = c->pelvisOffset;
    s.legs = c->legStates;
    s.lookAtReached = c->lookReached;
    s.lodRate = 1 << c->lodLevel;
    s.lodIk = c->lodIk;
    s.poseChanged = c->poseChanged;
    return s;
}

std::span<const glm::mat4> AnimationSystem::modelMatrices(CharacterId id) const {
    const Character* c = get(id);
    QUENCH_ASSERT(c, "stale or null character");
    return c ? std::span<const glm::mat4>(c->modelMatrices) : std::span<const glm::mat4>();
}

glm::mat4 AnimationSystem::jointWorld(CharacterId id, int joint) const {
    const Character* c = get(id);
    QUENCH_ASSERT(c, "stale or null character");
    if (!c || joint < 0 || std::size_t(joint) >= c->modelMatrices.size()) return glm::mat4(1.0f);
    return c->desc.transform * c->modelMatrices[std::size_t(joint)];
}

int AnimationSystem::findJoint(CharacterId id, std::string_view name) const {
    const Character* c = get(id);
    QUENCH_ASSERT(c, "stale or null character");
    return c && c->skeleton ? c->skeleton->findJoint(name) : -1;
}

RootMotion AnimationSystem::rootMotion(CharacterId id) const {
    const Character* c = get(id);
    QUENCH_ASSERT(c, "stale or null character");
    return c ? c->rootMotion : RootMotion{};
}

std::span<const glm::mat4> AnimationSystem::skinPalette(CharacterId id, int skin) const {
    const Character* c = get(id);
    QUENCH_ASSERT(c && skin >= 0 && std::size_t(skin) < c->skins.size(), "stale character or skin");
    if (!c || skin < 0 || std::size_t(skin) >= c->skins.size()) return {};
    return c->skins[std::size_t(skin)].palette;
}

} // namespace quench::animation
