#include <quench/assets/AssetManager.h>

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace quench::assets {

const char* stateName(AssetState state) {
    switch (state) {
    case AssetState::Loading: return "loading";
    case AssetState::Ready: return "ready";
    case AssetState::Failed: return "failed";
    }
    return "?";
}

namespace {

struct TypeEntry {
    std::string name;
    std::function<std::unique_ptr<Asset>(std::string_view)> make;
    AssetMemory budget;
    std::uint64_t evictions = 0;
    bool removed = false; // unregisterType
};

void eraseValue(std::vector<Asset*>& list, Asset* asset) { std::erase(list, asset); }

} // namespace

struct AssetManager::Impl {
    std::vector<TypeEntry> types;
    std::unordered_map<std::type_index, std::size_t> typeIndex;
    std::unordered_map<AssetId, std::unique_ptr<Asset>, AssetIdHash> assets;
    std::vector<Asset*> queued;  // waiting for a load job
    std::vector<Asset*> running; // in a load job
    std::vector<Asset*> decoded; // waiting for the upload budget
    std::mutex doneMutex;
    std::vector<Asset*> done; // load jobs that returned, filled by the jobs
    std::uint64_t updates = 0;
    double lastPoll = 0.0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::uint64_t loadsStarted = 0, loadsFinished = 0, reloads = 0, evictions = 0;
    std::uint64_t uploadedLastUpdate = 0;
    std::uint32_t finishedLastUpdate = 0;

    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
};

AssetManager::AssetManager(JobSystem& jobs, const AssetManagerSettings& settings)
    : m_jobs(jobs), m_settings(settings), m_impl(std::make_unique<Impl>()) {}

AssetManager::~AssetManager() {
    for (Asset* asset : m_impl->running) m_jobs.wait(asset->m_job);
    m_impl->running.clear();
    m_impl->queued.clear();
    m_impl->decoded.clear();
    {
        std::lock_guard lock(m_impl->doneMutex);
        m_impl->done.clear();
    }
    // Unreferenced first: unloading one (a model) can release the last reference to another (its textures).
    while (!m_impl->assets.empty()) {
        auto it = std::find_if(m_impl->assets.begin(), m_impl->assets.end(),
                               [](const auto& entry) { return entry.second->m_refs == 0; });
        if (it == m_impl->assets.end()) {
            QUENCH_ASSERT(false, "AssetManager destroyed while asset references are alive");
            it = m_impl->assets.begin();
            it->second->m_refs = 0;
        }
        std::unique_ptr<Asset> asset = std::move(it->second);
        m_impl->assets.erase(it);
        asset->unload();
    }
}

void AssetManager::addType(std::type_index type, std::string name,
                           std::function<std::unique_ptr<Asset>(std::string_view)> make, AssetMemory budget) {
    QUENCH_ASSERT(!m_impl->typeIndex.contains(type), "asset type registered twice");
    m_impl->typeIndex.emplace(type, m_impl->types.size());
    m_impl->types.push_back({std::move(name), std::move(make), budget, 0, false});
}

void AssetManager::removeType(std::type_index type) {
    const auto typeIt = m_impl->typeIndex.find(type);
    QUENCH_ASSERT(typeIt != m_impl->typeIndex.end(), "unregisterType of an unregistered asset type");
    if (typeIt == m_impl->typeIndex.end()) return;
    const std::size_t index = typeIt->second;
    const auto ofType = [index](const Asset* asset) { return asset->m_type == index; };
    for (Asset* asset : m_impl->running)
        if (ofType(asset)) m_jobs.wait(asset->m_job);
    std::erase_if(m_impl->running, ofType);
    std::erase_if(m_impl->queued, ofType);
    std::erase_if(m_impl->decoded, ofType);
    {
        std::lock_guard lock(m_impl->doneMutex);
        std::erase_if(m_impl->done, ofType);
    }
    for (;;) {
        auto it = std::find_if(m_impl->assets.begin(), m_impl->assets.end(), [&](const auto& entry) {
            return ofType(entry.second.get()) && entry.second->m_refs == 0;
        });
        if (it == m_impl->assets.end()) {
            it = std::find_if(m_impl->assets.begin(), m_impl->assets.end(),
                              [&](const auto& entry) { return ofType(entry.second.get()); });
            if (it == m_impl->assets.end()) break;
            QUENCH_ASSERT(false, "unregisterType while references to the type's assets are alive");
            it->second->m_refs = 0;
        }
        std::unique_ptr<Asset> asset = std::move(it->second);
        m_impl->assets.erase(it);
        asset->unload();
    }
    m_impl->typeIndex.erase(typeIt);
    m_impl->types[index].make = nullptr;
    m_impl->types[index].removed = true;
}

Asset* AssetManager::acquire(std::type_index type, std::string_view path, float priority) {
    const auto typeIt = m_impl->typeIndex.find(type);
    QUENCH_ASSERT(typeIt != m_impl->typeIndex.end(), "AssetManager::load of an unregistered asset type");
    if (typeIt == m_impl->typeIndex.end()) return nullptr;
    const AssetId id = assetId(path);
    if (auto it = m_impl->assets.find(id); it != m_impl->assets.end()) {
        Asset* asset = it->second.get();
        QUENCH_ASSERT(asset->m_type == typeIt->second, "AssetManager::load: the path is cached as another type");
        if (asset->m_type != typeIt->second) return nullptr;
        if (asset->m_refs == 0 || priority < asset->m_priority) asset->m_priority = priority;
        return asset;
    }

    std::unique_ptr<Asset> made = m_impl->types[typeIt->second].make(path);
    QUENCH_ASSERT(made, "asset factory returned null");
    if (!made) return nullptr;
    Asset* asset = made.get();
    asset->m_manager = this;
    asset->m_type = typeIt->second;
    asset->m_id = id;
    asset->m_path = std::string(path);
    asset->m_priority = priority;
    asset->m_lastUsed = m_impl->updates;
    m_impl->assets.emplace(id, std::move(made));
    if (std::string reason = fs::checkPath(path); !reason.empty()) {
        asset->m_state = AssetState::Failed;
        asset->m_error = reason;
        asset->failed();
        asset->m_memory = asset->residentMemory();
        log::error("assets", "{} '{}': {}; the placeholder stays", m_impl->types[asset->m_type].name, path, reason);
        return asset;
    }
    asset->m_notBefore = m_impl->now() + m_settings.simulatedLatency;
    m_impl->queued.push_back(asset);
    return asset;
}

Asset* AssetManager::lookup(std::type_index type, AssetId id) const {
    const auto typeIt = m_impl->typeIndex.find(type);
    const auto it = m_impl->assets.find(id);
    if (typeIt == m_impl->typeIndex.end() || it == m_impl->assets.end() || it->second->m_type != typeIt->second)
        return nullptr;
    return it->second.get();
}

void AssetManager::addRef(Asset& asset) { ++asset.m_refs; }

void AssetManager::release(Asset& asset) {
    QUENCH_ASSERT(asset.m_refs > 0, "AssetRef released more often than taken");
    if (asset.m_refs > 0 && --asset.m_refs == 0) asset.m_lastUsed = m_impl->updates;
}

void AssetManager::setPriority(AssetId id, float priority) {
    if (auto it = m_impl->assets.find(id); it != m_impl->assets.end()) it->second->m_priority = priority;
}

void AssetManager::reload(AssetId id) {
    auto it = m_impl->assets.find(id);
    if (it == m_impl->assets.end()) return;
    Asset* asset = it->second.get();
    if (asset->m_state == AssetState::Loading || asset->m_jobRunning || asset->m_reloadPending ||
        !fs::checkPath(asset->m_path).empty())
        return;
    if (asset->m_state == AssetState::Ready)
        asset->m_reloadPending = true;
    else
        asset->m_state = AssetState::Loading; // a failed load tries again; its placeholder stays meanwhile
    asset->m_notBefore = m_impl->now();
    m_impl->queued.push_back(asset);
}

void AssetManager::setBudget(std::string_view typeName, AssetMemory budget) {
    for (TypeEntry& type : m_impl->types)
        if (type.name == typeName && !type.removed) type.budget = budget;
}

void AssetManager::evictUnreferenced() {
    std::vector<AssetId> ids;
    for (const auto& [id, asset] : m_impl->assets)
        if (asset->m_refs == 0 && !asset->m_jobRunning) ids.push_back(id);
    // Evicting a model can release its textures: repeat until nothing changes.
    while (!ids.empty()) {
        for (AssetId id : ids) {
            auto it = m_impl->assets.find(id);
            if (it == m_impl->assets.end() || it->second->m_refs != 0) continue;
            std::unique_ptr<Asset> asset = std::move(it->second);
            m_impl->assets.erase(it);
            eraseValue(m_impl->queued, asset.get());
            eraseValue(m_impl->decoded, asset.get());
            asset->unload();
            m_impl->types[asset->m_type].evictions++;
            m_impl->evictions++;
        }
        ids.clear();
        for (const auto& [id, asset] : m_impl->assets)
            if (asset->m_refs == 0 && !asset->m_jobRunning) ids.push_back(id);
    }
}

void AssetManager::update() {
    Impl& s = *m_impl;
    ++s.updates;
    if (m_jobs.workerCount() == 0) m_jobs.runFor(m_settings.mainThreadJobSeconds);
    const double now = s.now();

    // Loads whose job returned.
    std::vector<Asset*> done;
    {
        std::lock_guard lock(s.doneMutex);
        done.swap(s.done);
    }
    for (Asset* asset : done) {
        asset->m_jobRunning = false;
        eraseValue(s.running, asset);
        if (asset->m_decodeError.empty()) {
            s.decoded.push_back(asset);
            continue;
        }
        const std::string& type = s.types[asset->m_type].name;
        if (asset->m_reloadPending) {
            asset->m_reloadPending = false;
            log::warning("assets", "reload of {} '{}' failed: {}; the loaded data stays", type, asset->m_path,
                         asset->m_decodeError);
        } else {
            asset->m_state = AssetState::Failed;
            asset->m_error = std::move(asset->m_decodeError);
            asset->failed();
            asset->m_memory = asset->residentMemory();
            s.loadsFinished++;
            log::error("assets", "{} '{}': {}; the placeholder stays", type, asset->m_path, asset->m_error);
        }
        asset->m_decodeError.clear();
    }

    // Uploads within the frame's budget, the lowest priority value first (at least one per update).
    std::stable_sort(s.decoded.begin(), s.decoded.end(),
                     [](const Asset* a, const Asset* b) { return a->m_priority < b->m_priority; });
    s.uploadedLastUpdate = 0;
    s.finishedLastUpdate = 0;
    std::size_t taken = 0;
    for (; taken < s.decoded.size(); ++taken) {
        Asset* asset = s.decoded[taken];
        const std::uint64_t bytes = asset->uploadBytes();
        if (s.finishedLastUpdate > 0 && s.uploadedLastUpdate + bytes > m_settings.uploadBytesPerFrame) break;
        s.uploadedLastUpdate += bytes;
        s.finishedLastUpdate++;
        const bool reloading = asset->m_reloadPending;
        asset->m_reloadPending = false;
        const std::string& type = s.types[asset->m_type].name;
        if (Result<void> finished = asset->finish(); finished) {
            asset->m_state = AssetState::Ready;
            asset->m_error.clear();
            asset->m_version++;
            if (reloading) {
                s.reloads++;
                log::info("assets", "reloaded {} '{}'", type, asset->m_path);
            } else {
                s.loadsFinished++;
            }
        } else if (reloading) {
            log::warning("assets", "reload of {} '{}' failed: {}; the loaded data stays", type, asset->m_path,
                         finished.error());
        } else {
            asset->m_state = AssetState::Failed;
            asset->m_error = finished.error();
            asset->failed();
            s.loadsFinished++;
            log::error("assets", "{} '{}': {}; the placeholder stays", type, asset->m_path, asset->m_error);
        }
        asset->m_memory = asset->residentMemory();
    }
    s.decoded.erase(s.decoded.begin(), s.decoded.begin() + std::ptrdiff_t(taken));

    // Queued loads nobody wants any more are dropped before they start.
    for (Asset* asset : std::vector<Asset*>(s.queued)) {
        if (asset->m_refs != 0 || asset->m_reloadPending || asset->m_state != AssetState::Loading) continue;
        eraseValue(s.queued, asset);
        auto it = s.assets.find(asset->m_id);
        std::unique_ptr<Asset> owned = std::move(it->second);
        s.assets.erase(it);
        owned->unload();
    }

    // New load jobs.
    std::stable_sort(s.queued.begin(), s.queued.end(),
                     [](const Asset* a, const Asset* b) { return a->m_priority < b->m_priority; });
    for (auto it = s.queued.begin(); it != s.queued.end() && int(s.running.size()) < m_settings.maxLoadsInFlight;) {
        Asset* asset = *it;
        if (asset->m_notBefore > now) {
            ++it;
            continue;
        }
        it = s.queued.erase(it);
        asset->m_modified = fs::assetModifiedTime(asset->m_path).value_or(-1);
        asset->m_jobRunning = true;
        asset->m_decodeError.clear();
        s.running.push_back(asset);
        s.loadsStarted++;
        asset->m_job = m_jobs.run(
            [this, asset] {
                std::string error;
                Result<fs::Bytes> bytes = fs::readAsset(asset->m_path);
                if (!bytes)
                    error = bytes.error();
                else if (Result<void> decoded = asset->decode(std::move(*bytes)); !decoded)
                    error = decoded.error().empty() ? std::string("decode failed") : decoded.error();
                asset->m_decodeError = std::move(error);
                std::lock_guard lock(m_impl->doneMutex);
                m_impl->done.push_back(asset);
            },
            m_settings.jobPriority);
    }

    // Changed files (hot reload).
    if (m_settings.hotReload && now - s.lastPoll >= m_settings.hotReloadInterval) {
        s.lastPoll = now;
        for (const auto& [id, asset] : s.assets) {
            if (asset->m_state == AssetState::Loading || asset->m_jobRunning || asset->m_reloadPending) continue;
            const std::optional<std::int64_t> modified = fs::assetModifiedTime(asset->m_path);
            if (modified && *modified != asset->m_modified) reload(id);
        }
    }

    // Unreferenced assets over their type's budget, least recently used first; failed ones at once, so that a later
    // load tries again.
    for (std::size_t type = 0; type < s.types.size(); ++type) {
        AssetMemory used{};
        std::vector<Asset*> candidates;
        for (const auto& [id, asset] : s.assets) {
            if (asset->m_type != type) continue;
            used.cpu += asset->m_memory.cpu;
            used.gpu += asset->m_memory.gpu;
            if (asset->m_refs == 0 && asset->m_state != AssetState::Loading && !asset->m_jobRunning &&
                !asset->m_reloadPending)
                candidates.push_back(asset.get());
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Asset* a, const Asset* b) {
            if ((a->m_state == AssetState::Failed) != (b->m_state == AssetState::Failed))
                return a->m_state == AssetState::Failed;
            return a->m_lastUsed < b->m_lastUsed;
        });
        const AssetMemory budget = s.types[type].budget;
        for (Asset* asset : candidates) {
            const bool over = used.cpu > budget.cpu || used.gpu > budget.gpu;
            if (!over && asset->m_state != AssetState::Failed) break;
            used.cpu -= std::min(used.cpu, asset->m_memory.cpu);
            used.gpu -= std::min(used.gpu, asset->m_memory.gpu);
            auto it = s.assets.find(asset->m_id);
            std::unique_ptr<Asset> owned = std::move(it->second);
            s.assets.erase(it);
            owned->unload();
            s.types[type].evictions++;
            s.evictions++;
        }
    }
}

bool AssetManager::idle() const { return m_impl->queued.empty() && m_impl->running.empty() && m_impl->decoded.empty(); }

AssetManager::Stats AssetManager::stats() const {
    const Impl& s = *m_impl;
    Stats stats;
    stats.queued = std::uint32_t(s.queued.size());
    stats.decoding = std::uint32_t(s.running.size());
    stats.waitingForUpload = std::uint32_t(s.decoded.size());
    stats.loadsStarted = s.loadsStarted;
    stats.loadsFinished = s.loadsFinished;
    stats.reloads = s.reloads;
    stats.evictions = s.evictions;
    stats.uploadedLastUpdate = s.uploadedLastUpdate;
    stats.finishedLastUpdate = s.finishedLastUpdate;
    stats.types.resize(s.types.size());
    for (std::size_t i = 0; i < s.types.size(); ++i) {
        stats.types[i].name = s.types[i].name;
        stats.types[i].budget = s.types[i].budget;
        stats.types[i].evictions = s.types[i].evictions;
    }
    for (const auto& [id, asset] : s.assets) {
        TypeStats& type = stats.types[asset->m_type];
        type.count++;
        type.memory.cpu += asset->m_memory.cpu;
        type.memory.gpu += asset->m_memory.gpu;
        if (asset->m_refs == 0) type.unreferenced++;
        switch (asset->m_state) {
        case AssetState::Loading:
            type.loading++;
            stats.loading++;
            break;
        case AssetState::Ready:
            type.ready++;
            stats.ready++;
            break;
        case AssetState::Failed:
            type.failed++;
            stats.failed++;
            break;
        }
    }
    for (std::size_t i = s.types.size(); i-- > 0;)
        if (s.types[i].removed) stats.types.erase(stats.types.begin() + std::ptrdiff_t(i));
    return stats;
}

void AssetManager::forEach(const std::function<void(const Asset&)>& fn) const {
    std::vector<const Asset*> sorted;
    for (const auto& [id, asset] : m_impl->assets) sorted.push_back(asset.get());
    std::sort(sorted.begin(), sorted.end(), [](const Asset* a, const Asset* b) { return a->path() < b->path(); });
    for (const Asset* asset : sorted) fn(*asset);
}

} // namespace quench::assets
