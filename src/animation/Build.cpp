// Skeletons and clips from descriptions, with ozz-animation's offline library (raw data, builders, optimizer).

#include <quench/animation/Clip.h>
#include <quench/animation/Skeleton.h>

#include "OzzImpl.h"

#include <ozz/animation/offline/additive_animation_builder.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/animation_optimizer.h>
#include <ozz/animation/offline/motion_extractor.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/raw_track.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/offline/track_builder.h>

#include <algorithm>
#include <format>
#include <map>

namespace quench::animation {

namespace {

namespace offline = ozz::animation::offline;

ozz::math::Float3 toFloat3(const glm::vec3& v) { return {v.x, v.y, v.z}; }
ozz::math::Quaternion toQuaternion(const glm::quat& q) {
    const glm::quat n = glm::normalize(q);
    return {n.x, n.y, n.z, n.w};
}

ozz::math::Transform toOzzTransform(const Transform& t) {
    return {toFloat3(t.translation), toQuaternion(t.rotation), toFloat3(t.scale)};
}

void addJoint(const SkeletonDesc& desc, const std::vector<std::vector<int>>& children, int index,
              offline::RawSkeleton::Joint& out) {
    const SkeletonDesc::Joint& joint = desc.joints[std::size_t(index)];
    out.name = joint.name.c_str();
    out.transform = toOzzTransform(joint.rest);
    out.children.resize(children[std::size_t(index)].size());
    for (std::size_t c = 0; c < out.children.size(); ++c)
        addJoint(desc, children, children[std::size_t(index)][c], out.children[c]);
}

// Sorted, clamped to [0, duration], one key per time (the last given wins); an empty channel gets the rest value.
template <class Out, class In, class Convert>
void addKeys(const std::vector<Key<In>>& keys, float duration, const In& rest, Convert convert, ozz::vector<Out>& out) {
    std::vector<Key<In>> sorted(keys);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Key<In>& a, const Key<In>& b) { return a.time < b.time; });
    for (const Key<In>& k : sorted) {
        const float time = std::clamp(k.time, 0.0f, duration);
        if (!out.empty() && out.back().time >= time)
            out.back().value = convert(k.value);
        else
            out.push_back({time, convert(k.value)});
    }
    if (out.empty()) out.push_back({0.0f, convert(rest)});
}

} // namespace

Result<std::shared_ptr<const Skeleton>> buildSkeleton(const SkeletonDesc& desc) {
    detail::quietOzz();
    const std::size_t count = desc.joints.size();
    if (count == 0 || count > std::size_t(kMaxJoints))
        return Error{std::format("skeleton: {} joints (1 to {})", count, kMaxJoints)};
    std::vector<std::vector<int>> children(count);
    std::vector<int> roots;
    std::map<std::string, int, std::less<>> names;
    for (std::size_t i = 0; i < count; ++i) {
        const SkeletonDesc::Joint& joint = desc.joints[i];
        if (joint.name.empty()) return Error{std::format("skeleton: joint {} has no name", i)};
        if (!names.emplace(joint.name, int(i)).second)
            return Error{std::format("skeleton: two joints are named '{}'", joint.name)};
        if (joint.parent >= int(i))
            return Error{std::format("skeleton: joint '{}' comes before its parent", joint.name)};
        (joint.parent < 0 ? roots : children[std::size_t(joint.parent)]).push_back(int(i));
    }
    offline::RawSkeleton raw;
    raw.roots.resize(roots.size());
    for (std::size_t r = 0; r < roots.size(); ++r) addJoint(desc, children, roots[r], raw.roots[r]);
    if (!raw.Validate()) return Error{"skeleton: ozz-animation refuses the hierarchy"};
    ozz::unique_ptr<ozz::animation::Skeleton> built = offline::SkeletonBuilder()(raw);
    if (!built) return Error{"skeleton: ozz-animation could not build it"};
    auto impl = std::make_unique<detail::SkeletonImpl>();
    impl->skeleton = std::move(*built);
    detail::describeSkeleton(*impl);
    return std::shared_ptr<const Skeleton>(std::make_shared<Skeleton>(std::move(impl)));
}

Result<std::shared_ptr<const ClipSet>> buildClipSet(const Skeleton& skeleton, const SkeletonDesc& skeletonDesc,
                                                    std::span<const ClipDesc> clips, const ClipBuildOptions& options) {
    detail::quietOzz();
    const int joints = skeleton.jointCount();
    if (int(skeletonDesc.joints.size()) != joints)
        return Error{
            std::format("clips: the description has {} joints, the skeleton {}", skeletonDesc.joints.size(), joints)};
    std::vector<int> toSkeleton(skeletonDesc.joints.size());
    for (std::size_t i = 0; i < toSkeleton.size(); ++i) {
        toSkeleton[i] = skeleton.findJoint(skeletonDesc.joints[i].name);
        if (toSkeleton[i] < 0)
            return Error{std::format("clips: the skeleton has no joint '{}'", skeletonDesc.joints[i].name)};
    }
    const ozz::animation::Skeleton& ozzSkeleton = skeleton.impl().skeleton;

    std::vector<Clip> out;
    for (const ClipDesc& desc : clips) {
        const std::string what = std::format("clip '{}'", desc.name);
        if (desc.name.empty()) return Error{"clips: a clip has no name"};
        if (!(desc.duration > 0.0f)) return Error{std::format("{}: duration {}", what, desc.duration)};
        if (!desc.tracks.empty() && desc.tracks.size() != skeletonDesc.joints.size())
            return Error{std::format("{}: {} tracks for {} joints", what, desc.tracks.size(), joints)};
        if (std::ranges::any_of(out, [&](const Clip& c) { return c.name() == desc.name; }))
            return Error{std::format("clips: two clips are named '{}'", desc.name)};

        offline::RawAnimation raw;
        raw.name = desc.name.c_str();
        raw.duration = desc.duration;
        raw.tracks.resize(std::size_t(joints));
        for (std::size_t i = 0; i < skeletonDesc.joints.size(); ++i) {
            const Transform& rest = skeletonDesc.joints[i].rest;
            offline::RawAnimation::JointTrack& track = raw.tracks[std::size_t(toSkeleton[i])];
            static const JointTrack kEmpty;
            const JointTrack& in = desc.tracks.empty() ? kEmpty : desc.tracks[i];
            addKeys(in.translations, desc.duration, rest.translation, toFloat3, track.translations);
            addKeys(in.rotations, desc.duration, rest.rotation, toQuaternion, track.rotations);
            addKeys(in.scales, desc.duration, rest.scale, toFloat3, track.scales);
        }
        if (!raw.Validate()) return Error{std::format("{}: ozz-animation refuses the keys", what)};

        auto impl = std::make_unique<detail::ClipImpl>();
        impl->name = desc.name;
        impl->loop = desc.loop;
        impl->additive = desc.additive;
        if (desc.rootMotion) {
            const int joint = skeleton.findJoint(desc.rootMotion->joint);
            if (joint < 0) return Error{std::format("{}: no root motion joint '{}'", what, desc.rootMotion->joint)};
            offline::MotionExtractor extractor;
            extractor.root_joint = joint;
            if (!desc.rootMotion->rotation)
                extractor.rotation_settings = {false, false, false, offline::MotionExtractor::Reference::kSkeleton,
                                               false, false};
            offline::RawFloat3Track position;
            offline::RawQuaternionTrack rotation;
            offline::RawAnimation extracted;
            if (!extractor(raw, ozzSkeleton, &position, &rotation, &extracted))
                return Error{std::format("{}: root motion extraction failed", what)};
            ozz::unique_ptr<ozz::animation::Float3Track> p = offline::TrackBuilder()(position);
            ozz::unique_ptr<ozz::animation::QuaternionTrack> q = offline::TrackBuilder()(rotation);
            if (!p || !q) return Error{std::format("{}: root motion tracks could not be built", what)};
            impl->motionPosition = std::move(*p);
            impl->motionRotation = std::move(*q);
            impl->rootMotion = true;
            raw = std::move(extracted);
        }
        if (desc.additive) {
            offline::RawAnimation additive;
            if (!offline::AdditiveAnimationBuilder()(raw, &additive))
                return Error{std::format("{}: the additive clip could not be built", what)};
            raw = std::move(additive);
        }
        if (options.optimize) {
            offline::AnimationOptimizer optimizer;
            optimizer.setting = {options.tolerance, options.distance};
            offline::RawAnimation optimized;
            if (!optimizer(raw, ozzSkeleton, &optimized)) return Error{std::format("{}: optimization failed", what)};
            raw = std::move(optimized);
        }
        ozz::unique_ptr<ozz::animation::Animation> animation = offline::AnimationBuilder()(raw);
        if (!animation) return Error{std::format("{}: ozz-animation could not build it", what)};
        impl->animation = std::move(*animation);
        impl->events = desc.events;
        for (ClipEvent& e : impl->events) {
            if (e.name.empty()) return Error{std::format("{}: an event has no name", what)};
            e.time = std::clamp(e.time, 0.0f, desc.duration);
        }
        std::ranges::stable_sort(impl->events, {}, &ClipEvent::time);
        out.emplace_back(std::move(impl));
    }
    return std::shared_ptr<const ClipSet>(std::make_shared<ClipSet>(std::move(out), joints, skeleton.signature()));
}

} // namespace quench::animation
