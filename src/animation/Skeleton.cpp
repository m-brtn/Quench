#include <quench/animation/Skeleton.h>

#include "AnimationFile.h"
#include "OzzImpl.h"

#include <quench/core/Assert.h>

#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/log.h>
#include <ozz/base/maths/soa_float.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <format>
#include <mutex>

namespace quench::animation {

namespace detail {

namespace {

// The ten SoA lanes of a transform (translation xyz, rotation xyzw, scale xyz), four joints each.
struct Lanes {
    float v[10][4];
};

Lanes unpack(const ozz::math::SoaTransform& s) {
    Lanes l;
    const ozz::math::SimdFloat4 parts[10] = {s.translation.x, s.translation.y, s.translation.z, s.rotation.x,
                                             s.rotation.y,    s.rotation.z,    s.rotation.w,    s.scale.x,
                                             s.scale.y,       s.scale.z};
    for (int i = 0; i < 10; ++i) ozz::math::StorePtrU(parts[i], l.v[i]);
    return l;
}

} // namespace

Transform getJoint(ozz::span<const ozz::math::SoaTransform> pose, int joint) {
    QUENCH_ASSERT(joint >= 0 && std::size_t(joint) < pose.size() * 4, "joint out of the pose");
    const Lanes l = unpack(pose[std::size_t(joint) / 4]);
    const int i = joint & 3;
    Transform t;
    t.translation = {l.v[0][i], l.v[1][i], l.v[2][i]};
    t.rotation = glm::quat(l.v[6][i], l.v[3][i], l.v[4][i], l.v[5][i]);
    t.scale = {l.v[7][i], l.v[8][i], l.v[9][i]};
    return t;
}

void setJoint(ozz::span<ozz::math::SoaTransform> pose, int joint, const Transform& t) {
    QUENCH_ASSERT(joint >= 0 && std::size_t(joint) < pose.size() * 4, "joint out of the pose");
    ozz::math::SoaTransform& s = pose[std::size_t(joint) / 4];
    Lanes l = unpack(s);
    const int i = joint & 3;
    const float values[10] = {t.translation.x, t.translation.y, t.translation.z, t.rotation.x, t.rotation.y,
                              t.rotation.z,    t.rotation.w,    t.scale.x,       t.scale.y,    t.scale.z};
    for (int k = 0; k < 10; ++k) l.v[k][i] = values[k];
    using ozz::math::simd_float4::LoadPtrU;
    s.translation = {LoadPtrU(l.v[0]), LoadPtrU(l.v[1]), LoadPtrU(l.v[2])};
    s.rotation = {LoadPtrU(l.v[3]), LoadPtrU(l.v[4]), LoadPtrU(l.v[5]), LoadPtrU(l.v[6])};
    s.scale = {LoadPtrU(l.v[7]), LoadPtrU(l.v[8]), LoadPtrU(l.v[9])};
}

void quietOzz() {
    static std::once_flag once;
    std::call_once(once, [] { ozz::log::SetLevel(ozz::log::kSilent); });
}

void describeSkeleton(SkeletonImpl& impl) {
    const ozz::animation::Skeleton& s = impl.skeleton;
    const int count = s.num_joints();
    impl.names.assign(s.joint_names().begin(), s.joint_names().end());
    impl.rest.resize(std::size_t(count));
    for (int j = 0; j < count; ++j) impl.rest[std::size_t(j)] = getJoint(s.joint_rest_poses(), j);
    ozz::vector<ozz::math::Float4x4> models(static_cast<std::size_t>(count));
    ozz::animation::LocalToModelJob job;
    job.skeleton = &s;
    job.input = s.joint_rest_poses();
    job.output = ozz::make_span(models);
    [[maybe_unused]] const bool ok = job.Run();
    QUENCH_ASSERT(ok, "local-to-model of the rest pose");
    impl.restModel.resize(std::size_t(count));
    for (int j = 0; j < count; ++j) impl.restModel[std::size_t(j)] = toGlm(models[std::size_t(j)]);

    XXH64_state_t state;
    XXH64_reset(&state, 0);
    for (int j = 0; j < count; ++j) {
        const std::string& name = impl.names[std::size_t(j)];
        XXH64_update(&state, name.data(), name.size() + 1); // with the terminator: "ab","c" differs from "a","bc"
        const std::int16_t parent = s.joint_parents()[std::size_t(j)];
        XXH64_update(&state, &parent, sizeof(parent));
    }
    impl.signature = XXH64_digest(&state);
}

} // namespace detail

glm::mat4 Transform::matrix() const {
    return glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}

Skeleton::Skeleton(std::unique_ptr<detail::SkeletonImpl> impl) : m_impl(std::move(impl)) {}
Skeleton::~Skeleton() = default;

int Skeleton::jointCount() const { return m_impl->skeleton.num_joints(); }

std::string_view Skeleton::jointName(int joint) const {
    QUENCH_ASSERT(joint >= 0 && joint < jointCount(), "joint out of range");
    return m_impl->names[std::size_t(joint)];
}

int Skeleton::parent(int joint) const {
    QUENCH_ASSERT(joint >= 0 && joint < jointCount(), "joint out of range");
    return m_impl->skeleton.joint_parents()[std::size_t(joint)];
}

int Skeleton::findJoint(std::string_view name) const {
    const auto it = std::find(m_impl->names.begin(), m_impl->names.end(), name);
    return it == m_impl->names.end() ? -1 : int(it - m_impl->names.begin());
}

std::vector<int> Skeleton::subtree(int joint) const {
    QUENCH_ASSERT(joint >= 0 && joint < jointCount(), "joint out of range");
    // Depth-first order: the subtree is the joint and the joints after it until one whose parent lies before it.
    std::vector<int> out{joint};
    for (int j = joint + 1; j < jointCount(); ++j) {
        if (parent(j) < joint) break;
        out.push_back(j);
    }
    return out;
}

const Transform& Skeleton::restTransform(int joint) const {
    QUENCH_ASSERT(joint >= 0 && joint < jointCount(), "joint out of range");
    return m_impl->rest[std::size_t(joint)];
}

std::span<const glm::mat4> Skeleton::restModelMatrices() const { return m_impl->restModel; }

std::uint64_t Skeleton::signature() const { return m_impl->signature; }

std::uint64_t Skeleton::memoryBytes() const {
    std::uint64_t bytes = sizeof(detail::SkeletonImpl);
    const auto joints = std::uint64_t(jointCount());
    bytes += joints * (sizeof(Transform) + sizeof(glm::mat4) + sizeof(std::int16_t) + 2 * sizeof(std::string));
    bytes += std::uint64_t(m_impl->skeleton.num_soa_joints()) * sizeof(ozz::math::SoaTransform);
    for (const std::string& name : m_impl->names) bytes += 2 * (name.size() + 1);
    return bytes;
}

Result<std::shared_ptr<const Skeleton>> readSkeleton(std::span<const std::uint8_t> file) {
    detail::quietOzz();
    Result<detail::Payload> payload =
        detail::unwrapPayload(detail::kSkeletonMagic, detail::kSkeletonVersion, file, "skeleton");
    if (!payload) return Error{payload.error()};
    auto impl = std::make_unique<detail::SkeletonImpl>();
    if (!detail::loadOzz(payload->bytes, impl->skeleton))
        return Error{"corrupt skeleton: the ozz archive does not load"};
    if (impl->skeleton.num_joints() == 0) return Error{"skeleton without joints"};
    detail::describeSkeleton(*impl);
    return std::shared_ptr<const Skeleton>(std::make_shared<Skeleton>(std::move(impl)));
}

std::vector<std::uint8_t> writeSkeleton(const Skeleton& skeleton) {
    return detail::wrapPayload(detail::kSkeletonMagic, detail::kSkeletonVersion,
                               detail::saveOzz(skeleton.impl().skeleton));
}

} // namespace quench::animation
