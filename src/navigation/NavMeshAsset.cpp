#include <quench/navigation/NavMeshAsset.h>

namespace quench::navigation {

Result<void> NavMeshAsset::decode(fs::Bytes bytes) {
    Result<std::shared_ptr<const NavMeshData>> data = readNavMeshData(bytes);
    if (!data) return Error{data.error()};
    m_pending = std::move(*data);
    return {};
}

Result<void> NavMeshAsset::finish() {
    m_data = std::move(m_pending);
    return {};
}

void registerNavMeshAsset(assets::AssetManager& manager, assets::AssetMemory budget) {
    manager.registerType<NavMeshAsset>(
        "navmesh", [](std::string_view) { return std::unique_ptr<NavMeshAsset>(new NavMeshAsset()); }, budget);
}

void unregisterNavMeshAsset(assets::AssetManager& manager) { manager.unregisterType<NavMeshAsset>(); }

} // namespace quench::navigation
