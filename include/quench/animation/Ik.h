#pragma once

// IK solvers on model-space joint matrices (ozz-animation's two-bone and aim IK). They return rotations to multiply
// onto the joints' local rotations (local = local * correction); the caller recomputes the model matrices.
// AnimationSystem uses the same solvers for foot placement and look-at; these are for a game's own IK (hands on a
// handle, a weapon aimed).

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace quench::animation {

struct TwoBoneIk {
    glm::vec3 target{0.0f};              // where the end joint should be (model space)
    glm::vec3 pole{0.0f, 0.0f, 1.0f};    // the direction the middle joint bends toward (model space)
    glm::vec3 midAxis{1.0f, 0.0f, 0.0f}; // the middle joint's bending axis in its own joint space
    float weight = 1.0f;                 // 0..1
    float soften = 1.0f;                 // 0..1: below 1 the chain straightens more gently near full reach
    float twist = 0.0f;                  // radians around the start-to-target axis
};

struct TwoBoneResult {
    glm::quat start{1.0f, 0.0f, 0.0f, 0.0f}; // correction of the start joint (hip, shoulder)
    glm::quat mid{1.0f, 0.0f, 0.0f, 0.0f};   // of the middle joint (knee, elbow)
    bool reached = false;                    // false: the target is out of reach; the chain points at it, stretched
};

TwoBoneResult solveTwoBone(const glm::mat4& start, const glm::mat4& mid, const glm::mat4& end, const TwoBoneIk& ik);

struct AimIk {
    glm::vec3 target{0.0f};              // model space
    glm::vec3 forward{0.0f, 0.0f, 1.0f}; // the joint's axis to aim, in its joint space (unit length)
    glm::vec3 up{0.0f, 1.0f, 0.0f};      // the joint's up axis, in its joint space (unit length)
    glm::vec3 offset{0.0f};              // the aiming point (the eyes, a muzzle) in joint space
    glm::vec3 pole{0.0f, 1.0f, 0.0f};    // where `up` should point (model space)
    float weight = 1.0f;
};

struct AimResult {
    glm::quat correction{1.0f, 0.0f, 0.0f, 0.0f};
    bool reached = false; // false: the target is too close to the offset to be aimed at exactly
};

AimResult solveAim(const glm::mat4& joint, const AimIk& ik);

} // namespace quench::animation
