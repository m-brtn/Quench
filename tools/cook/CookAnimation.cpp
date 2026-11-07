// Skeletons and clips from glTF skins and animations (Cook.h), built by the animation module's offline path; morph
// target weights become the clips' float channels, named like the model cook names the blend shapes.

#include "Cook.h"
#include "Gltf.h"

#include <quench/animation/Clip.h>
#include <quench/animation/Skeleton.h>
#include <quench/core/Json.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>

namespace quench::cook {
namespace {

using animation::ClipDesc;
using animation::JointTrack;
using animation::Key;
using animation::SkeletonDesc;

constexpr float kResampleRate = 30.0f; // cubic spline keys per second
constexpr float kStepGap = 1e-4f;      // s: a step key is held until this long before the next one
// rad: glTF interpolates linear rotation keys spherically, the runtime normalized-linearly, which lags in the middle
// of a wide step (0.9 degrees at 90); wider steps get spherically interpolated keys in between (error below 1e-4 rad).
constexpr float kMaxLinearAngle = 0.2f;

struct GltfSkeleton {
    SkeletonDesc desc;
    std::map<std::string, int, std::less<>> jointOfName;
};

Result<GltfSkeleton> skeletonOf(const fastgltf::Asset& asset) {
    const std::size_t n = asset.nodes.size();
    const std::vector<int> parents = nodeParents(asset);
    const std::vector<std::string> names = jointNames(asset);

    // Depth first from the roots, children in their order; a node reached twice is part of a cycle.
    std::vector<std::size_t> order;
    std::vector<char> visited(n, 0);
    for (std::size_t root = 0; root < n; ++root) {
        if (parents[root] >= 0) continue;
        std::vector<std::size_t> stack{root};
        while (!stack.empty()) {
            const std::size_t node = stack.back();
            stack.pop_back();
            if (visited[node]) return Error{std::format("node {} has two parents", node)};
            visited[node] = 1;
            order.push_back(node);
            const auto& children = asset.nodes[node].children;
            for (auto it = children.rbegin(); it != children.rend(); ++it)
                if (*it < n) stack.push_back(*it);
        }
    }
    if (order.size() != n) return Error{"the node hierarchy has a cycle"};

    std::vector<char> joint(n, 0);
    const auto markWithAncestors = [&](std::size_t node) {
        for (int p = int(node); p >= 0 && !joint[std::size_t(p)]; p = parents[std::size_t(p)])
            joint[std::size_t(p)] = 1;
    };
    for (const fastgltf::Skin& skin : asset.skins)
        for (const std::size_t j : skin.joints)
            if (j < n) markWithAncestors(j);
    if (asset.skins.empty())
        for (const fastgltf::Animation& a : asset.animations)
            for (const fastgltf::AnimationChannel& c : a.channels)
                if (c.nodeIndex && *c.nodeIndex < n) markWithAncestors(*c.nodeIndex);
    for (const std::size_t node : order)
        if (!joint[node] && parents[node] >= 0 && joint[std::size_t(parents[node])] && !asset.nodes[node].meshIndex)
            joint[node] = 1;

    GltfSkeleton out;
    std::vector<int> jointOfNode(n, -1);
    for (const std::size_t node : order) {
        if (!joint[node]) continue;
        const NodeTransform t = nodeTransform(asset.nodes[node]);
        jointOfNode[node] = int(out.desc.joints.size());
        out.jointOfName.emplace(names[node], jointOfNode[node]);
        out.desc.joints.push_back({.name = names[node],
                                   .parent = parents[node] >= 0 ? jointOfNode[std::size_t(parents[node])] : -1,
                                   .rest = {t.translation, t.rotation, t.scale}});
    }
    if (out.desc.joints.empty()) return Error{"no skin and no animated node: nothing to make a skeleton of"};
    return out;
}

Result<std::vector<float>> readTimes(const fastgltf::Asset& asset, std::size_t accessorIndex, std::string_view what) {
    if (accessorIndex >= asset.accessors.size()) return Error{std::format("{}: no such accessor", what)};
    const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
    if (accessor.type != fastgltf::AccessorType::Scalar || accessor.count == 0)
        return Error{std::format("{}: key times are not a list of numbers", what)};
    std::vector<float> times(accessor.count);
    fastgltf::iterateAccessorWithIndex<float>(asset, accessor, [&](float t, std::size_t i) { times[i] = t; });
    for (std::size_t i = 0; i < times.size(); ++i)
        if (!std::isfinite(times[i]) || (i > 0 && times[i] <= times[i - 1]))
            return Error{std::format("{}: key times do not increase", what)};
    return times;
}

template <class V>
Result<std::vector<V>> readValues(const fastgltf::Asset& asset, std::size_t accessorIndex, std::size_t count,
                                  std::string_view what) {
    constexpr fastgltf::AccessorType kType = std::is_same_v<V, float> ? fastgltf::AccessorType::Scalar
                                             : sizeof(V) == 12        ? fastgltf::AccessorType::Vec3
                                                                      : fastgltf::AccessorType::Vec4;
    if (accessorIndex >= asset.accessors.size()) return Error{std::format("{}: no such accessor", what)};
    const fastgltf::Accessor& accessor = asset.accessors[accessorIndex];
    if (accessor.type != kType || accessor.count != count)
        return Error{std::format("{}: {} values for {} keys", what, accessor.count, count)};
    using Gltf = std::conditional_t<std::is_same_v<V, float>, float,
                                    std::conditional_t<sizeof(V) == 12, fastgltf::math::fvec3, fastgltf::math::fvec4>>;
    std::vector<V> values(count);
    fastgltf::iterateAccessorWithIndex<Gltf>(asset, accessor, [&](const Gltf& v, std::size_t i) {
        if constexpr (std::is_same_v<V, float>)
            values[i] = v;
        else if constexpr (sizeof(V) == 12)
            values[i] = V(v[0], v[1], v[2]);
        else
            values[i] = V(v[3], v[0], v[1], v[2]); // glTF quaternions are x, y, z, w
    });
    return values;
}

template <class V>
V hermite(const V& p0, const V& m0, const V& p1, const V& m1, float s) {
    const float s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * m1;
}

// Keys of one channel in the module's form: linear keys as they are (wide rotation steps split), step keys held, cubic
// splines resampled.
template <class V>
std::vector<Key<V>> channelKeys(const std::vector<float>& times, const std::vector<V>& values,
                                fastgltf::AnimationInterpolation interpolation, float start) {
    std::vector<Key<V>> keys;
    const std::size_t count = times.size();
    if (interpolation == fastgltf::AnimationInterpolation::CubicSpline) {
        // Per key: in-tangent, value, out-tangent; tangents are per second.
        for (std::size_t k = 0; k + 1 < count; ++k) {
            const float d = times[k + 1] - times[k];
            const int steps = std::max(1, int(std::ceil(d * kResampleRate)));
            for (int s = 0; s < steps; ++s) {
                const float u = float(s) / float(steps);
                keys.push_back({times[k] + u * d - start, hermite(values[3 * k + 1], d * values[3 * k + 2],
                                                                  values[3 * k + 4], d * values[3 * k + 3], u)});
            }
        }
        keys.push_back({times[count - 1] - start, values[3 * (count - 1) + 1]});
    } else {
        for (std::size_t k = 0; k < count; ++k) {
            if (interpolation == fastgltf::AnimationInterpolation::Step && k > 0) {
                const float gap = std::min(kStepGap, 0.5f * (times[k] - times[k - 1]));
                keys.push_back({times[k] - gap - start, values[k - 1]});
            }
            keys.push_back({times[k] - start, values[k]});
        }
    }
    if constexpr (std::is_same_v<V, glm::quat>) {
        for (std::size_t k = 0; k < keys.size(); ++k) {
            keys[k].value = glm::normalize(keys[k].value);
            if (k > 0 && glm::dot(keys[k - 1].value, keys[k].value) < 0.0f) keys[k].value = -keys[k].value;
        }
        if (interpolation == fastgltf::AnimationInterpolation::Linear) {
            std::vector<Key<V>> fine;
            for (std::size_t k = 0; k < keys.size(); ++k) {
                if (k > 0) {
                    const Key<V>& a = keys[k - 1];
                    const float angle = 2.0f * std::acos(std::min(1.0f, glm::dot(a.value, keys[k].value)));
                    const int steps = int(std::ceil(angle / kMaxLinearAngle));
                    for (int i = 1; i < steps; ++i) {
                        const float u = float(i) / float(steps);
                        fine.push_back({a.time + u * (keys[k].time - a.time), glm::slerp(a.value, keys[k].value, u)});
                    }
                }
                fine.push_back(keys[k]);
            }
            keys = std::move(fine);
        }
    }
    return keys;
}

struct ClipSettings {
    animation::ClipBuildOptions build;
    std::map<std::string, const json::Value*, std::less<>> clips;
    json::Value document;
};

Result<void> applySettings(const json::Value& v, ClipDesc& clip) {
    static const std::set<std::string, std::less<>> kKeys = {"loop", "additive", "events", "rootMotion"};
    for (const auto& [key, value] : v.object)
        if (!kKeys.contains(key)) return Error{std::format("clip '{}': unknown setting '{}'", clip.name, key)};
    if (v.has("loop")) {
        if (v["loop"].type != json::Value::Type::Bool)
            return Error{std::format("clip '{}': loop is true or false", clip.name)};
        clip.loop = v["loop"].boolean;
    }
    if (v.has("additive")) {
        if (v["additive"].type != json::Value::Type::Bool)
            return Error{std::format("clip '{}': additive is true or false", clip.name)};
        clip.additive = v["additive"].boolean;
    }
    if (v.has("events")) {
        if (!v["events"].isArray()) return Error{std::format("clip '{}': events is a list", clip.name)};
        for (const json::Value& e : v["events"].array) {
            const animation::ClipEvent event{float(e["time"].numberOr(NAN)), e["name"].stringOr(""),
                                             float(e["value"].numberOr(0.0))};
            if (!(event.time >= 0.0f && event.time <= clip.duration) || event.name.empty())
                return Error{
                    std::format("clip '{}': an event needs a name and a time in [0, {}] s", clip.name, clip.duration)};
            clip.events.push_back(event);
        }
    }
    if (v.has("rootMotion")) {
        const json::Value& r = v["rootMotion"];
        if (!r.isObject() || r["joint"].stringOr("").empty())
            return Error{std::format("clip '{}': rootMotion needs a joint", clip.name)};
        clip.rootMotion = animation::RootMotionDesc{r["joint"].string, r["rotation"].boolOr(false)};
    }
    return {};
}

Result<ClipSettings> readSettings(const std::filesystem::path& source, const AnimationOptions& options,
                                  std::vector<std::filesystem::path>& inputs) {
    ClipSettings out;
    std::filesystem::path path = options.clips;
    if (path.empty()) {
        path = std::filesystem::path(source).replace_extension(".anim.json");
        if (!std::filesystem::exists(path)) return out;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{std::format("cannot open {}", path.string())};
    inputs.push_back(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Result<json::Value> doc = json::parse(text);
    if (!doc) return Error{std::format("{}: {}", path.string(), doc.error())};
    out.document = std::move(*doc);
    const json::Value& root = out.document;
    if (!root.isObject()) return Error{std::format("{}: not an object", path.string())};
    out.build.tolerance = float(root["tolerance"].numberOr(out.build.tolerance));
    out.build.distance = float(root["distance"].numberOr(out.build.distance));
    out.build.optimize = root["optimize"].boolOr(out.build.optimize);
    if (!(out.build.tolerance > 0.0f) || !(out.build.distance > 0.0f))
        return Error{std::format("{}: tolerance and distance are positive", path.string())};
    if (root.has("clips")) {
        if (!root["clips"].isObject())
            return Error{std::format("{}: clips is an object of name: settings", path.string())};
        for (const auto& [name, value] : root["clips"].object) {
            if (!value.isObject()) return Error{std::format("{}: clip '{}' is not an object", path.string(), name)};
            out.clips.emplace(name, &value);
        }
    }
    return out;
}

// A weights channel's keys as one float channel per morph target of the node's mesh; false with a note when the node
// has no mesh with morph targets.
Result<bool> weightChannels(const fastgltf::Asset& asset, const GltfExtras& extras, std::size_t node,
                            const fastgltf::AnimationSampler& sampler, float start, const std::string& where,
                            ClipDesc& clip, std::vector<std::string>& notes) {
    const std::optional<std::size_t> mesh = asset.nodes[node].meshIndex;
    if (!mesh || *mesh >= asset.meshes.size() || asset.meshes[*mesh].primitives.empty() ||
        asset.meshes[*mesh].primitives[0].targets.empty()) {
        notes.push_back(std::format("{}: weights of a node without blend shapes; dropped", where));
        return false;
    }
    const std::size_t targets = asset.meshes[*mesh].primitives[0].targets.size();
    Result<std::vector<float>> times = readTimes(asset, sampler.inputAccessor, where);
    if (!times) return Error{times.error()};
    const std::size_t parts = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline ? 3 : 1;
    Result<std::vector<float>> values =
        readValues<float>(asset, sampler.outputAccessor, times->size() * parts * targets,
                          std::format("{} (weights of {} blend shapes)", where, targets));
    if (!values) return Error{values.error()};
    std::vector<float> one(times->size() * parts);
    for (std::size_t k = 0; k < targets; ++k) {
        animation::ChannelTrack channel{morphTargetName(asset, extras, *mesh, k), {}};
        if (std::ranges::find(clip.channels, channel.name, &animation::ChannelTrack::name) != clip.channels.end()) {
            notes.push_back(
                std::format("{}: blend shape '{}' is animated twice; the first is kept", where, channel.name));
            continue;
        }
        for (std::size_t i = 0; i < one.size(); ++i) one[i] = (*values)[i * targets + k];
        channel.keys = channelKeys(*times, one, sampler.interpolation, start);
        clip.channels.push_back(std::move(channel));
    }
    return true;
}

Result<std::vector<ClipDesc>> clipsOf(const fastgltf::Asset& asset, const GltfExtras& extras,
                                      const GltfSkeleton& skeleton, std::vector<std::string>& notes) {
    const std::vector<std::string> names = jointNames(asset);
    std::vector<ClipDesc> clips;
    for (std::size_t a = 0; a < asset.animations.size(); ++a) {
        const fastgltf::Animation& animation = asset.animations[a];
        ClipDesc clip;
        clip.name = animation.name.empty() ? std::format("clip{}", a) : std::string(animation.name);
        const std::string what = std::format("animation '{}'", clip.name);

        float start = std::numeric_limits<float>::max(), end = std::numeric_limits<float>::lowest();
        for (const fastgltf::AnimationSampler& sampler : animation.samplers) {
            Result<std::vector<float>> times = readTimes(asset, sampler.inputAccessor, what);
            if (!times) return Error{times.error()};
            start = std::min(start, times->front());
            end = std::max(end, times->back());
        }
        if (animation.samplers.empty()) {
            notes.push_back(std::format("{}: no keys; dropped", what));
            continue;
        }
        if (start != 0.0f) notes.push_back(std::format("{}: starts at {} s; moved to 0", what, start));
        clip.duration = end - start;
        if (!(clip.duration > 0.0f)) {
            clip.duration = 1.0f / kResampleRate;
            notes.push_back(std::format("{}: one pose; cooked as a clip of {} s", what, clip.duration));
        }

        clip.tracks.resize(skeleton.desc.joints.size());
        for (const fastgltf::AnimationChannel& channel : animation.channels) {
            if (!channel.nodeIndex || *channel.nodeIndex >= names.size()) continue;
            const std::string& node = names[*channel.nodeIndex];
            if (channel.samplerIndex >= animation.samplers.size())
                return Error{std::format("{}: no sampler {}", what, channel.samplerIndex)};
            if (channel.path == fastgltf::AnimationPath::Weights) {
                Result<bool> r =
                    weightChannels(asset, extras, *channel.nodeIndex, animation.samplers[channel.samplerIndex], start,
                                   std::format("{} '{}'", what, node), clip, notes);
                if (!r) return Error{r.error()};
                continue;
            }
            const auto joint = skeleton.jointOfName.find(node);
            if (joint == skeleton.jointOfName.end()) {
                notes.push_back(
                    std::format("{}: '{}' is not a joint of the skeleton; its keys are dropped", what, node));
                continue;
            }
            const fastgltf::AnimationSampler& sampler = animation.samplers[channel.samplerIndex];
            Result<std::vector<float>> times = readTimes(asset, sampler.inputAccessor, what);
            if (!times) return Error{times.error()};
            const std::size_t count =
                times->size() * (sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline ? 3 : 1);
            JointTrack& track = clip.tracks[std::size_t(joint->second)];
            const std::string where = std::format("{} '{}'", what, node);
            if (channel.path == fastgltf::AnimationPath::Rotation) {
                Result<std::vector<glm::quat>> values =
                    readValues<glm::quat>(asset, sampler.outputAccessor, count, where);
                if (!values) return Error{values.error()};
                track.rotations = channelKeys(*times, *values, sampler.interpolation, start);
            } else {
                Result<std::vector<glm::vec3>> values =
                    readValues<glm::vec3>(asset, sampler.outputAccessor, count, where);
                if (!values) return Error{values.error()};
                (channel.path == fastgltf::AnimationPath::Translation ? track.translations : track.scales) =
                    channelKeys(*times, *values, sampler.interpolation, start);
            }
        }
        clips.push_back(std::move(clip));
    }
    if (clips.empty()) return Error{"the file has no animation"};
    return clips;
}

Result<fastgltf::Asset> load(const std::filesystem::path& source, std::vector<std::filesystem::path>& inputs,
                             GltfExtras* extras = nullptr) {
    inputs.push_back(source);
    return loadGltf(source, fastgltf::Options::DecomposeNodeMatrices, inputs, extras);
}

} // namespace

Result<CookedAnimation> cookSkeleton(const std::filesystem::path& source) {
    CookedAnimation out;
    Result<fastgltf::Asset> asset = load(source, out.inputs);
    if (!asset) return Error{asset.error()};
    Result<GltfSkeleton> skeleton = skeletonOf(*asset);
    if (!skeleton) return Error{std::format("{}: {}", source.string(), skeleton.error())};
    Result<std::shared_ptr<const animation::Skeleton>> built = animation::buildSkeleton(skeleton->desc);
    if (!built) return Error{std::format("{}: {}", source.string(), built.error())};
    out.file = animation::writeSkeleton(**built);
    for (int j = 0; j < (*built)->jointCount(); ++j) out.names.emplace_back((*built)->jointName(j));
    return out;
}

Result<CookedAnimation> cookAnimation(const std::filesystem::path& source, const AnimationOptions& options) {
    CookedAnimation out;
    if (!options.retarget.empty() && options.skeleton.empty())
        return Error{"retargeting needs the target's skeleton (--skeleton)"};
    GltfExtras extras;
    Result<fastgltf::Asset> asset = load(source, out.inputs, &extras);
    if (!asset) return Error{asset.error()};
    const std::filesystem::path& skeletonSource = options.skeleton.empty() ? source : options.skeleton;
    Result<fastgltf::Asset> other =
        options.skeleton.empty() ? Result<fastgltf::Asset>(Error{""}) : load(options.skeleton, out.inputs);
    if (!options.skeleton.empty() && !other) return Error{other.error()};
    Result<GltfSkeleton> skeleton = skeletonOf(options.skeleton.empty() ? *asset : *other);
    if (!skeleton) return Error{std::format("{}: {}", skeletonSource.string(), skeleton.error())};
    Result<std::shared_ptr<const animation::Skeleton>> built = animation::buildSkeleton(skeleton->desc);
    if (!built) return Error{std::format("{}: {}", skeletonSource.string(), built.error())};

    // Retargeted: the clips are read for the input's own skeleton (and its clip settings name its joints).
    Result<GltfSkeleton> own = options.retarget.empty() ? Result<GltfSkeleton>(Error{""}) : skeletonOf(*asset);
    if (!options.retarget.empty() && !own) return Error{std::format("{}: {}", source.string(), own.error())};
    Result<std::vector<ClipDesc>> clips =
        clipsOf(*asset, extras, options.retarget.empty() ? *skeleton : *own, out.notes);
    if (!clips) return Error{std::format("{}: {}", source.string(), clips.error())};
    Result<ClipSettings> settings = readSettings(source, options, out.inputs);
    if (!settings) return Error{settings.error()};
    for (const auto& [name, value] : settings->clips) {
        const auto clip = std::ranges::find(*clips, name, &ClipDesc::name);
        if (clip == clips->end())
            return Error{std::format("{}: the clip settings name '{}', which the file has no animation of",
                                     source.string(), name)};
        if (Result<void> r = applySettings(*value, *clip); !r)
            return Error{std::format("{}: {}", source.string(), r.error())};
    }
    if (!options.retarget.empty()) {
        std::ifstream in(options.retarget, std::ios::binary);
        if (!in) return Error{std::format("cannot open {}", options.retarget.string())};
        out.inputs.push_back(options.retarget);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Result<RetargetMap> map = parseRetargetMap(text);
        if (!map) return Error{std::format("{}: {}", options.retarget.string(), map.error())};
        Result<std::vector<ClipDesc>> retargeted = retargetClips(own->desc, skeleton->desc, *clips, *map, &out.notes);
        if (!retargeted) return Error{std::format("{}: {}", options.retarget.string(), retargeted.error())};
        clips = std::move(retargeted);
    }
    Result<std::shared_ptr<const animation::ClipSet>> set =
        animation::buildClipSet(**built, skeleton->desc, *clips, settings->build);
    if (!set) return Error{std::format("{}: {}", source.string(), set.error())};
    out.file = animation::writeClipSet(**set);
    for (const animation::Clip& clip : (*set)->clips()) out.names.push_back(clip.name());
    return out;
}

} // namespace quench::cook
