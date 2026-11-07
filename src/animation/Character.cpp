// One character's update: state machines, sampling, layers, events, root motion, float channels, then IK (Ik.cpp) and
// palettes.

#include "Character.h"

#include <quench/core/Assert.h>

#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/track_sampling_job.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace quench::animation::detail {

namespace {

using Character = AnimationSystem::Character;
using ozz::math::SoaTransform;
using Pose = ozz::vector<SoaTransform>;

// The clips of a state that contribute (a 1D blend tree is linear between neighbouring points, a blend space has its
// gradient band weights), weights summing to 1.
struct Active {
    std::array<int, kMaxBlendPoints> slot{};
    std::array<float, kMaxBlendPoints> weight{};
    int count = 0;

    void add(int s, float w) {
        slot[std::size_t(count)] = s;
        weight[std::size_t(count)] = w;
        ++count;
    }
};

Active activeClips(const Character& c, const Character::StateRuntime& s) {
    Active a;
    if (s.clips.empty()) return a;
    if (!s.points.empty()) {
        const glm::vec2 p(c.params[std::size_t(s.def->blend2dParameters[0])],
                          c.params[std::size_t(s.def->blend2dParameters[1])]);
        std::array<float, kMaxBlendPoints> w{};
        blendSpaceWeights(s.points, p, std::span(w).first(s.points.size()));
        // Weights under 1e-4 are dropped (as the blending job's threshold would) and the rest normalized again.
        float total = 0.0f;
        for (std::size_t i = 0; i < s.points.size(); ++i)
            if (w[i] >= 1e-4f) total += w[i];
        for (std::size_t i = 0; i < s.points.size(); ++i)
            if (w[i] >= 1e-4f) a.add(int(i), w[i] / total);
        return a;
    }
    if (s.def->blend.empty() || s.clips.size() == 1) {
        a.add(0, 1.0f);
        return a;
    }
    const float p = c.params[std::size_t(s.def->blendParameter)];
    const int last = int(s.clips.size()) - 1;
    if (p <= s.clips[0].at) {
        a.add(0, 1.0f);
        return a;
    }
    if (p >= s.clips[std::size_t(last)].at) {
        a.add(last, 1.0f);
        return a;
    }
    int i = 0;
    while (i + 1 < last && p >= s.clips[std::size_t(i + 1)].at) ++i;
    const float lo = s.clips[std::size_t(i)].at, hi = s.clips[std::size_t(i + 1)].at;
    const float t = hi > lo ? (p - lo) / (hi - lo) : 1.0f;
    if (t < 1e-4f) {
        a.add(i, 1.0f);
    } else if (t > 1.0f - 1e-4f) {
        a.add(i + 1, 1.0f);
    } else {
        a.add(i, 1.0f - t);
        a.add(i + 1, t);
    }
    return a;
}

// The clip that delivers a state's events: the heaviest.
int dominant(const Active& a) {
    int k = 0;
    for (int i = 1; i < a.count; ++i)
        if (a.weight[std::size_t(i)] > a.weight[std::size_t(k)]) k = i;
    return k;
}

float clipDuration(const Character::ClipSlot& slot) { return slot.clip ? slot.clip->duration() : 1.0f; }

float advanceTime(const Character& c, const Character::StateRuntime& s, const Active& a, float time, float dt) {
    if (a.count == 0) return time + dt; // an empty state: seconds (exit times count in them)
    float duration = 0.0f;
    for (int k = 0; k < a.count; ++k)
        duration += a.weight[std::size_t(k)] * clipDuration(s.clips[std::size_t(a.slot[std::size_t(k)])]);
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
    const auto rest = c.skeleton->impl().skeleton.joint_rest_poses();
    std::array<ozz::animation::BlendingJob::Layer, kMaxBlendPoints> layers;
    for (int k = 0; k < a.count; ++k) {
        const Character::ClipSlot& slot = s.clips[std::size_t(a.slot[std::size_t(k)])];
        Pose& target = a.count == 1 ? out : c.samples[std::size_t(k)];
        layers[std::size_t(k)].weight = a.weight[std::size_t(k)];
        layers[std::size_t(k)].transform = ozz::make_span(target);
        if (!slot.clip) {
            copyPose(rest, target);
            continue;
        }
        ozz::animation::SamplingJob job;
        job.animation = &slot.clip->impl().animation;
        job.context = c.contexts[std::size_t(slot.context)].get();
        job.ratio = ratioAt(s.loop, time + slot.phase);
        job.output = ozz::make_span(target);
        [[maybe_unused]] const bool ok = job.Run();
        QUENCH_ASSERT(ok, "sampling job");
        ++c.clipsSampled;
    }
    if (a.count > 1) {
        ozz::animation::BlendingJob job;
        job.threshold = 1e-4f;
        job.layers = {layers.data(), std::size_t(a.count)};
        job.rest_pose = rest;
        job.output = ozz::make_span(out);
        [[maybe_unused]] const bool ok = job.Run();
        QUENCH_ASSERT(ok, "blending job");
    }
    return true;
}

// Samples a state's float channels at `time` into `out` (per character channel: weight times value, weight), the
// clips at the ratios sampleState uses.
void sampleChannels(const Character::StateRuntime& s, const Active& a, float time, std::vector<glm::vec2>& out) {
    std::fill(out.begin(), out.end(), glm::vec2(0.0f));
    for (int k = 0; k < a.count; ++k) {
        const Character::ClipSlot& slot = s.clips[std::size_t(a.slot[std::size_t(k)])];
        if (!slot.clip || slot.channels.empty()) continue;
        const float w = a.weight[std::size_t(k)];
        const detail::ClipImpl& impl = slot.clip->impl();
        for (std::size_t j = 0; j < slot.channels.size(); ++j) {
            float value = 0.0f;
            ozz::animation::FloatTrackSamplingJob job;
            job.track = &impl.channels[j];
            job.ratio = ratioAt(s.loop, time + slot.phase);
            job.result = &value;
            [[maybe_unused]] const bool ok = job.Run();
            QUENCH_ASSERT(ok, "channel sampling job");
            out[std::size_t(slot.channels[j])] += glm::vec2(w * value, w);
        }
    }
}

// The float channels of a layer above the first into c.channels: an override layer moves a channel toward its own
// value by its weight times the share of its clips that have the channel (a clip without it leaves it to the layers
// below), an additive layer adds its difference.
void layerChannels(Character& c, const LayerDef& def, float ea, float eb, float w) {
    const float total = ea + eb;
    for (std::size_t i = 0; i < c.channels.size(); ++i) {
        glm::vec2 l(0.0f);
        if (ea > 0.0f) l += ea * c.stateChannels[0][i];
        if (eb > 0.0f) l += eb * c.stateChannels[1][i];
        l /= total;
        glm::vec2& ch = c.channels[i];
        if (def.blend == LayerBlend::Override)
            ch = (1.0f - w * l.y) * ch + w * l;
        else
            ch.x += w * l.x;
    }
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
    const std::size_t k = std::size_t(dominant(a));
    const Character::ClipSlot& slot = s.clips[std::size_t(a.slot[k])];
    const Clip* clip = slot.clip;
    if (!clip || clip->events().empty()) return;
    const float duration = clip->duration();
    for (const ClipEvent& e : clip->events()) {
        const std::uint32_t n = crossings(s.loop, t0 + slot.phase, t1 + slot.phase, e.time / duration, entered);
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
        const Character::ClipSlot& slot = s.clips[std::size_t(a.slot[std::size_t(k)])];
        const Clip* clip = slot.clip;
        const float w = weight * a.weight[std::size_t(k)];
        if (!clip || !clip->hasRootMotion() || w <= 0.0f) continue;
        glm::vec3 translation;
        glm::quat rotation;
        clipMotion(*clip, s.loop, t0 + slot.phase, t1 + slot.phase, translation, rotation);
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

void writeAllChannels(Character& c) {
    for (Character::ChannelSet& set : c.channelSets) writeChannels(c, set);
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

namespace {

// Samples, blends and poses the character by `dt` into c.pose and c.models (with IK), its events and its root motion
// into c.evaluatedMotion.
void evaluate(Character& c, float dt, const AnimationSettings& settings, const GroundQuery& ground) {
    c.evaluatedMotion = {};
    const auto rest = c.skeleton->impl().skeleton.joint_rest_poses();
    dt = std::max(0.0f, dt * c.timeScale);
    if (!c.bound) {
        copyPose(rest, c.pose);
        updateModels(c);
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
        const bool channels = !c.channels.empty();
        if (channels) {
            sampleChannels(cur, a, t1, c.stateChannels[0]);
            if (next) sampleChannels(*next, an, n1, c.stateChannels[1]);
        }
        if (l == 0) {
            const ozz::span<const SoaTransform> poseA = hasCur ? ozz::make_span(c.statePose[0]) : rest;
            const ozz::span<const SoaTransform> poseB = hasNext ? ozz::make_span(c.statePose[1]) : rest;
            if (fading)
                blend2(c, poseA, wCur, poseB, wNext, c.pose);
            else
                copyPose(poseA, c.pose);
            // An empty state's clips are the rest pose: none has a channel.
            for (std::size_t i = 0; channels && i < c.channels.size(); ++i)
                c.channels[i] = (hasCur ? wCur * c.stateChannels[0][i] : glm::vec2(0.0f)) +
                                (hasNext ? wNext * c.stateChannels[1][i] : glm::vec2(0.0f));
        } else {
            const float ea = hasCur ? wCur : 0.0f, eb = hasNext ? wNext : 0.0f;
            const float w = lw * (ea + eb);
            if (w > 1e-4f && channels) layerChannels(c, *layer.def, ea, eb, std::min(w, 1.0f));
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
        c.evaluatedMotion.translation = motion.translation / motion.weight;
        const float length = glm::length(motion.rotation);
        if (length > 1e-6f) c.evaluatedMotion.rotation = motion.rotation / length;
    }

    updateModels(c);
    if (!c.legs.empty() && c.feetEnabled && c.lodIk && ground) placeFeet(c, dt, ground);
    if (!c.lookChain.empty() && c.lodIk) lookAt(c, dt);
    c.legStates.resize(c.legs.size());
    for (std::size_t i = 0; i < c.legs.size(); ++i) c.legStates[i] = c.legs[i].state;
}

// Reports the owed root motion over the frames it is spread over: all of it at once without LOD.
void reportMotion(Character& c) {
    if (c.owedFrames <= 1) {
        c.rootMotion = c.owedMotion;
        c.owedMotion = {};
        c.owedFrames = 0;
        return;
    }
    const float share = 1.0f / float(c.owedFrames);
    c.rootMotion.translation = c.owedMotion.translation * share;
    c.rootMotion.rotation = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), c.owedMotion.rotation, share);
    c.owedMotion.translation -= c.rootMotion.translation;
    c.owedMotion.rotation = glm::normalize(glm::inverse(c.rootMotion.rotation) * c.owedMotion.rotation);
    --c.owedFrames;
}

} // namespace

void update(Character& c, const AnimationSettings& settings, const GroundQuery& ground) {
    c.events.clear();
    c.rootMotion = {};
    c.clipsSampled = 0;
    c.groundQueries = 0;
    if (!c.skeleton) return;
    // An evaluation ahead of the time shown is reached from the pose shown last frame (lodSpan before it).
    const bool ahead = c.lodEvaluate && c.lodLag > 1e-6f;
    if (ahead) {
        copyPose(ozz::make_span(c.pose), c.lodFrom);
        c.lodChannelsFrom = c.channels;
    }
    if (c.lodEvaluate) {
        evaluate(c, c.lodDt, settings, ground);
        c.owedMotion.translation += c.evaluatedMotion.translation;
        c.owedMotion.rotation = c.owedMotion.rotation * c.evaluatedMotion.rotation;
        c.owedFrames = std::max(c.lodFrames, 1);
        c.lodBlending = ahead;
        if (ahead) {
            copyPose(ozz::make_span(c.pose), c.lodTo);
            c.lodChannelsTo = c.channels;
        }
    }
    reportMotion(c);

    c.poseChanged = c.lodEvaluate || c.lodBlending;
    c.lodShown = true;
    if (c.lodBlending) {
        const float f = c.lodSpan > 0.0f ? std::clamp(1.0f - c.lodLag / c.lodSpan, 0.0f, 1.0f) : 1.0f;
        if (f >= 1.0f) {
            copyPose(ozz::make_span(c.lodTo), c.pose);
            c.channels = c.lodChannelsTo;
            c.lodBlending = false;
        } else {
            blend2(c, ozz::make_span(c.lodFrom), 1.0f - f, ozz::make_span(c.lodTo), f, c.pose);
            for (std::size_t i = 0; i < c.channels.size() && i < c.lodChannelsFrom.size(); ++i)
                c.channels[i] = (1.0f - f) * c.lodChannelsFrom[i] + f * c.lodChannelsTo[i];
        }
        updateModels(c);
    }
    if (!c.poseChanged) return;
    for (std::size_t j = 0; j < c.models.size(); ++j) c.modelMatrices[j] = toGlm(c.models[j]);
    writePalettes(c);
    writeAllChannels(c);
}

void writeChannels(const Character& c, Character::ChannelSet& set) {
    for (std::size_t i = 0; i < set.map.size(); ++i) {
        const int channel = set.map[i];
        if (channel < 0 || std::size_t(channel) >= c.channels.size()) {
            set.values[i] = set.rest[i];
            continue;
        }
        const glm::vec2 ch = c.channels[std::size_t(channel)];
        set.values[i] = ch.x + (1.0f - ch.y) * set.rest[i];
    }
}

} // namespace quench::animation::detail
