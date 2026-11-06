#include <quench/animation/Clip.h>

#include "AnimationFile.h"
#include "OzzImpl.h"

#include <algorithm>
#include <format>

// .qanim payload: u32 joint count, u32 skeleton signature low, high, u32 clip count; per clip: string name, u32 flags
// (1 loop, 2 additive, 4 root motion), f32 duration, u32 event count, events (f32 time, f32 value, string name), blob
// ozz animation, with root motion also blob ozz position track and blob ozz rotation track.
namespace quench::animation {

namespace {

constexpr std::uint32_t kLoop = 1, kAdditive = 2, kRootMotion = 4;
constexpr std::uint32_t kMaxClips = 4096, kMaxEvents = 65536;

} // namespace

Clip::Clip(std::unique_ptr<detail::ClipImpl> impl) : m_impl(std::move(impl)) {}
Clip::~Clip() = default;
Clip::Clip(Clip&&) noexcept = default;
Clip& Clip::operator=(Clip&&) noexcept = default;

const std::string& Clip::name() const { return m_impl->name; }
float Clip::duration() const { return m_impl->animation.duration(); }
bool Clip::loops() const { return m_impl->loop; }
bool Clip::additive() const { return m_impl->additive; }
bool Clip::hasRootMotion() const { return m_impl->rootMotion; }
std::span<const ClipEvent> Clip::events() const { return m_impl->events; }

std::uint64_t Clip::memoryBytes() const {
    std::uint64_t bytes = sizeof(detail::ClipImpl) + m_impl->animation.size() + m_impl->name.size();
    for (const ClipEvent& e : m_impl->events) bytes += sizeof(ClipEvent) + e.name.size();
    if (m_impl->rootMotion) bytes += m_impl->motionPosition.size() + m_impl->motionRotation.size();
    return bytes;
}

ClipSet::ClipSet(std::vector<Clip> clips, int jointCount, std::uint64_t skeletonSignature)
    : m_clips(std::move(clips)), m_jointCount(jointCount), m_signature(skeletonSignature) {}

const Clip* ClipSet::find(std::string_view name) const {
    const auto it = std::ranges::find_if(m_clips, [name](const Clip& c) { return c.name() == name; });
    return it == m_clips.end() ? nullptr : &*it;
}

std::uint64_t ClipSet::memoryBytes() const {
    std::uint64_t bytes = sizeof(ClipSet);
    for (const Clip& c : m_clips) bytes += c.memoryBytes();
    return bytes;
}

std::vector<std::uint8_t> writeClipSet(const ClipSet& set) {
    detail::ByteWriter w;
    w.u32(std::uint32_t(set.jointCount()));
    w.u32(std::uint32_t(set.skeletonSignature()));
    w.u32(std::uint32_t(set.skeletonSignature() >> 32));
    w.u32(std::uint32_t(set.clips().size()));
    for (const Clip& clip : set.clips()) {
        const detail::ClipImpl& c = clip.impl();
        w.string(c.name);
        w.u32((c.loop ? kLoop : 0) | (c.additive ? kAdditive : 0) | (c.rootMotion ? kRootMotion : 0));
        w.f32(c.animation.duration());
        w.u32(std::uint32_t(c.events.size()));
        for (const ClipEvent& e : c.events) {
            w.f32(e.time);
            w.f32(e.value);
            w.string(e.name);
        }
        w.blob(detail::saveOzz(c.animation));
        if (c.rootMotion) {
            w.blob(detail::saveOzz(c.motionPosition));
            w.blob(detail::saveOzz(c.motionRotation));
        }
    }
    return detail::wrapPayload(detail::kClipSetMagic, w.out);
}

Result<std::shared_ptr<const ClipSet>> readClipSet(std::span<const std::uint8_t> file) {
    detail::quietOzz();
    Result<std::span<const std::uint8_t>> payload = detail::unwrapPayload(detail::kClipSetMagic, file, "clip set");
    if (!payload) return Error{payload.error()};
    detail::ByteReader r(*payload);
    std::uint32_t joints = 0, lo = 0, hi = 0, count = 0;
    if (!r.u32(joints) || !r.u32(lo) || !r.u32(hi) || !r.u32(count)) return Error{"truncated clip set"};
    if (joints == 0 || joints > std::uint32_t(kMaxJoints) || count > kMaxClips)
        return Error{std::format("corrupt clip set: {} joints, {} clips", joints, count)};
    std::vector<Clip> clips;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto c = std::make_unique<detail::ClipImpl>();
        std::uint32_t flags = 0, events = 0;
        float duration = 0.0f;
        if (!r.string(c->name) || !r.u32(flags) || !r.f32(duration) || !r.u32(events) || events > kMaxEvents)
            return Error{std::format("corrupt clip set: clip {}", i)};
        c->loop = (flags & kLoop) != 0;
        c->additive = (flags & kAdditive) != 0;
        c->rootMotion = (flags & kRootMotion) != 0;
        c->events.resize(events);
        for (ClipEvent& e : c->events)
            if (!r.f32(e.time) || !r.f32(e.value) || !r.string(e.name))
                return Error{std::format("corrupt clip set: events of '{}'", c->name)};
        std::span<const std::uint8_t> blob;
        if (!r.blob(blob) || !detail::loadOzz(blob, c->animation))
            return Error{std::format("corrupt clip set: animation '{}'", c->name)};
        if (c->animation.num_tracks() != int(joints) || c->animation.duration() != duration)
            return Error{std::format("corrupt clip set: '{}' has {} tracks for {} joints", c->name,
                                     c->animation.num_tracks(), joints)};
        if (c->rootMotion) {
            if (!r.blob(blob) || !detail::loadOzz(blob, c->motionPosition) || !r.blob(blob) ||
                !detail::loadOzz(blob, c->motionRotation))
                return Error{std::format("corrupt clip set: root motion of '{}'", c->name)};
        }
        clips.emplace_back(std::move(c));
    }
    if (r.remaining() != 0) return Error{std::format("corrupt clip set: {} bytes after the last clip", r.remaining())};
    return std::shared_ptr<const ClipSet>(
        std::make_shared<ClipSet>(std::move(clips), int(joints), (std::uint64_t(hi) << 32) | lo));
}

} // namespace quench::animation
