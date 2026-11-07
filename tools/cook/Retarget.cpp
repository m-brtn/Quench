// Retargeting at cook time (Cook.h): the clips of one skeleton turned into clips of another, through a bone map.

#include "Cook.h"

#include <quench/core/Json.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace quench::cook {
namespace {

using animation::ClipDesc;
using animation::JointTrack;
using animation::Key;
using animation::SkeletonDesc;

constexpr float kGridRate = 30.0f; // keys per second at least, besides the source's own key times

template <class V>
V sample(const std::vector<Key<V>>& keys, float time, const V& rest) {
    if (keys.empty()) return rest;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    const auto b =
        std::upper_bound(keys.begin(), keys.end(), time, [](float t, const Key<V>& k) { return t < k.time; });
    const auto a = b - 1;
    const float u = (time - a->time) / (b->time - a->time);
    if constexpr (std::is_same_v<V, glm::quat>) {
        // Normalized-linear, as the runtime interpolates.
        const glm::quat to = glm::dot(a->value, b->value) < 0.0f ? -b->value : b->value;
        return glm::normalize(a->value * (1.0f - u) + to * u);
    } else {
        return a->value + (b->value - a->value) * u;
    }
}

glm::mat4 local(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    glm::mat4 m = glm::mat4_cast(r);
    m[0] *= s.x;
    m[1] *= s.y;
    m[2] *= s.z;
    m[3] = glm::vec4(t, 1.0f);
    return m;
}

glm::quat rotationOf(const glm::mat4& m) {
    return glm::normalize(glm::quat_cast(
        glm::mat3(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])))));
}

// The shortest rotation that turns unit vector a onto unit vector b.
glm::quat arc(const glm::vec3& a, const glm::vec3& b) {
    const float c = glm::dot(a, b);
    if (c < -0.999999f) {
        const glm::vec3 axis =
            std::abs(a.x) < 0.9f ? glm::cross(a, glm::vec3(1, 0, 0)) : glm::cross(a, glm::vec3(0, 1, 0));
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 v = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + c, v.x, v.y, v.z));
}

std::vector<glm::mat4> restModel(const SkeletonDesc& desc) {
    std::vector<glm::mat4> out(desc.joints.size());
    for (std::size_t j = 0; j < desc.joints.size(); ++j) {
        const auto& rest = desc.joints[j].rest;
        const glm::mat4 m = local(rest.translation, rest.rotation, rest.scale);
        out[j] = desc.joints[j].parent >= 0 ? out[std::size_t(desc.joints[j].parent)] * m : m;
    }
    return out;
}

int findJoint(const SkeletonDesc& desc, std::string_view name) {
    for (std::size_t j = 0; j < desc.joints.size(); ++j)
        if (desc.joints[j].name == name) return int(j);
    return -1;
}

} // namespace

Result<RetargetMap> parseRetargetMap(std::string_view text) {
    Result<json::Value> doc = json::parse(std::string(text));
    if (!doc) return Error{doc.error()};
    const json::Value& root = *doc;
    if (!root.isObject()) return Error{"not an object"};
    static const std::set<std::string, std::less<>> kKeys = {"joints", "translations", "scale"};
    for (const auto& [key, value] : root.object)
        if (!kKeys.contains(key)) return Error{std::format("unknown key '{}'", key)};
    RetargetMap map;
    if (!root["joints"].isObject() || root["joints"].object.empty())
        return Error{"joints is an object of target joint: source joint"};
    for (const auto& [target, source] : root["joints"].object) {
        if (source.type != json::Value::Type::String || source.string.empty())
            return Error{std::format("joint '{}': its source is a joint name", target)};
        map.joints.emplace_back(target, source.string);
    }
    if (root.has("translations")) {
        if (!root["translations"].isArray()) return Error{"translations is a list of target joints"};
        for (const json::Value& t : root["translations"].array) {
            if (t.type != json::Value::Type::String) return Error{"translations is a list of target joints"};
            if (std::ranges::find(map.joints, t.string, &std::pair<std::string, std::string>::first) ==
                map.joints.end())
                return Error{std::format("translation joint '{}' is not in joints", t.string)};
            map.translations.push_back(t.string);
        }
    }
    if (root.has("scale")) {
        const json::Value& s = root["scale"];
        if (s.type == json::Value::Type::Number && s.number > 0.0) {
            map.scale = float(s.number);
        } else if (s.type == json::Value::Type::String &&
                   std::ranges::find(map.joints, s.string, &std::pair<std::string, std::string>::first) !=
                       map.joints.end()) {
            map.scaleJoint = s.string;
        } else {
            return Error{"scale is a positive number or a joint of joints"};
        }
    }
    return map;
}

Result<std::vector<ClipDesc>> retargetClips(const SkeletonDesc& source, const SkeletonDesc& target,
                                            std::span<const ClipDesc> clips, const RetargetMap& map,
                                            std::vector<std::string>* notes) {
    const std::size_t nt = target.joints.size();
    std::vector<int> sourceOf(nt, -1);
    for (const auto& [t, s] : map.joints) {
        const int tj = findJoint(target, t), sj = findJoint(source, s);
        if (tj < 0) return Error{std::format("the target skeleton has no joint '{}'", t)};
        if (sj < 0) return Error{std::format("the source skeleton has no joint '{}' (mapped to '{}')", s, t)};
        sourceOf[std::size_t(tj)] = sj;
    }
    std::vector<char> translated(nt, 0);
    for (const std::string& t : map.translations) translated[std::size_t(findJoint(target, t))] = 1;

    const std::vector<glm::mat4> sourceRest = restModel(source), targetRest = restModel(target);
    const auto height = [](const glm::mat4& m) { return m[3].y; };
    float scale = map.scale;
    if (scale <= 0.0f) {
        std::string joint = map.scaleJoint;
        if (joint.empty())
            for (const std::string& t : map.translations) {
                const int tj = findJoint(target, t);
                if (std::abs(height(targetRest[std::size_t(tj)])) > 1e-3f &&
                    std::abs(height(sourceRest[std::size_t(sourceOf[std::size_t(tj)])])) > 1e-3f) {
                    joint = t;
                    break;
                }
            }
        scale = 1.0f;
        if (!joint.empty()) {
            const int tj = findJoint(target, joint);
            const float from = height(sourceRest[std::size_t(sourceOf[std::size_t(tj)])]);
            if (std::abs(from) < 1e-3f)
                return Error{std::format("scale joint '{}': its source is at height 0 at rest", joint)};
            scale = height(targetRest[std::size_t(tj)]) / from;
        }
    }

    // Per target joint the rotation (model space) that turns its rest bone onto the source's: the bone runs to the
    // one mapped joint below it (through unmapped ones). A joint with several (a hip, a chest) is not turned; one
    // with none (a hand, a head) turns as its nearest mapped ancestor.
    std::vector<glm::quat> align(nt, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    std::vector<std::vector<int>> below(nt); // the first mapped joints below each joint
    for (std::size_t j = nt; j-- > 0;) {
        const int p = target.joints[j].parent;
        if (p < 0) continue;
        if (sourceOf[j] >= 0)
            below[std::size_t(p)].push_back(int(j));
        else
            below[std::size_t(p)].insert(below[std::size_t(p)].end(), below[j].begin(), below[j].end());
    }
    for (std::size_t j = 0; j < nt; ++j) {
        if (sourceOf[j] < 0 || below[j].size() != 1) continue;
        const std::size_t k = std::size_t(below[j][0]);
        const glm::vec3 dt = glm::vec3(targetRest[k][3] - targetRest[j][3]);
        const glm::vec3 ds =
            glm::vec3(sourceRest[std::size_t(sourceOf[k])][3] - sourceRest[std::size_t(sourceOf[j])][3]);
        if (glm::length(dt) < 1e-6f || glm::length(ds) < 1e-6f) continue;
        align[j] = arc(glm::normalize(dt), glm::normalize(ds));
    }
    for (std::size_t j = 0; j < nt; ++j) {
        if (sourceOf[j] < 0 || !below[j].empty()) continue;
        for (int a = target.joints[j].parent; a >= 0; a = target.joints[std::size_t(a)].parent)
            if (sourceOf[std::size_t(a)] >= 0) {
                align[j] = align[std::size_t(a)];
                break;
            }
    }
    // The constant part of each mapped joint's model rotation: source rest inverted, the alignment, target rest.
    std::vector<glm::quat> offset(nt);
    for (std::size_t j = 0; j < nt; ++j)
        if (sourceOf[j] >= 0)
            offset[j] =
                glm::inverse(rotationOf(sourceRest[std::size_t(sourceOf[j])])) * align[j] * rotationOf(targetRest[j]);

    if (notes) {
        std::string unmapped;
        for (std::size_t j = 0; j < nt; ++j)
            if (sourceOf[j] < 0) unmapped += (unmapped.empty() ? "" : ", ") + target.joints[j].name;
        if (!unmapped.empty()) notes->push_back(std::format("retarget: not mapped, at their rest pose: {}", unmapped));
    }

    std::vector<ClipDesc> out;
    const std::size_t ns = source.joints.size();
    for (const ClipDesc& clip : clips) {
        if (clip.tracks.size() != ns)
            return Error{std::format("clip '{}': {} tracks for {} joints", clip.name, clip.tracks.size(), ns)};
        std::set<float> times{0.0f, clip.duration};
        const int grid = std::max(1, int(std::ceil(clip.duration * kGridRate)));
        for (int i = 1; i < grid; ++i) times.insert(clip.duration * float(i) / float(grid));
        for (std::size_t j = 0; j < ns; ++j) {
            const JointTrack& track = clip.tracks[j];
            for (const auto& k : track.translations) times.insert(k.time);
            for (const auto& k : track.rotations) times.insert(k.time);
            for (const auto& k : track.scales) times.insert(k.time);
            if (notes && !track.scales.empty() && std::ranges::find(sourceOf, int(j)) != sourceOf.end())
                notes->push_back(std::format("retarget: clip '{}': the scale keys of '{}' are not retargeted",
                                             clip.name, source.joints[j].name));
        }

        ClipDesc result;
        result.name = clip.name;
        result.duration = clip.duration;
        result.events = clip.events;
        result.loop = clip.loop;
        result.additive = clip.additive;
        if (clip.rootMotion) {
            const int sj = findJoint(source, clip.rootMotion->joint);
            const auto mapped = std::ranges::find(sourceOf, sj);
            if (sj < 0 || mapped == sourceOf.end())
                return Error{std::format("clip '{}': its root motion joint '{}' is not mapped", clip.name,
                                         clip.rootMotion->joint)};
            result.rootMotion = animation::RootMotionDesc{target.joints[std::size_t(mapped - sourceOf.begin())].name,
                                                          clip.rootMotion->rotation};
        }
        result.tracks.resize(nt);

        std::vector<glm::mat4> sourceModel(ns), targetModel(nt);
        for (const float time : times) {
            if (time < 0.0f || time > clip.duration) continue;
            for (std::size_t j = 0; j < ns; ++j) {
                const auto& rest = source.joints[j].rest;
                const JointTrack& track = clip.tracks[j];
                const glm::mat4 m =
                    local(sample(track.translations, time, rest.translation),
                          sample(track.rotations, time, rest.rotation), sample(track.scales, time, rest.scale));
                sourceModel[j] =
                    source.joints[j].parent >= 0 ? sourceModel[std::size_t(source.joints[j].parent)] * m : m;
            }
            for (std::size_t j = 0; j < nt; ++j) {
                const auto& rest = target.joints[j].rest;
                const int p = target.joints[j].parent;
                const glm::mat4 parent = p >= 0 ? targetModel[std::size_t(p)] : glm::mat4(1.0f);
                glm::quat rotation = rest.rotation;
                glm::vec3 translation = rest.translation;
                if (sourceOf[j] >= 0) {
                    const std::size_t s = std::size_t(sourceOf[j]);
                    const glm::quat model = rotationOf(sourceModel[s]) * offset[j];
                    rotation =
                        glm::normalize((p >= 0 ? glm::inverse(rotationOf(parent)) : glm::quat(1, 0, 0, 0)) * model);
                    if (translated[j]) {
                        const glm::vec3 at =
                            glm::vec3(targetRest[j][3]) + scale * glm::vec3(sourceModel[s][3] - sourceRest[s][3]);
                        translation = glm::vec3(glm::inverse(parent) * glm::vec4(at, 1.0f));
                    }
                    JointTrack& track = result.tracks[j];
                    if (!track.rotations.empty() && glm::dot(track.rotations.back().value, rotation) < 0.0f)
                        rotation = -rotation;
                    track.rotations.push_back({time, rotation});
                    if (translated[j]) track.translations.push_back({time, translation});
                }
                targetModel[j] = parent * local(translation, rotation, rest.scale);
            }
        }
        out.push_back(std::move(result));
    }
    return out;
}

} // namespace quench::cook
