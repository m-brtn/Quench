#pragma once

// The ozz-animation objects behind Skeleton and Clip, and conversions between ozz's SIMD and SoA types and GLM.

#include <quench/animation/Clip.h>
#include <quench/animation/Skeleton.h>

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/animation/runtime/track.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/simd_quaternion.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace quench::animation::detail {

struct SkeletonImpl {
    ozz::animation::Skeleton skeleton;
    std::vector<std::string> names;
    std::vector<Transform> rest;
    std::vector<glm::mat4> restModel;
    std::uint64_t signature = 0;
};

struct ClipImpl {
    ozz::animation::Animation animation;
    std::string name;
    std::vector<ClipEvent> events;
    bool loop = true;
    bool additive = false;
    bool rootMotion = false;
    ozz::animation::Float3Track motionPosition;     // by ratio, in the root motion joint's parent space
    ozz::animation::QuaternionTrack motionRotation; // identity keys without rotation extraction
    std::vector<std::string> channelNames;
    std::vector<ozz::animation::FloatTrack> channels; // by ratio; additive clips: the difference to the first value
};

// Fills names, rest and restModel from the ozz skeleton and computes the signature.
void describeSkeleton(SkeletonImpl& impl);

// ozz-animation writes its own errors to std::clog; the engine reports through quench::log instead. Called before
// every read and build (once per process).
void quietOzz();

inline glm::mat4 toGlm(const ozz::math::Float4x4& m) {
    glm::mat4 out;
    for (int c = 0; c < 4; ++c) ozz::math::StorePtrU(m.cols[c], &out[c][0]);
    return out;
}

inline ozz::math::Float4x4 toOzz(const glm::mat4& m) {
    ozz::math::Float4x4 out;
    for (int c = 0; c < 4; ++c) out.cols[c] = ozz::math::simd_float4::LoadPtrU(&m[c][0]);
    return out;
}

inline ozz::math::SimdFloat4 toOzz(const glm::vec3& v, float w) {
    return ozz::math::simd_float4::Load(v.x, v.y, v.z, w);
}

inline glm::vec3 toVec3(ozz::math::SimdFloat4 v) {
    float f[4];
    ozz::math::StorePtrU(v, f);
    return {f[0], f[1], f[2]};
}

inline glm::quat toGlm(const ozz::math::SimdQuaternion& q) {
    float f[4];
    ozz::math::StorePtrU(q.xyzw, f);
    return glm::quat(f[3], f[0], f[1], f[2]);
}

inline ozz::math::SimdQuaternion toOzz(const glm::quat& q) {
    return {ozz::math::simd_float4::Load(q.x, q.y, q.z, q.w)};
}

// One joint of an SoA pose (4 joints per SoaTransform).
Transform getJoint(ozz::span<const ozz::math::SoaTransform> pose, int joint);
void setJoint(ozz::span<ozz::math::SoaTransform> pose, int joint, const Transform& transform);

} // namespace quench::animation::detail
