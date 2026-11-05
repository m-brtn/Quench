#pragma once

#include <quench/assets/AssetId.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Jobs.h>
#include <quench/core/Result.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

// Asset loading for every kind of asset (textures and models in quench_render's SceneAssets; sounds and animations
// later): one cache by AssetId, reference-counted handles, loading on the job system and a per-frame budget for the
// work that has to happen on the main thread (GPU uploads).
//
// A load reads the file through quench::fs and decodes it on a job; update() then finishes it on the main thread
// (Asset::finish(), for example the GPU upload) while the frame's upload budget allows. Until then, and when a load
// fails, the asset type shows a placeholder: a failed load is one log line, never a crash. Unreferenced assets stay
// cached until their type's memory budget is exceeded, then the least recently used go first. With hot reload on,
// changed files are loaded again and swapped in under the same ID.
//
// Main thread only, except Asset::decode().
namespace quench::assets {

enum class AssetState : std::uint8_t {
    Loading, // queued, reading, decoding or waiting for the upload budget
    Ready,
    Failed, // the placeholder stays; Asset::error() says why
};

const char* stateName(AssetState state);

struct AssetMemory {
    std::uint64_t cpu = 0; // bytes kept in memory
    std::uint64_t gpu = 0; // bytes on the GPU

    std::uint64_t total() const { return cpu + gpu; }
};

class AssetManager;
template <class T>
class AssetRef;

// One asset of some type: subclassed per type, created by the type's factory when a path is first loaded.
class Asset {
public:
    virtual ~Asset() = default;
    Asset(const Asset&) = delete;
    Asset& operator=(const Asset&) = delete;

    AssetId id() const { return m_id; }
    const std::string& path() const { return m_path; }
    AssetState state() const { return m_state; }
    const std::string& error() const { return m_error; }
    std::uint32_t refs() const { return m_refs; }
    // Bumped by every finish(): what was made from an older version (scene instances of a model) is out of date.
    std::uint32_t version() const { return m_version; }
    AssetMemory memory() const { return m_memory; }
    float priority() const { return m_priority; }
    bool reloading() const { return m_reloadPending; }

protected:
    Asset() = default;

    // On a job thread: decode the file's bytes and keep the result for finish(). Touches only the asset's own pending
    // data (a reload decodes while the main thread uses the resident data).
    virtual Result<void> decode(fs::Bytes bytes) = 0;
    // After decode(): the bytes finish() will upload, counted against the frame's upload budget.
    virtual std::uint64_t uploadBytes() const = 0;
    // On the main thread: make the decoded data resident (replacing the old data on a reload) and drop the pending
    // data. An error fails the load like a decode error.
    virtual Result<void> finish() = 0;
    // On the main thread: the first load failed (a failed reload keeps the old data and does not call this).
    virtual void failed() {}
    // On the main thread: release the resident data (eviction, the manager's destruction).
    virtual void unload() = 0;
    // What the asset holds now (called after finish(), failed() and unload()).
    virtual AssetMemory residentMemory() const = 0;

private:
    friend class AssetManager;
    template <class T>
    friend class AssetRef;

    AssetManager* m_manager = nullptr;
    std::size_t m_type = 0;
    AssetId m_id{};
    std::string m_path;
    AssetState m_state = AssetState::Loading;
    std::string m_error;
    std::uint32_t m_refs = 0;
    std::uint32_t m_version = 0;
    AssetMemory m_memory{};
    float m_priority = 0.0f;
    std::uint64_t m_lastUsed = 0; // update() count when the last reference went away
    double m_notBefore = 0.0;     // simulated latency: the load starts at this time
    std::int64_t m_modified = -1; // file modification time at the last load (-1: unknown)
    bool m_jobRunning = false;
    bool m_reloadPending = false;
    std::string m_decodeError; // written by the job, read after it is done
    Job m_job;
};

// A reference to an asset of type T: keeps it in the cache (not evicted). Copyable; main thread only; must not
// outlive the manager.
template <class T>
class AssetRef {
public:
    AssetRef() = default;
    explicit AssetRef(T* asset) : m_asset(asset) { addRef(); }
    AssetRef(const AssetRef& other) : m_asset(other.m_asset) { addRef(); }
    AssetRef(AssetRef&& other) noexcept : m_asset(other.m_asset) { other.m_asset = nullptr; }
    AssetRef& operator=(AssetRef other) noexcept {
        std::swap(m_asset, other.m_asset);
        return *this;
    }
    ~AssetRef() { release(); }

    T* get() const { return m_asset; }
    T* operator->() const { return m_asset; }
    T& operator*() const { return *m_asset; }
    explicit operator bool() const { return m_asset != nullptr; }
    AssetState state() const { return m_asset ? m_asset->state() : AssetState::Failed; }
    bool ready() const { return state() == AssetState::Ready; }
    friend bool operator==(const AssetRef& a, const AssetRef& b) { return a.m_asset == b.m_asset; }

private:
    void addRef();
    void release();

    T* m_asset = nullptr;
};

struct AssetManagerSettings {
    int maxLoadsInFlight = 4;                       // read and decode jobs at once
    std::uint64_t uploadBytesPerFrame = 8ull << 20; // finish() calls per update() stop at this (at least one runs)
    double simulatedLatency = 0.0;                  // seconds a load waits before it starts (to see placeholders)
    bool hotReload = false;                         // poll the files of loaded assets for changes
    double hotReloadInterval = 0.5;                 // seconds between polls
    double mainThreadJobSeconds = 0.004;            // without worker threads: job time per update()
    JobPriority jobPriority = JobPriority::Low;     // below physics and game jobs
};

class AssetManager {
public:
    // Creates the asset object for a path (no file access).
    template <class T>
    using Factory = std::function<std::unique_ptr<T>(std::string_view path)>;

    struct TypeStats {
        std::string name;
        std::uint32_t count = 0; // cached assets, referenced or not
        std::uint32_t loading = 0;
        std::uint32_t ready = 0;
        std::uint32_t failed = 0;
        std::uint32_t unreferenced = 0;
        AssetMemory memory{};
        AssetMemory budget{};
        std::uint64_t evictions = 0;
    };

    struct Stats {
        std::uint32_t loading = 0;
        std::uint32_t ready = 0;
        std::uint32_t failed = 0;
        std::uint32_t queued = 0;           // waiting for a load job
        std::uint32_t decoding = 0;         // in a load job
        std::uint32_t waitingForUpload = 0; // decoded, waiting for the upload budget
        std::uint64_t loadsStarted = 0;
        std::uint64_t loadsFinished = 0; // ready or failed
        std::uint64_t reloads = 0;
        std::uint64_t evictions = 0;
        std::uint64_t uploadedLastUpdate = 0; // bytes
        std::uint32_t finishedLastUpdate = 0;
        std::vector<TypeStats> types;
    };

    explicit AssetManager(JobSystem& jobs, const AssetManagerSettings& settings = {});
    // Waits for running load jobs, then unloads every asset. References must be gone.
    ~AssetManager();
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    static constexpr AssetMemory kUnlimited{std::numeric_limits<std::uint64_t>::max(),
                                            std::numeric_limits<std::uint64_t>::max()};

    // Registers asset type T (derived from Asset) under a name for the stats; the budget limits what its
    // unreferenced assets may keep cached.
    template <class T>
    void registerType(std::string name, Factory<T> factory, AssetMemory budget = kUnlimited) {
        addType(
            std::type_index(typeid(T)), std::move(name),
            [f = std::move(factory)](std::string_view path) -> std::unique_ptr<Asset> { return f(path); }, budget);
    }

    // Waits for the type's load jobs and unloads its assets (before what they use goes, a scene for example). Their
    // references must be gone. The type can be registered again afterwards.
    template <class T>
    void unregisterType() {
        removeType(std::type_index(typeid(T)));
    }

    // The asset of `path`, loading it if it is not cached. Lower priorities load first (a distance, for example).
    template <class T>
    AssetRef<T> load(std::string_view path, float priority = 0.0f) {
        return AssetRef<T>(static_cast<T*>(acquire(std::type_index(typeid(T)), path, priority)));
    }
    // A cached asset without loading it; empty when not cached.
    template <class T>
    AssetRef<T> find(AssetId id) const {
        return AssetRef<T>(static_cast<T*>(lookup(std::type_index(typeid(T)), id)));
    }

    // Reorders queued loads and uploads.
    void setPriority(AssetId id, float priority);
    // Loads the file again and swaps the new data in under the same ID; the old data stays until then (and if the
    // reload fails).
    void reload(AssetId id);
    void setBudget(std::string_view typeName, AssetMemory budget);
    // Drops every unreferenced asset now.
    void evictUnreferenced();

    // Once per frame: collects finished loads, starts queued ones, finishes uploads within the budget, evicts over
    // budget, and polls for changed files.
    void update();
    // No load queued, running or waiting for its upload.
    bool idle() const;

    Stats stats() const;
    void forEach(const std::function<void(const Asset&)>& fn) const;
    const AssetManagerSettings& settings() const { return m_settings; }
    void setSimulatedLatency(double seconds) { m_settings.simulatedLatency = seconds; }
    void setHotReload(bool enabled) { m_settings.hotReload = enabled; }
    void setUploadBudget(std::uint64_t bytesPerFrame) { m_settings.uploadBytesPerFrame = bytesPerFrame; }
    JobSystem& jobs() { return m_jobs; }

private:
    template <class T>
    friend class AssetRef;
    struct Impl;

    void addType(std::type_index type, std::string name, std::function<std::unique_ptr<Asset>(std::string_view)> make,
                 AssetMemory budget);
    void removeType(std::type_index type);
    Asset* acquire(std::type_index type, std::string_view path, float priority);
    Asset* lookup(std::type_index type, AssetId id) const;
    void addRef(Asset& asset);
    void release(Asset& asset);

    JobSystem& m_jobs;
    AssetManagerSettings m_settings;
    std::unique_ptr<Impl> m_impl;
};

template <class T>
void AssetRef<T>::addRef() {
    if (m_asset) m_asset->m_manager->addRef(*m_asset);
}

template <class T>
void AssetRef<T>::release() {
    if (m_asset) m_asset->m_manager->release(*m_asset);
    m_asset = nullptr;
}

} // namespace quench::assets
