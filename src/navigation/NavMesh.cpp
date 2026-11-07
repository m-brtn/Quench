#include <quench/navigation/NavMesh.h>

#include "Internal.h"

#include <quench/core/Jobs.h>
#include <quench/core/Log.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace quench::navigation {

using detail::Config;
using detail::LayerBytes;
using detail::SourceGeometry;
using detail::TileData;

namespace {

using Clock = std::chrono::steady_clock;

struct NavMeshFree {
    void operator()(dtNavMesh* mesh) const { dtFreeNavMesh(mesh); }
};

// One tile column's build: inputs copied when it starts, results read by update() once its job is done.
struct TileWork {
    int tx = 0;
    int tz = 0;
    std::shared_ptr<const SourceGeometry> source; // voxelize first when set
    std::vector<LayerBytes> layers;               // otherwise build from these
    std::vector<Obstacle> obstacles;
    std::vector<Link> links;

    bool rasterized = false;
    std::vector<LayerBytes> newLayers;
    std::vector<TileData> tiles;
    std::string error;
};

void runTile(const Config& config, TileWork& work) {
    const std::vector<LayerBytes>* layers = &work.layers;
    if (work.source) {
        Result<std::vector<LayerBytes>> voxels = detail::rasterizeTile(config, *work.source, work.tx, work.tz);
        if (!voxels) {
            work.error = voxels.error();
            return;
        }
        work.newLayers = std::move(*voxels);
        work.rasterized = true;
        layers = &work.newLayers;
    }
    for (const LayerBytes& layer : *layers) {
        Result<TileData> tile = detail::buildTile(config, *layer, work.obstacles, work.links);
        if (!tile) {
            work.error = tile.error();
            continue;
        }
        if (tile->data) work.tiles.push_back(std::move(*tile));
    }
}

struct Tile {
    std::vector<LayerBytes> layers;
    std::vector<dtTileRef> refs;
    std::vector<std::uint64_t> hashes;
    std::uint64_t meshBytes = 0;
    int polygons = 0;
    std::vector<Link> links; // the links that start in this tile
    std::shared_ptr<const SourceGeometry> source;
    Job sourceReady;
    bool dirty = false;
    bool running = false;
    Job job;
    std::shared_ptr<TileWork> work;
};

struct ObstacleSlot {
    Obstacle obstacle;
    std::uint32_t generation = 0;
    bool used = false;
};

} // namespace

struct NavMesh::Impl {
    explicit Impl(JobSystem& jobSystem) : jobs(jobSystem) {}

    JobSystem& jobs;
    std::shared_ptr<const Config> config;
    std::unique_ptr<dtNavMesh, NavMeshFree> mesh;
    std::vector<Link> links;
    std::vector<Tile> tiles;
    std::vector<int> dirty;
    std::vector<int> running;
    std::vector<ObstacleSlot> obstacles;
    std::vector<std::uint32_t> freeObstacles;
    int obstacleCount = 0;
    std::uint64_t revision = 0;
    int polygons = 0;
    bool batchOpen = false;
    bool firstBatch = true;
    Clock::time_point batchStart;
    double lastBuildSeconds = 0.0;

    Result<void> init(Config c, std::vector<Link> meshLinks) {
        config = std::make_shared<const Config>(std::move(c));
        const Config& cfg = *config;
        mesh.reset(dtAllocNavMesh());
        dtNavMeshParams params{};
        params.orig[0] = cfg.settings.boundsMin.x;
        params.orig[1] = cfg.settings.boundsMin.y;
        params.orig[2] = cfg.settings.boundsMin.z;
        params.tileWidth = cfg.tileWorld;
        params.tileHeight = cfg.tileWorld;
        params.maxTiles = cfg.tilesX * cfg.tilesZ * cfg.settings.maxLayersPerTile;
        params.maxPolys = 1 << DT_POLY_BITS;
        if (!mesh || dtStatusFailed(mesh->init(&params)))
            return Error{std::format("cannot make a mesh of {} x {} tiles", cfg.tilesX, cfg.tilesZ)};
        tiles.resize(std::size_t(cfg.tilesX) * std::size_t(cfg.tilesZ));
        links = std::move(meshLinks);
        for (const Link& link : links) {
            const float fx = std::floor((link.start.x - cfg.settings.boundsMin.x) / cfg.tileWorld);
            const float fz = std::floor((link.start.z - cfg.settings.boundsMin.z) / cfg.tileWorld);
            if (!(fx >= 0.0f && fz >= 0.0f && fx < float(cfg.tilesX) && fz < float(cfg.tilesZ))) {
                log::warning(detail::kLog, "link {} starts outside the tile grid; dropped", link.id);
                continue;
            }
            tiles[std::size_t(fz) * std::size_t(cfg.tilesX) + std::size_t(fx)].links.push_back(link);
        }
        return {};
    }

    void openBatch() {
        if (batchOpen) return;
        batchOpen = true;
        batchStart = Clock::now();
    }

    void markDirty(int index) {
        Tile& tile = tiles[std::size_t(index)];
        if (!tile.dirty) {
            tile.dirty = true;
            dirty.push_back(index);
        }
        openBatch();
    }

    // The tiles whose columns overlap [min, max] in x and z.
    void markRange(glm::vec3 min, glm::vec3 max) {
        const Config& c = *config;
        const auto tile = [&](float v, float origin, int count) {
            return int(std::clamp(std::floor((v - origin) / c.tileWorld), -1.0f, float(count)));
        };
        const int x0 = std::max(0, tile(min.x, c.settings.boundsMin.x, c.tilesX));
        const int x1 = std::min(c.tilesX - 1, tile(max.x, c.settings.boundsMin.x, c.tilesX));
        const int z0 = std::max(0, tile(min.z, c.settings.boundsMin.z, c.tilesZ));
        const int z1 = std::min(c.tilesZ - 1, tile(max.z, c.settings.boundsMin.z, c.tilesZ));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) markDirty(z * c.tilesX + x);
    }

    void markObstacle(const Obstacle& obstacle) {
        glm::vec3 min, max;
        detail::obstacleBounds(obstacle, config->settings.agent, min, max);
        markRange(min, max);
    }

    void start(int index) {
        const Config& c = *config;
        Tile& tile = tiles[std::size_t(index)];
        auto work = std::make_shared<TileWork>();
        work->tx = index % c.tilesX;
        work->tz = index / c.tilesX;
        work->source = tile.source;
        if (!work->source) work->layers = tile.layers;
        work->links = tile.links;
        const glm::vec3 lo = c.tileMin(work->tx, work->tz), hi = c.tileMax(work->tx, work->tz);
        for (const ObstacleSlot& slot : obstacles) {
            if (!slot.used) continue;
            glm::vec3 min, max;
            detail::obstacleBounds(slot.obstacle, c.settings.agent, min, max);
            if (max.x >= lo.x && min.x <= hi.x && max.z >= lo.z && min.z <= hi.z)
                work->obstacles.push_back(slot.obstacle);
        }
        std::vector<Job> after;
        if (tile.source) after.push_back(tile.sourceReady);
        tile.job = jobs.run([cfg = config, work] { runTile(*cfg, *work); }, JobPriority::Low, after);
        tile.work = std::move(work);
        tile.running = true;
        tile.dirty = false;
        running.push_back(index);
    }

    void kick() {
        std::vector<int> waiting;
        for (const int index : dirty) {
            if (tiles[std::size_t(index)].running)
                waiting.push_back(index);
            else
                start(index);
        }
        dirty = std::move(waiting);
    }

    void apply(int index) {
        Tile& tile = tiles[std::size_t(index)];
        TileWork& work = *tile.work;
        if (!work.error.empty()) log::error(detail::kLog, "{}", work.error);
        if (work.rasterized) tile.layers = std::move(work.newLayers);
        if (work.source && tile.source == work.source) {
            // A failed voxelization keeps the old layers rather than trying again every frame.
            tile.source.reset();
            tile.sourceReady = {};
        }
        if (!work.rasterized && work.source) {
            tile.running = false;
            tile.job = {};
            tile.work.reset();
            return;
        }
        for (const dtTileRef ref : tile.refs) mesh->removeTile(ref, nullptr, nullptr);
        polygons -= tile.polygons;
        tile.refs.clear();
        tile.hashes.clear();
        tile.meshBytes = 0;
        tile.polygons = 0;
        for (std::size_t k = 0; k < work.tiles.size(); ++k) {
            TileData& data = work.tiles[k];
            if (int(k) >= config->settings.maxLayersPerTile) {
                log::warning(detail::kLog, "tile ({}, {}): {} layers, more than {}; the rest dropped", work.tx, work.tz,
                             work.tiles.size(), config->settings.maxLayersPerTile);
                break;
            }
            // A fixed slot per layer and the slot's next salt: the same references whatever order the tiles finish
            // in, and new ones for a rebuilt tile (paths through the old one see that it is gone).
            const int slot = index * config->settings.maxLayersPerTile + int(k);
            const unsigned salt = std::as_const(*mesh).getTile(slot)->salt;
            const dtTileRef wanted = mesh->encodePolyId(salt, unsigned(slot), 0);
            dtTileRef ref = 0;
            const dtStatus status = mesh->addTile(data.data.get(), data.size, DT_TILE_FREE_DATA, wanted, &ref);
            if (dtStatusFailed(status)) {
                log::error(detail::kLog, "tile ({}, {}): not added to the mesh (status {:#x})", work.tx, work.tz,
                           status);
                continue;
            }
            data.data.release();
            tile.refs.push_back(ref);
            tile.hashes.push_back(data.hash);
            tile.meshBytes += std::uint64_t(data.size);
            tile.polygons += data.polygons;
        }
        polygons += tile.polygons;
        ++revision;
        tile.running = false;
        tile.job = {};
        tile.work.reset();
    }

    void update(double jobSeconds) {
        if (jobs.workerCount() == 0 && !running.empty()) jobs.runFor(jobSeconds);
        std::size_t kept = 0;
        for (const int index : running) {
            if (tiles[std::size_t(index)].job.done())
                apply(index);
            else
                running[kept++] = index;
        }
        running.resize(kept);
        kick();
        if (batchOpen && running.empty() && dirty.empty()) {
            batchOpen = false;
            lastBuildSeconds = std::chrono::duration<double>(Clock::now() - batchStart).count();
            const Config& c = *config;
            if (firstBatch)
                log::info(detail::kLog, "navmesh of {} x {} tiles: {} polygons in {:.1f} ms", c.tilesX, c.tilesZ,
                          polygons, lastBuildSeconds * 1000.0);
            firstBatch = false;
        }
    }

    std::uint64_t layerBytes() const {
        std::uint64_t bytes = 0;
        for (const Tile& tile : tiles)
            for (const LayerBytes& layer : tile.layers) bytes += layer->size();
        return bytes;
    }
};

NavMesh::NavMesh(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
NavMesh::NavMesh(NavMesh&&) noexcept = default;
NavMesh& NavMesh::operator=(NavMesh&&) noexcept = default;
NavMesh::~NavMesh() = default;

Result<NavMesh> NavMesh::build(Geometry geometry, const BuildSettings& settings, JobSystem& jobs) {
    Result<std::pair<glm::vec3, glm::vec3>> bounds = detail::checkGeometry(geometry, settings);
    if (!bounds) return Error{bounds.error()};
    Result<Config> config = detail::makeConfig(settings, bounds->first, bounds->second);
    if (!config) return Error{config.error()};
    auto impl = std::make_unique<Impl>(jobs);
    std::vector<Link> links = std::move(geometry.links);
    geometry.links.clear();
    if (Result<void> r = impl->init(std::move(*config), std::move(links)); !r) return Error{r.error()};
    const Config& c = *impl->config;

    auto source = std::make_shared<SourceGeometry>();
    source->geometry = std::move(geometry);
    source->maskMaxX = c.tilesX - 1;
    source->maskMaxZ = c.tilesZ - 1;
    const Job ready = jobs.run([source, cfg = impl->config] { source->prepare(*cfg); }, JobPriority::Low);
    for (int i = 0; i < c.tilesX * c.tilesZ; ++i) {
        impl->tiles[std::size_t(i)].source = source;
        impl->tiles[std::size_t(i)].sourceReady = ready;
        impl->markDirty(i);
    }
    impl->kick();
    return NavMesh(std::move(impl));
}

Result<NavMesh> NavMesh::create(std::shared_ptr<const NavMeshData> data, JobSystem& jobs) {
    if (!data) return Error{"no navmesh data"};
    Result<Config> config = detail::makeConfig(data->settings, data->settings.boundsMin, data->settings.boundsMax);
    if (!config) return Error{config.error()};
    if (config->tilesX != data->tilesX || config->tilesZ != data->tilesZ)
        return Error{std::format("{} x {} tiles in data whose bounds make {} x {}", data->tilesX, data->tilesZ,
                                 config->tilesX, config->tilesZ)};
    for (const Link& link : data->links)
        if (link.area >= config->settings.areas.size())
            return Error{std::format("link {}: area {} of {}", link.id, int(link.area), config->settings.areas.size())};
    for (const NavMeshData::Layer& layer : data->layers)
        if (Result<void> r = detail::checkLayer(*config, layer); !r) return Error{r.error()};
    auto impl = std::make_unique<Impl>(jobs);
    if (Result<void> r = impl->init(std::move(*config), data->links); !r) return Error{r.error()};
    const int tilesX = impl->config->tilesX;
    for (const NavMeshData::Layer& layer : data->layers) {
        const int index = layer.tileZ * tilesX + layer.tileX;
        impl->tiles[std::size_t(index)].layers.push_back(LayerBytes(data, &layer.bytes));
        impl->markDirty(index);
    }
    impl->kick();
    return NavMesh(std::move(impl));
}

void NavMesh::update(double jobSeconds) { m_impl->update(jobSeconds); }

bool NavMesh::ready() const { return m_impl->running.empty() && m_impl->dirty.empty(); }

void NavMesh::finish() {
    while (!ready()) {
        for (const int index : m_impl->running) m_impl->jobs.wait(m_impl->tiles[std::size_t(index)].job);
        m_impl->update(0.0);
    }
}

ObstacleId NavMesh::addObstacle(const Obstacle& obstacle) {
    Impl& m = *m_impl;
    std::uint32_t index;
    if (!m.freeObstacles.empty()) {
        index = m.freeObstacles.back();
        m.freeObstacles.pop_back();
    } else {
        index = std::uint32_t(m.obstacles.size());
        m.obstacles.emplace_back();
    }
    ObstacleSlot& slot = m.obstacles[index];
    slot.obstacle = obstacle;
    slot.used = true;
    ++m.obstacleCount;
    m.markObstacle(obstacle);
    return {index, slot.generation};
}

void NavMesh::setObstacle(ObstacleId id, const Obstacle& obstacle) {
    Impl& m = *m_impl;
    if (!this->obstacle(id)) return;
    ObstacleSlot& slot = m.obstacles[id.index];
    m.markObstacle(slot.obstacle);
    slot.obstacle = obstacle;
    m.markObstacle(obstacle);
}

void NavMesh::removeObstacle(ObstacleId id) {
    Impl& m = *m_impl;
    if (!obstacle(id)) return;
    ObstacleSlot& slot = m.obstacles[id.index];
    m.markObstacle(slot.obstacle);
    slot.used = false;
    ++slot.generation;
    --m.obstacleCount;
    m.freeObstacles.push_back(id.index);
}

const Obstacle* NavMesh::obstacle(ObstacleId id) const {
    const Impl& m = *m_impl;
    if (id.index >= m.obstacles.size()) return nullptr;
    const ObstacleSlot& slot = m.obstacles[id.index];
    return slot.used && slot.generation == id.generation ? &slot.obstacle : nullptr;
}

Result<void> NavMesh::rebuild(Geometry geometry, glm::vec3 min, glm::vec3 max) {
    Impl& m = *m_impl;
    const Config& c = *m.config;
    geometry.links.clear();
    if (Result<std::pair<glm::vec3, glm::vec3>> bounds = detail::checkGeometry(geometry, c.settings); !bounds)
        return Error{bounds.error()};
    const float grow = c.settings.agent.radius + float(c.borderSize) * c.settings.cellSize;
    min -= glm::vec3(grow);
    max += glm::vec3(grow);
    const auto tile = [&](float v, float origin, int count) {
        return int(std::clamp(std::floor((v - origin) / c.tileWorld), -1.0f, float(count)));
    };
    auto source = std::make_shared<SourceGeometry>();
    source->maskMinX = std::max(0, tile(min.x, c.settings.boundsMin.x, c.tilesX));
    source->maskMaxX = std::min(c.tilesX - 1, tile(max.x, c.settings.boundsMin.x, c.tilesX));
    source->maskMinZ = std::max(0, tile(min.z, c.settings.boundsMin.z, c.tilesZ));
    source->maskMaxZ = std::min(c.tilesZ - 1, tile(max.z, c.settings.boundsMin.z, c.tilesZ));
    if (source->maskMinX > source->maskMaxX || source->maskMinZ > source->maskMaxZ) return {};
    source->geometry = std::move(geometry);
    const Job ready = m.jobs.run([source, cfg = m.config] { source->prepare(*cfg); }, JobPriority::Low);
    for (int z = source->maskMinZ; z <= source->maskMaxZ; ++z)
        for (int x = source->maskMinX; x <= source->maskMaxX; ++x) {
            Tile& t = m.tiles[std::size_t(z * c.tilesX + x)];
            t.source = source;
            t.sourceReady = ready;
            m.markDirty(z * c.tilesX + x);
        }
    m.kick();
    return {};
}

const BuildSettings& NavMesh::settings() const { return m_impl->config->settings; }

std::span<const Link> NavMesh::links() const { return m_impl->links; }

NavMeshStats NavMesh::stats() const {
    const Impl& m = *m_impl;
    NavMeshStats s;
    s.tilesX = m.config->tilesX;
    s.tilesZ = m.config->tilesZ;
    for (const Tile& tile : m.tiles) {
        s.tiles += int(tile.refs.size());
        s.memoryBytes += tile.meshBytes;
    }
    s.memoryBytes += m.layerBytes();
    s.polygons = m.polygons;
    s.pendingTiles = int(m.running.size());
    for (const int index : m.dirty)
        if (!m.tiles[std::size_t(index)].running) ++s.pendingTiles;
    s.obstacles = m.obstacleCount;
    s.lastBuildSeconds = m.lastBuildSeconds;
    return s;
}

std::uint64_t NavMesh::revision() const { return m_impl->revision; }

std::uint64_t NavMesh::contentHash() const {
    std::vector<std::uint64_t> hashes;
    for (const Tile& tile : m_impl->tiles) hashes.insert(hashes.end(), tile.hashes.begin(), tile.hashes.end());
    return XXH64(hashes.data(), hashes.size() * sizeof(std::uint64_t), 0);
}

NavMeshData NavMesh::data() const {
    const Impl& m = *m_impl;
    NavMeshData out;
    out.settings = m.config->settings;
    out.links = m.links;
    out.tilesX = m.config->tilesX;
    out.tilesZ = m.config->tilesZ;
    for (int i = 0; i < int(m.tiles.size()); ++i)
        for (std::size_t k = 0; k < m.tiles[std::size_t(i)].layers.size(); ++k)
            out.layers.push_back({i % out.tilesX, i / out.tilesX, std::int32_t(k), *m.tiles[std::size_t(i)].layers[k]});
    return out;
}

namespace detail {

dtNavMesh* NavMeshAccess::mesh(const NavMesh& mesh) { return mesh.m_impl->mesh.get(); }

const Config& NavMeshAccess::config(const NavMesh& mesh) { return *mesh.m_impl->config; }

std::vector<Obstacle> NavMeshAccess::obstacles(const NavMesh& mesh) {
    std::vector<Obstacle> out;
    for (const ObstacleSlot& slot : mesh.m_impl->obstacles)
        if (slot.used) out.push_back(slot.obstacle);
    return out;
}

} // namespace detail

} // namespace quench::navigation
