// One character's update: state machines, sampling, layers, events, root motion, then IK (Ik.cpp) and palettes.

#include "Character.h"

#include <quench/core/Assert.h>

#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/track_sampling_job.h>

#include <algorithm>
#include <cmath>

namespace quench::animation::detail {

namespace {

using Character = AnimationSystem::Character;
using ozz::math::SoaTransform;
using Pose = ozz::vector<SoaTransform>;

// The clips of a state that contribute, at most two (a 1D blend tree is linear between neighbouring points).
struct Active {
    int slot[2] = {0, 0};
    float weight[2] = {0.0f, 0.0f};
    int count = 0;
};

Active activeClips(const Character& c, const Character::StateRuntime& s) {
    Active a;
    if (s.clips.empty()) return a;
    if (s.def->blend.empty() || s.clips.size() == 1) {
        a.count = 1;
        a.weight[0] = 1.0f;
        return a;
    }
    const float p = c.params[std::size_t(s.def->blendParameter)];
    const int last = int(s.clips.size()) - 1;
    if (p <= s.clips[0].at || last == 0) {
        a.count = 1;
        a.weight[0] = 1.0f;
        return a;
    }
    if (p >= s.clips[std::size_t(last)].at) {
        a.count = 1;
        a.slot[0] = last;
        a.weight[0] = 1.0f;
        return a;
    }
    int i = 0;
    while (i + 1 < last && p >= s.clips[std::size_t(i + 1)].at) ++i;
    const float lo = s.clips[std::size_t(i)].at, hi = s.clips[std::size_t(i + 1)].at;
    const float t = hi > lo ? (p - lo) / (hi - lo) : 1.0f;
    if (t < 1e-4f) return {{i, 0}, {1.0f, 0.0f}, 1};
    if (t > 1.0f - 1e-4f) return {{i + 1, 0}, {1.0f, 0.0f}, 1};
    return {{i, i + 1}, {1.0f - t, t}, 2};
}

float clipDuration(const Character::ClipSlot& slot) { return slot.clip ? slot.clip->duration() : 1.0f; }

float advanceTime(const Character& c, const Character::StateRuntime& s, const Active& a, float time, float dt) {
    if (a.count == 0) return time + dt; // an empty state: seconds (exit times count in them)
    float duration = 0.0f;
    for (int k = 0; k < a.count; ++k) duration += a.weight[k] * clipDuration(s.clips[std::size_t(a.slot[k])]);
    float rate = s.def->speed;
    if (s.def->speedParameter >= 0) rate *= std::max(0.0f, c.params[std::size_t(s.def->speedParameter)]);
    return duration > 1e-6f ? time + dt * rate / duration : time;
}

float ratioAt(bool loop, float time) { return loop ? time - std::floor(time) : std::clamp(time, 0.0f, 1.0f); }

float smoothstep(float f) { return f * f * (3.0f - 2.0f * f); }

void copyPose(ozz::span<const SoaTransform> from, Pose& to) { std::copy(from.begin(), from.end(), to.begin()); }

// Normalized blend of two poses (weights need not sum to 1).
void blend2(const Character& c, ozz::span<const SoaTransform> a, float wa, ozz::span<const SoaTransform> b, float wb,
            Pose& out) {
    ozz::animation::BlendingJob::Layer layers[2];
    layers[0].weight = wa;
    layers[0].transform = a;
    layers[1].weight = wb;
    layers[1].transform = b;
    ozz::animation::BlendingJob job;
    job.threshold = 1e-4f;
    job.layers = layers;
    job.rest_pose = c.skeleton->impl().skeleton.joint_rest_poses();
    job.output = ozz::make_span(out);
    [[maybe_unused]] const bool ok = job.Run();
    QUENCH_ASSERT(ok, "blending job");
}

// Samples a state at `time` into `out`; false for an empty state.
bool sampleState(Character& c, const Character::StateRuntime& s, const Active& a, float time, Pose& out) {
    if (a.count == 0) return false;
    const float ratio = ratioAt(s.loop, time);
    const auto rest = c.skeleton->impl().skeleton.joint_rest_poses();
    for (int k = 0; k < a.count; ++k) {
        const Character::ClipSlot& slot = s.clips[std::size_t(a.slot[k])];
        Pose& target = a.count == 1 ? out : c.sample[k];
        if (!slot.clip) {
            copyPose(rest, target);
            continue;
        }
        ozz::animation::SamplingJob job;
        job.animation = &slot.clip->impl().animation;
        job.context = c.contexts[std::size_t(slot.context)].get();
        job.ratio = ratio;
        job.output = ozz::make_span(target);
        [[maybe_unused]] const bool ok = job.Run();
        QUENCH_ASSERT(ok, "sampling job");
        ++c.clipsSampled;
    }
    if (a.count == 2)
        blend2(c, ozz::make_span(c.sample[0]), a.weight[0], ozz::make_span(c.sample[1]), a.weight[1], out);
    return true;
}

// How often a marker at normalized position u lies in (t0, t1] (from t0 itself when the state was just entered).
std::uint32_t crossings(bool loop, float t0, float t1, float u, bool entered) {
    if (entered) t0 -= 1e-6f;
    if (t1 <= t0) return 0;
    if (!loop) return t0 < u && u <= t1 ? 1 : 0;
    const double n = std::floor(double(t1) - u) - std::floor(double(t0) - u);
    return n > 0 ? std::uint32_t(std::min(n, 1e6)) : 0;
}

void collectEvents(Character& c, const Character::StateRuntime& s, const Active& a, float t0, float t1, bool entered,
                   float weight, int layer, float threshold) {
    if (!c.desc.events || a.count == 0 || weight < threshold) return;
    const int k = a.count == 2 && a.weight[1] > a.weight[0] ? 1 : 0;
    const Clip* clip = s.clips[std::size_t(a.slot[k])].clip;
    if (!clip || clip->events().empty()) return;
    const float duration = clip->duration();
    for (const ClipEvent& e : clip->events()) {
        const std::uint32_t n = crossings(s.loop, t0, t1, e.time / duration, entered);
        if (n > 0) c.events.push_back({&e, clip, weight * a.weight[k], n, layer});
    }
}

struct MotionSum {
    glm::vec3 translation{0.0f};
    glm::quat rotation{0.0f, 0.0f, 0.0f, 0.0f}; // weighted sum, normalized at the end
    float weight = 0.0f;
};

void motionAt(const Clip& clip, float ratio, glm::vec3& position, glm::quat& rotation) {
    const detail::ClipImpl& impl = clip.impl();
    ozz::math::Float3 p;
    ozz::animation::Float3TrackSamplingJob pj;
    pj.track = &impl.motionPosition;
    pj.ratio = ratio;
    pj.result = &p;
    ozz::math::Quaternion q;
    ozz::animation::QuaternionTrackSamplingJob qj;
    qj.track = &impl.motionRotation;
    qj.ratio = ratio;
    qj.result = &q;
    [[maybe_unused]] const bool ok = pj.Run() && qj.Run();
    QUENCH_ASSERT(ok, "root motion sampling");
    position = {p.x, p.y, p.z};
    rotation = glm::normalize(glm::quat(q.w, q.x, q.y, q.z));
}

// The root motion between two normalized times, in the character's space at t0, through every loop in between.
void clipMotion(const Clip& clip, bool loop, float t0, float t1, glm::vec3& translation, glm::quat& rotation) {
    translation = glm::vec3(0.0f);
    rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (t1 <= t0) return;
    const auto segment = [&](float ra, float rb) {
        glm::vec3 pa, pb;
        glm::quat qa, qb;
        motionAt(clip, ra, pa, qa);
        motionAt(clip, rb, pb, qb);
        const glm::quat inverse = glm::inverse(qa);
        translation += rotation * (inverse * (pb - pa));
        rotation = glm::normalize(rotation * (inverse * qb));
    };
    if (!loop) {
        segment(std::clamp(t0, 0.0f, 1.0f), std::clamp(t1, 0.0f, 1.0f));
        return;
    }
    const double n0 = std::floor(double(t0)), n1 = std::floor(double(t1));
    const auto r0 = float(double(t0) - n0), r1 = float(double(t1) - n1);
    if (n0 == n1) {
        segment(r0, r1);
        return;
    }
    segment(r0, 1.0f);
    for (double n = n0 + 1; n < n1 && n < n0 + 64; ++n) segment(0.0f, 1.0f);
    segment(0.0f, r1);
}

void addMotion(const Character::StateRuntime& s, const Active& a, float t0, float t1, float weight, MotionSum& sum) {
    for (int k = 0; k < a.count; ++k) {
        const Clip* clip = s.clips[std::size_t(a.slot[k])].clip;
        const float w = weight * a.weight[k];
        if (!clip || !clip->hasRootMotion() || w <= 0.0f) continue;
        glm::vec3 translation;
        glm::quat rotation;
        clipMotion(*clip, s.loop, t0, t1, translation, rotation);
        sum.translation += w * translation;
        if (glm::dot(sum.rotation, rotation) < 0.0f) rotation = -rotation;
        sum.rotation += w * rotation;
        sum.weight += w;
    }
}

bool holds(const Character& c, const ConditionDef& cond) {
    const float v = c.params[std::size_t(cond.parameter)];
    switch (cond.op) {
    case CompareOp::Greater: return v > cond.value;
    case CompareOp::GreaterEqual: return v >= cond.value;
    case CompareOp::Less: return v < cond.value;
    case CompareOp::LessEqual: return v <= cond.value;
    case CompareOp::Equal: return v == cond.value;
    case CompareOp::NotEqual: return v != cond.value;
    }
    return false;
}

void checkTransitions(Character& c, Character::LayerRuntime& layer, float t0, float t1) {
    for (const TransitionDef& t : layer.def->transitions) {
        if (t.from >= 0 ? t.from != layer.current : t.to == layer.current) continue;
        if (t.trigger >= 0 && c.params[std::size_t(t.trigger)] < 0.5f) continue;
        if (!std::ranges::all_of(t.conditions, [&](const ConditionDef& cond) { return holds(c, cond); })) continue;
        if (t.exitTime >= 0.0f) {
            const bool loop = layer.states[std::size_t(layer.current)].loop;
            if (loop && t.exitTime < 1.0f) {
                if (crossings(true, t0, t1, t.exitTime, false) == 0) continue;
            } else if (t1 < t.exitTime) {
                continue;
            }
        }
        if (t.trigger >= 0) c.params[std::size_t(t.trigger)] = 0.0f;
        if (t.duration <= 0.0f) {
            layer.current = t.to;
            layer.time = 0.0f;
            layer.entered = true;
        } else {
            layer.next = t.to;
            layer.nextTime = 0.0f;
            layer.nextEntered = true;
            layer.fade = 0.0f;
            layer.fadeDuration = t.duration;
        }
        return;
    }
}

float layerWeight(const Character& c, const Character::LayerRuntime& layer, int index) {
    if (index == 0) return 1.0f;
    float w = layer.def->weight;
    if (layer.def->weightParameter >= 0) w *= std::clamp(c.params[std::size_t(layer.def->weightParameter)], 0.0f, 1.0f);
    return w;
}

void fillWeights(const Character::LayerRuntime& layer, float w, ozz::vector<ozz::math::SimdFloat4>& base,
                 ozz::vector<ozz::math::SimdFloat4>& over) {
    const ozz::math::SimdFloat4 one = ozz::math::simd_float4::one();
    const ozz::math::SimdFloat4 weight = ozz::math::simd_float4::Load1(w);
    for (std::size_t i = 0; i < base.size(); ++i) {
        over[i] = layer.mask.empty() ? weight : layer.mask[i] * weight;
        base[i] = one - over[i];
    }
}

void writePalettes(Character& c) {
    for (Character::Skin& skin : c.skins)
        for (std::size_t i = 0; i < skin.map.size(); ++i)
            skin.palette[i] =
                skin.map[i] >= 0 ? c.modelMatrices[std::size_t(skin.map[i])] * skin.inverseBind[i] : glm::mat4(1.0f);
}

} // namespace

void updateModels(Character& c, int from) {
    ozz::animation::LocalToModelJob job;
    job.skeleton = &c.skeleton->impl().skeleton;
    job.input = ozz::make_span(c.pose);
    job.output = ozz::make_span(c.models);
    job.from = from < 0 ? ozz::animation::Skeleton::kNoParent : from;
    [[maybe_unused]] const bool ok = job.Run();
    QUENCH_ASSERT(ok, "local-to-model job");
}

void evaluate(Character& c, float dt, const AnimationSettings& settings, const GroundQuery& ground) {
    c.events.clear();
    c.rootMotion = {};
    c.clipsSampled = 0;
    c.groundQueries = 0;
    if (!c.skeleton) return;
    const auto rest = c.skeleton->impl().skeleton.joint_rest_poses();
    dt = std::max(0.0f, dt * c.timeScale);
    if (!c.bound) {
        copyPose(rest, c.pose);
        updateModels(c);
        for (std::size_t j = 0; j < c.models.size(); ++j) c.modelMatrices[j] = toGlm(c.models[j]);
        writePalettes(c);
        return;
    }
    if (c.pendingPhase >= 0.0f) {
        c.layers[0].time = c.pendingPhase;
        c.pendingPhase = -1.0f;
    }

    MotionSum motion;
    for (std::size_t l = 0; l < c.layers.size(); ++l) {
        Character::LayerRuntime& layer = c.layers[l];
        const Character::StateRuntime& cur = layer.states[std::size_t(layer.current)];
        const Active a = activeClips(c, cur);
        const float t0 = layer.time, t1 = advanceTime(c, cur, a, t0, dt);
        const bool fading = layer.next >= 0;
        const Character::StateRuntime* next = fading ? &layer.states[std::size_t(layer.next)] : nullptr;
        const Active an = next ? activeClips(c, *next) : Active{};
        const float n0 = layer.nextTime, n1 = next ? advanceTime(c, *next, an, n0, dt) : 0.0f;
        const float f = !fading                     ? 0.0f
                        : layer.fadeDuration > 0.0f ? std::clamp((layer.fade + dt) / layer.fadeDuration, 0.0f, 1.0f)
                                                    : 1.0f;
        const float wNext = fading ? smoothstep(f) : 0.0f, wCur = 1.0f - wNext;
        const float lw = layerWeight(c, layer, int(l));

        collectEvents(c, cur, a, t0, t1, layer.entered, lw * wCur, int(l), settings.eventWeight);
        if (next) collectEvents(c, *next, an, n0, n1, layer.nextEntered, lw * wNext, int(l), settings.eventWeight);
        if (l == 0) {
            addMotion(cur, a, t0, t1, wCur, motion);
            if (next) addMotion(*next, an, n0, n1, wNext, motion);
        }

        const bool hasCur = sampleState(c, cur, a, t1, c.statePose[0]);
        const bool hasNext = next && sampleState(c, *next, an, n1, c.statePose[1]);
        if (l == 0) {
            const ozz::span<const SoaTransform> poseA = hasCur ? ozz::make_span(c.statePose[0]) : rest;
            const ozz::span<const SoaTransform> poseB = hasNext ? ozz::make_span(c.statePose[1]) : rest;
            if (fading)
                blend2(c, poseA, wCur, poseB, wNext, c.pose);
            else
                copyPose(poseA, c.pose);
        } else {
            const float ea = hasCur ? wCur : 0.0f, eb = hasNext ? wNext : 0.0f;
            const float w = lw * (ea + eb);
            if (w > 1e-4f) {
                ozz::span<const SoaTransform> layerPose;
                if (ea > 0.0f && eb > 0.0f) {
                    blend2(c, ozz::make_span(c.statePose[0]), ea, ozz::make_span(c.statePose[1]), eb, c.layerPose);
                    layerPose = ozz::make_span(c.layerPose);
                } else {
                    layerPose = ozz::make_span(ea > 0.0f ? c.statePose[0] : c.statePose[1]);
                }
                ozz::animation::BlendingJob job;
                ozz::animation::BlendingJob::Layer layers[2];
                job.threshold = 1e-4f;
                job.rest_pose = rest;
                job.output = ozz::make_span(c.scratch);
                if (layer.def->blend == LayerBlend::Override) {
                    fillWeights(layer, std::min(w, 1.0f), c.weightsA, c.weightsB);
                    layers[0] = {1.0f, ozz::make_span(c.pose), ozz::make_span(c.weightsA)};
                    layers[1] = {1.0f, layerPose, ozz::make_span(c.weightsB)};
                    job.layers = layers;
                } else {
                    layers[0] = {1.0f, ozz::make_span(c.pose), {}};
                    layers[1] = {std::min(w, 1.0f), layerPose,
                                 layer.mask.empty() ? ozz::span<const ozz::math::SimdFloat4>()
                                                    : ozz::make_span(layer.mask)};
                    job.layers = {layers, 1};
                    job.additive_layers = {layers + 1, 1};
                }
                [[maybe_unused]] const bool ok = job.Run();
                QUENCH_ASSERT(ok, "layer blending job");
                std::swap(c.pose, c.scratch);
            }
        }

        // Looping states drop 4096 loops at a time, so that float time keeps its precision in long sessions.
        layer.time = cur.loop && t1 >= 4096.0f ? t1 - 4096.0f : t1;
        layer.entered = false;
        if (fading) {
            layer.nextTime = next->loop && n1 >= 4096.0f ? n1 - 4096.0f : n1;
            layer.nextEntered = false;
            layer.fade += dt;
            if (layer.fade >= layer.fadeDuration) {
                layer.current = layer.next;
                layer.time = layer.nextTime;
                layer.next = -1;
            }
        } else {
            checkTransitions(c, layer, t0, t1);
        }
    }
    if (motion.weight > 0.0f) {
        c.rootMotion.translation = motion.translation / motion.weight;
        const float length = glm::length(motion.rotation);
        if (length > 1e-6f) c.rootMotion.rotation = motion.rotation / length;
    }

    updateModels(c);
    if (!c.legs.empty() && c.feetEnabled && ground) placeFeet(c, dt, ground);
    if (!c.lookChain.empty()) lookAt(c, dt);
    c.legStates.resize(c.legs.size());
    for (std::size_t i = 0; i < c.legs.size(); ++i) c.legStates[i] = c.legs[i].state;
    for (std::size_t j = 0; j < c.models.size(); ++j) c.modelMatrices[j] = toGlm(c.models[j]);
    writePalettes(c);
}

} // namespace quench::animation::detail
