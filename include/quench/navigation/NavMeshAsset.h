#pragma once

// Cooked navigation data as an asset type: .qnav files (quench_cook navmesh, quench_cook_assets(NAVMESHES)) read and
// checked on the asset manager's jobs. While loading and after a failure data() is empty, so a game has no mesh to
// make yet (its agents wait); a reload replaces the data and bumps Asset::version(), and the game makes a new NavMesh
// of it (the old one keeps the old data alive until it goes).
//
//   navigation::registerNavMeshAsset(app.assets());
//   m_level = app.assets().load<navigation::NavMeshAsset>("levels/yard.qnav");
//   ... once m_level->data() is set: NavMesh::create(m_level->data(), app.jobs())

#include <quench/assets/AssetManager.h>
#include <quench/navigation/NavMesh.h>

#include <memory>

namespace quench::navigation {

class NavMeshAsset final : public assets::Asset {
public:
    const std::shared_ptr<const NavMeshData>& data() const { return m_data; }

private:
    friend void registerNavMeshAsset(assets::AssetManager&, assets::AssetMemory);
    NavMeshAsset() = default;
    Result<void> decode(fs::Bytes bytes) override;
    std::uint64_t uploadBytes() const override { return 0; }
    Result<void> finish() override;
    void unload() override { m_data.reset(); }
    assets::AssetMemory residentMemory() const override { return {m_data ? m_data->memoryBytes() : 0, 0}; }

    std::shared_ptr<const NavMeshData> m_data, m_pending;
};

// Registers the type ("navmesh") with a manager; unregister it before the manager goes if the manager outlives what
// registered it. The budget limits what unreferenced navmeshes keep cached.
void registerNavMeshAsset(assets::AssetManager& manager, assets::AssetMemory budget = assets::AssetManager::kUnlimited);
void unregisterNavMeshAsset(assets::AssetManager& manager);

} // namespace quench::navigation
