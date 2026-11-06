// Foot placement (two-bone IK per leg after moving the pelvis to the lower foot's ground) and look-at (aim IK down
// a chain). The look-at chain follows ozz-animation's look-at sample (samples/look_at/sample_look_at.cc, MIT,
// Copyright (c) 2020 Guillaume Blanc): corrected forward and offset vectors are carried from the head down the chain,
// and the last joint gets the full weight.

#include <quench/animation/Ik.h>

#include "Character.h"

#include <quench/core/Assert.h>

#include <ozz/animation/runtime/ik_aim_job.h>
#include <ozz/animation/runtime/ik_two_bone_job.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>

namespace quench::animation::detail {

namespace {

using Character = AnimationSystem::Character;

glm::quat rotationOf(const glm::mat4& m) {
    const glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])),
                      glm::normalize(glm::vec3(m[2])));
    return glm::normalize(glm::quat_cast(r));
}

// Right-multiplies a joint's local rotation (ozz's IK corrections are in the joint's local space).
void correctJoint(Character& c, int joint, const glm::quat& correction) {
    Transform t = getJoint(ozz::make_span(c.pose), joint);
    t.rotation = glm::normalize(t.rotation * correction);
    setJoint(ozz::make_span(c.pose), joint, t);
}

float follow(float current, float target, float rate, float dt, bool snap) {
    return snap ? target : current + (target - current) * (1.0f - std::exp(-rate * dt));
}

// The shortest rotation from unit vector a to unit vector b.
glm::quat rotationBetween(const glm::vec3& a, const glm::vec3& b) {
    const float d = glm::dot(a, b);
    if (d < -0.9999f) {
        const glm::vec3 side = std::abs(a.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        return glm::angleAxis(3.14159265f, glm::normalize(glm::cross(a, side)));
    }
    const glm::vec3 axis = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + d, axis.x, axis.y, axis.z));
}

} // namespace

} // namespace quench::animation::detail

namespace quench::animation {

TwoBoneResult solveTwoBone(const glm::mat4& start, const glm::mat4& mid, const glm::mat4& end, const TwoBoneIk& ik) {
    const ozz::math::Float4x4 s = detail::toOzz(start), m = detail::toOzz(mid), e = detail::toOzz(end);
    ozz::math::SimdQuaternion startCorrection = ozz::math::SimdQuaternion::identity();
    ozz::math::SimdQuaternion midCorrection = ozz::math::SimdQuaternion::identity();
    bool reached = false;
    ozz::animation::IKTwoBoneJob job;
    job.target = detail::toOzz(ik.target, 1.0f);
    job.pole_vector = detail::toOzz(ik.pole, 0.0f);
    job.mid_axis = detail::toOzz(ik.midAxis, 0.0f);
    job.weight = std::clamp(ik.weight, 0.0f, 1.0f);
    job.soften = std::clamp(ik.soften, 0.0f, 1.0f);
    job.twist_angle = ik.twist;
    job.start_joint = &s;
    job.mid_joint = &m;
    job.end_joint = &e;
    job.start_joint_correction = &startCorrection;
    job.mid_joint_correction = &midCorrection;
    job.reached = &reached;
    [[maybe_unused]] const bool ok = job.Run();
    QUENCH_ASSERT(ok, "two-bone IK job");
    return {detail::toGlm(startCorrection), detail::toGlm(midCorrection), reached};
}

AimResult solveAim(const glm::mat4& joint, const AimIk& ik) {
    const ozz::math::Float4x4 j = detail::toOzz(joint);
    ozz::math::SimdQuaternion correction = ozz::math::SimdQuaternion::identity();
    bool reached = false;
    ozz::animation::IKAimJob job;
    job.target = detail::toOzz(ik.target, 1.0f);
    job.forward = detail::toOzz(ik.forward, 0.0f);
    job.up = detail::toOzz(ik.up, 0.0f);
    job.offset = detail::toOzz(ik.offset, 1.0f);
    job.pole_vector = detail::toOzz(ik.pole, 0.0f);
    job.weight = std::clamp(ik.weight, 0.0f, 1.0f);
    job.joint = &j;
    job.joint_correction = &correction;
    job.reached = &reached;
    [[maybe_unused]] const bool ok = job.Run();
    QUENCH_ASSERT(ok, "aim IK job");
    return {detail::toGlm(correction), reached};
}

} // namespace quench::animation

namespace quench::animation::detail {

void placeFeet(Character& c, float dt, const GroundQuery& ground) {
    const FootPlacementDesc& desc = *c.desc.feet;
    const glm::mat4& world = c.desc.transform;
    const glm::mat3 toModel = glm::inverse(glm::mat3(world));
    const float baseY = world[3].y;
    const bool snap = !c.groundSnapped;
    c.groundSnapped = true;

    std::vector<glm::vec3> ankles(c.legs.size());
    std::vector<glm::vec3> normals(c.legs.size(), glm::vec3(0.0f, 1.0f, 0.0f));
    float lowest = 1e9f;
    for (std::size_t i = 0; i < c.legs.size(); ++i) {
        Character::Leg& leg = c.legs[i];
        ankles[i] = toVec3(c.models[std::size_t(leg.ankle)].cols[3]);
        const glm::vec3 ankle = glm::vec3(world * glm::vec4(ankles[i], 1.0f));
        const glm::vec3 from(ankle.x, baseY + desc.rayHeight, ankle.z);
        ++c.groundQueries;
        float target = 0.0f;
        leg.state.grounded = false;
        if (const std::optional<GroundHit> hit = ground(from, desc.rayHeight + desc.maxStepDown)) {
            const float offset = hit->position.y - baseY;
            if (offset <= desc.maxStepUp && offset >= -desc.maxStepDown) {
                target = offset;
                leg.state.grounded = true;
                const float length = glm::length(hit->normal);
                if (length > 1e-6f) normals[i] = glm::normalize(toModel * (hit->normal / length));
            }
        }
        leg.offset = follow(leg.offset, target, desc.smoothing, dt, snap);
        leg.state.offset = leg.offset;
        lowest = std::min(lowest, leg.offset);
    }
    if (c.legs.empty()) return;
    c.pelvisOffset = follow(c.pelvisOffset, lowest, desc.smoothing, dt, snap);

    const glm::vec3 up = toModel * glm::vec3(0.0f, 1.0f, 0.0f);
    if (c.pelvis >= 0 && std::abs(c.pelvisOffset) > 1e-5f) {
        const int parent = c.skeleton->parent(c.pelvis);
        const glm::mat3 parentToModel = parent >= 0 ? glm::mat3(toGlm(c.models[std::size_t(parent)])) : glm::mat3(1.0f);
        Transform t = getJoint(ozz::make_span(c.pose), c.pelvis);
        t.translation += glm::inverse(parentToModel) * (up * c.pelvisOffset);
        setJoint(ozz::make_span(c.pose), c.pelvis, t);
        updateModels(c, c.pelvis);
    }

    for (std::size_t i = 0; i < c.legs.size(); ++i) {
        Character::Leg& leg = c.legs[i];
        const glm::vec3 target = ankles[i] + up * leg.offset;
        const glm::quat animatedFoot = rotationOf(toGlm(c.models[std::size_t(leg.ankle)]));
        const glm::vec3 hip = toVec3(c.models[std::size_t(leg.hip)].cols[3]);
        const glm::vec3 knee = toVec3(c.models[std::size_t(leg.knee)].cols[3]);
        const glm::vec3 ankle = toVec3(c.models[std::size_t(leg.ankle)].cols[3]);
        // The pole keeps the knee on the side the animation bends it to; a straight leg bends forward.
        glm::vec3 pole = glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 axis = ankle - hip;
        const float axisLength = glm::length(axis);
        if (axisLength > 1e-5f) {
            const glm::vec3 d = axis / axisLength;
            const glm::vec3 side = (knee - hip) - d * glm::dot(knee - hip, d);
            if (glm::length(side) > 1e-3f) pole = glm::normalize(side);
        }
        TwoBoneIk ik;
        ik.target = target;
        ik.pole = pole;
        ik.midAxis = glm::normalize(leg.kneeAxis);
        ik.soften = desc.soften;
        const TwoBoneResult solved =
            solveTwoBone(toGlm(c.models[std::size_t(leg.hip)]), toGlm(c.models[std::size_t(leg.knee)]),
                         toGlm(c.models[std::size_t(leg.ankle)]), ik);
        correctJoint(c, leg.hip, solved.start);
        correctJoint(c, leg.knee, solved.mid);
        updateModels(c, leg.hip);
        leg.state.reached = solved.reached;

        // The foot keeps its animated orientation in model space (the leg's corrections turned it), tilted to the
        // ground's slope.
        glm::quat tilt(1.0f, 0.0f, 0.0f, 0.0f);
        if (desc.alignToGround > 0.0f && leg.state.grounded)
            tilt =
                glm::slerp(tilt, rotationBetween(glm::normalize(up), normals[i]), std::min(desc.alignToGround, 1.0f));
        const glm::quat solvedFoot = rotationOf(toGlm(c.models[std::size_t(leg.ankle)]));
        correctJoint(c, leg.ankle, glm::inverse(solvedFoot) * tilt * animatedFoot);
        updateModels(c, leg.ankle);
    }
}

void lookAt(Character& c, float dt) {
    const LookAtDesc& desc = *c.desc.lookAt;
    const glm::mat4 toModel = glm::inverse(c.desc.transform);
    const Character::LookChainJoint& head = c.lookChain[0];
    const glm::mat4 headModel = toGlm(c.models[std::size_t(head.joint)]);
    // A target outside maxAngle (or none) is not followed: the turn fades out toward the last target followed.
    float wanted = 0.0f;
    if (c.lookTarget) {
        const glm::vec3 target = glm::vec3(toModel * glm::vec4(*c.lookTarget, 1.0f));
        const glm::vec3 eyes = glm::vec3(headModel * glm::vec4(head.offset, 1.0f));
        const glm::vec3 to = target - eyes;
        const float length = glm::length(to);
        const float angle =
            length > 1e-4f ? std::acos(std::clamp(glm::dot(to / length, glm::normalize(desc.forward)), -1.0f, 1.0f))
                           : 0.0f;
        if (angle <= desc.maxAngle) {
            wanted = std::clamp(c.lookWeight, 0.0f, 1.0f);
            c.lastLookTarget = target;
        }
    }
    c.lookBlend = follow(c.lookBlend, wanted, 6.0f, dt, false);
    c.lookReached = false;
    if (c.lookBlend < 1e-3f) return;
    const glm::vec3 target = c.lastLookTarget;

    ozz::animation::IKAimJob job;
    job.pole_vector = toOzz(glm::vec3(0.0f, 1.0f, 0.0f), 0.0f);
    job.target = toOzz(target, 1.0f);
    ozz::math::SimdQuaternion correction = ozz::math::SimdQuaternion::identity();
    job.joint_correction = &correction;
    bool reached = false;
    job.reached = &reached;
    int previous = -1;
    for (std::size_t i = 0; i < c.lookChain.size(); ++i) {
        const Character::LookChainJoint& link = c.lookChain[i];
        job.joint = &c.models[std::size_t(link.joint)];
        job.up = toOzz(link.up, 0.0f);
        const bool last = i + 1 == c.lookChain.size();
        job.weight = c.lookBlend * (last ? 1.0f : desc.chainWeight);
        if (i == 0) {
            job.offset = toOzz(link.offset, 1.0f);
            job.forward = toOzz(link.forward, 0.0f);
        } else {
            const ozz::math::SimdFloat4 forward = ozz::math::TransformVector(
                c.models[std::size_t(previous)], ozz::math::TransformVector(correction, job.forward));
            const ozz::math::SimdFloat4 offset = ozz::math::TransformPoint(
                c.models[std::size_t(previous)], ozz::math::TransformVector(correction, job.offset));
            const ozz::math::Float4x4 inverse = ozz::math::Invert(c.models[std::size_t(link.joint)]);
            job.forward = ozz::math::TransformVector(inverse, forward);
            job.offset = ozz::math::TransformPoint(inverse, offset);
        }
        [[maybe_unused]] const bool ok = job.Run();
        QUENCH_ASSERT(ok, "aim IK job");
        correctJoint(c, link.joint, toGlm(correction));
        previous = link.joint;
    }
    c.lookReached = reached;
    updateModels(c, previous);
}

} // namespace quench::animation::detail
