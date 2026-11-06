#include <quench/animation/AnimationSystem.h>

namespace quench::animation {

Result<void> SkeletonAsset::decode(fs::Bytes bytes) {
    Result<std::shared_ptr<const Skeleton>> skeleton = readSkeleton(bytes);
    if (!skeleton) return Error{skeleton.error()};
    m_pending = std::move(*skeleton);
    return {};
}

Result<void> SkeletonAsset::finish() {
    m_data = std::move(m_pending);
    return {};
}

Result<void> ClipSetAsset::decode(fs::Bytes bytes) {
    Result<std::shared_ptr<const ClipSet>> clips = readClipSet(bytes);
    if (!clips) return Error{clips.error()};
    m_pending = std::move(*clips);
    return {};
}

Result<void> ClipSetAsset::finish() {
    m_data = std::move(m_pending);
    return {};
}

Result<void> GraphAsset::decode(fs::Bytes bytes) {
    Result<GraphDef> graph = parseGraph({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    if (!graph) return Error{graph.error()};
    m_pending = std::make_shared<const GraphDef>(std::move(*graph));
    m_pendingBytes = bytes.size() * 4; // the parsed definition, roughly
    return {};
}

Result<void> GraphAsset::finish() {
    m_data = std::move(m_pending);
    m_bytes = m_pendingBytes;
    return {};
}

} // namespace quench::animation
