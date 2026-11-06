#pragma once

// Animation clips: keyframes for every joint of one skeleton, compressed by ozz-animation (keys removed within a
// tolerance, quantized), with named events on their timeline and optional root motion. A clip set holds the clips of
// one source file (a cooked .qanim) and the signature of the skeleton they were made for.

#include <quench/animation/Skeleton.h>
#include <quench/core/Result.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::animation {

namespace detail {
struct ClipImpl;
}

// A marker on a clip's timeline, delivered to the game when playback passes it (AnimationSystem::events()).
struct ClipEvent {
    float time = 0.0f; // seconds from the clip's start
    std::string name;
    float value = 0.0f; // free for the game (a foot index, an intensity)
};

template <class T>
struct Key {
    float time = 0.0f; // seconds
    T value{};
};

// Keys of one joint; empty channels keep the joint's rest value. Linear interpolation between keys.
struct JointTrack {
    std::vector<Key<glm::vec3>> translations;
    std::vector<Key<glm::quat>> rotations;
    std::vector<Key<glm::vec3>> scales;
};

struct RootMotionDesc {
    std::string joint;     // the joint whose horizontal motion becomes root motion (its parent at the model origin)
    bool rotation = false; // also its rotation about Y (turns)
};

struct ClipDesc {
    std::string name;
    float duration = 0.0f;          // seconds, > 0
    std::vector<JointTrack> tracks; // per joint of the description the skeleton was built from (SkeletonDesc order)
    std::vector<ClipEvent> events;  // any order; times within [0, duration]
    bool loop = true;               // plays on from the start after the end
    bool additive = false;          // stored as the difference to its first frame, for additive layers
    std::optional<RootMotionDesc> rootMotion;
};

struct ClipBuildOptions {
    float tolerance = 0.001f; // largest error of the key reduction at `distance` from a joint (m)
    float distance = 0.1f;    // m
    bool optimize = true;     // false: every key is kept
};

class Clip {
public:
    explicit Clip(std::unique_ptr<detail::ClipImpl> impl);
    ~Clip();
    Clip(Clip&&) noexcept;
    Clip& operator=(Clip&&) noexcept;

    const std::string& name() const;
    float duration() const;
    bool loops() const;
    bool additive() const;
    bool hasRootMotion() const;
    // Sorted by time.
    std::span<const ClipEvent> events() const;
    std::uint64_t memoryBytes() const;

    const detail::ClipImpl& impl() const { return *m_impl; }

private:
    std::unique_ptr<detail::ClipImpl> m_impl;
};

class ClipSet {
public:
    ClipSet(std::vector<Clip> clips, int jointCount, std::uint64_t skeletonSignature);

    std::span<const Clip> clips() const { return m_clips; }
    // nullptr when there is no clip of that name.
    const Clip* find(std::string_view name) const;
    int jointCount() const { return m_jointCount; }
    std::uint64_t skeletonSignature() const { return m_signature; }
    std::uint64_t memoryBytes() const;

private:
    std::vector<Clip> m_clips;
    int m_jointCount = 0;
    std::uint64_t m_signature = 0;
};

// The description's joint order is mapped to the skeleton's by joint name.
Result<std::shared_ptr<const ClipSet>> buildClipSet(const Skeleton& skeleton, const SkeletonDesc& skeletonDesc,
                                                    std::span<const ClipDesc> clips,
                                                    const ClipBuildOptions& options = {});

// .qanim files: a header, a checksum, the clips' names, flags and events and their ozz-animation archives.
Result<std::shared_ptr<const ClipSet>> readClipSet(std::span<const std::uint8_t> file);
std::vector<std::uint8_t> writeClipSet(const ClipSet& clips);

} // namespace quench::animation
