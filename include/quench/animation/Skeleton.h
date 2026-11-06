#pragma once

// Skeletons of the animation module: a joint hierarchy with names and a rest pose. Built from a description (the
// cook, tests, games that make one at run time) or read from a cooked .qskel file; immutable afterwards and shared
// by every character that uses it. ozz-animation underneath (docs/animation.md); its types stay out of this header.

#include <quench/core/Result.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::animation {

namespace detail {
struct SkeletonImpl;
}

inline constexpr int kMaxJoints = 1024; // ozz-animation's limit

// A joint's transform relative to its parent (or to the model for a root).
struct Transform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z
    glm::vec3 scale{1.0f};

    glm::mat4 matrix() const;
};

struct SkeletonDesc {
    struct Joint {
        std::string name; // unique within the skeleton
        int parent = -1;  // index into joints, before this joint; -1: a root
        Transform rest;
    };
    std::vector<Joint> joints;
};

class Skeleton {
public:
    explicit Skeleton(std::unique_ptr<detail::SkeletonImpl> impl);
    ~Skeleton();
    Skeleton(const Skeleton&) = delete;
    Skeleton& operator=(const Skeleton&) = delete;

    // Joints are ordered depth first, parents before children (not necessarily the description's order).
    int jointCount() const;
    std::string_view jointName(int joint) const;
    int parent(int joint) const; // -1 for a root
    // -1 when there is no joint of that name.
    int findJoint(std::string_view name) const;
    // The joint and every joint below it.
    std::vector<int> subtree(int joint) const;
    const Transform& restTransform(int joint) const;
    // Rest pose in model space.
    std::span<const glm::mat4> restModelMatrices() const;
    // Of the names and parents: clips built for this skeleton carry it, and a clip set with another one is refused.
    std::uint64_t signature() const;
    // Bytes held by the skeleton.
    std::uint64_t memoryBytes() const;

    const detail::SkeletonImpl& impl() const { return *m_impl; }

private:
    std::unique_ptr<detail::SkeletonImpl> m_impl;
};

Result<std::shared_ptr<const Skeleton>> buildSkeleton(const SkeletonDesc& desc);

// .qskel files: a header, a checksum and the ozz-animation archive of the skeleton. Reading checks all three; a file
// changed after cooking is refused, not trusted.
Result<std::shared_ptr<const Skeleton>> readSkeleton(std::span<const std::uint8_t> file);
std::vector<std::uint8_t> writeSkeleton(const Skeleton& skeleton);

} // namespace quench::animation
