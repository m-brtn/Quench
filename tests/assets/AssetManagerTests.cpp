#include "Test.h"

#include <quench/assets/AssetManager.h>
#include <quench/core/FileSystem.h>

#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace quench;
using namespace quench::assets;

namespace {

// A file with a number: decode parses it ("slow" sleeps first, anything else fails), finish makes it resident.
class NumberAsset final : public Asset {
public:
    int value = 0;           // resident
    bool placeholder = true; // until the first finish()
    int finishes = 0;
    int failures = 0;
    std::vector<std::string>* finishOrder = nullptr;

private:
    Result<void> decode(fs::Bytes bytes) override {
        std::string text(bytes.begin(), bytes.end());
        if (text.starts_with("slow ")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            text = text.substr(5);
        }
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), m_pending);
        if (ec != std::errc() || end != text.data() + text.size()) return Error{"not a number: '" + text + "'"};
        m_pendingBytes = bytes.size();
        return {};
    }
    std::uint64_t uploadBytes() const override { return m_pendingBytes; }
    Result<void> finish() override {
        value = m_pending;
        m_resident = m_pendingBytes;
        placeholder = false;
        ++finishes;
        if (finishOrder) finishOrder->push_back(path());
        return {};
    }
    void failed() override { ++failures; }
    void unload() override { m_resident = 0; }
    AssetMemory residentMemory() const override { return {m_resident, 0}; }

    int m_pending = 0;
    std::uint64_t m_pendingBytes = 0;
    std::uint64_t m_resident = 0;
};

// Holds a reference to the number named in its file: unloading it can release the last reference to another asset.
class LinkAsset final : public Asset {
public:
    AssetRef<NumberAsset> target;
    AssetManager* manager = nullptr;

private:
    Result<void> decode(fs::Bytes bytes) override {
        m_pending.assign(bytes.begin(), bytes.end());
        return {};
    }
    std::uint64_t uploadBytes() const override { return 0; }
    Result<void> finish() override {
        target = manager->load<NumberAsset>(m_pending);
        return {};
    }
    void unload() override { target = {}; }
    AssetMemory residentMemory() const override { return {}; }

    std::string m_pending;
};

std::filesystem::path g_assets;

void writeFile(const std::string& path, const std::string& text) {
    std::filesystem::create_directories((g_assets / path).parent_path());
    std::ofstream(g_assets / path, std::ios::binary | std::ios::trunc) << text;
}

// A clean asset directory per case, installed as the file system backend.
void useDir(const char* name) {
    g_assets = std::filesystem::path(quench::test::tmpDir()) / name / "assets";
    std::filesystem::remove_all(g_assets.parent_path());
    std::filesystem::create_directories(g_assets);
    fs::setBackend(fs::makeStdBackend(g_assets, {}));
}

void registerNumbers(AssetManager& manager, AssetMemory budget = AssetManager::kUnlimited,
                     std::vector<std::string>* order = nullptr) {
    manager.registerType<NumberAsset>(
        "number",
        [order](std::string_view) {
            auto asset = std::make_unique<NumberAsset>();
            asset->finishOrder = order;
            return asset;
        },
        budget);
}

// Updates until nothing is loading (at most 5 s).
bool settle(AssetManager& manager) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < end) {
        manager.update();
        if (manager.idle()) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

// Runs a case with worker threads and without (loads then run inside update()).
template <class Fn>
void forEachMode(Fn fn) {
    for (int workers : {2, 0}) {
        JobSystem jobs(JobSystemDesc{.workerThreads = workers});
        fn(jobs, workers);
    }
}

} // namespace

QUENCH_TEST(assets_load_ready) {
    useDir("assets_load_ready");
    writeFile("a.txt", "1");
    writeFile("numbers/b.txt", "22");
    forEachMode([](JobSystem& jobs, int workers) {
        AssetManager manager(jobs);
        registerNumbers(manager);
        AssetRef<NumberAsset> a = manager.load<NumberAsset>("a.txt");
        AssetRef<NumberAsset> b = manager.load<NumberAsset>("numbers/b.txt");
        CHECK(a.state() == AssetState::Loading && a->placeholder, "%d workers: not loading at first", workers);
        REQUIRE(settle(manager), "%d workers: loads did not finish", workers);
        CHECK(a.ready() && a->value == 1 && b.ready() && b->value == 22, "%d workers: %s %d, %s %d", workers,
              stateName(a.state()), a->value, stateName(b.state()), b->value);
        CHECK(a->version() == 1 && a->id() == assetId("a.txt") && a->memory().cpu == 1, "%d workers: version %u",
              workers, a->version());
        const AssetManager::Stats stats = manager.stats();
        CHECK(stats.ready == 2 && stats.loadsStarted == 2 && stats.loadsFinished == 2 && stats.types.size() == 1 &&
                  stats.types[0].memory.cpu == 3,
              "%d workers: stats ready %u, started %llu", workers, stats.ready, (unsigned long long)stats.loadsStarted);
    });
    fs::setBackend(nullptr);
}

QUENCH_TEST(assets_failures_keep_placeholder) {
    useDir("assets_failures_keep_placeholder");
    writeFile("bad.txt", "twelve");
    forEachMode([](JobSystem& jobs, int workers) {
        AssetManager manager(jobs);
        registerNumbers(manager);
        AssetRef<NumberAsset> missing = manager.load<NumberAsset>("missing.txt");
        AssetRef<NumberAsset> bad = manager.load<NumberAsset>("bad.txt");
        AssetRef<NumberAsset> escape = manager.load<NumberAsset>("../outside.txt");
        CHECK(escape.state() == AssetState::Failed && escape->failures == 1, "%d workers: bad path not failed at once",
              workers);
        REQUIRE(settle(manager), "%d workers: loads did not finish", workers);
        CHECK(missing.state() == AssetState::Failed && missing->placeholder && missing->failures == 1 &&
                  missing->error().find("missing.txt") != std::string::npos,
              "%d workers: missing: %s '%s'", workers, stateName(missing.state()), missing->error().c_str());
        CHECK(bad.state() == AssetState::Failed && bad->error().find("twelve") != std::string::npos,
              "%d workers: bad: %s '%s'", workers, stateName(bad.state()), bad->error().c_str());
        CHECK(manager.stats().failed == 3, "%d workers: %u failed", workers, manager.stats().failed);
        // Unreferenced failures are dropped, so that a later load tries again.
        missing = {};
        manager.update();
        CHECK(!manager.find<NumberAsset>(assetId("missing.txt")), "%d workers: failed asset kept", workers);
    });
    fs::setBackend(nullptr);
}

QUENCH_TEST(assets_cache_and_refs) {
    useDir("assets_cache_and_refs");
    writeFile("a.txt", "7");
    JobSystem jobs(JobSystemDesc{.workerThreads = 1});
    AssetManager manager(jobs);
    registerNumbers(manager);
    AssetRef<NumberAsset> first = manager.load<NumberAsset>("a.txt");
    AssetRef<NumberAsset> second = manager.load<NumberAsset>("a.txt");
    CHECK(first == second && first->refs() == 2, "not shared: refs %u", first->refs());
    REQUIRE(settle(manager), "load did not finish");
    NumberAsset* raw = first.get();
    first = {};
    second = {};
    CHECK(raw->refs() == 0, "refs %u", raw->refs());
    manager.update();
    AssetRef<NumberAsset> found = manager.find<NumberAsset>(assetId("a.txt"));
    CHECK(found.get() == raw && found.ready() && found->value == 7, "unreferenced asset not kept in the cache");
    AssetRef<NumberAsset> again = manager.load<NumberAsset>("a.txt");
    CHECK(again.get() == raw && manager.stats().loadsStarted == 1, "cached asset loaded again");
    found = {};
    again = {};
    manager.evictUnreferenced();
    CHECK(!manager.find<NumberAsset>(assetId("a.txt")) && manager.stats().evictions == 1, "evictUnreferenced");
}

QUENCH_TEST(assets_budget_evicts_lru) {
    useDir("assets_budget_evicts_lru");
    for (char c : std::string("abcd"))
        writeFile(std::string(1, c) + ".txt", "10000" + std::string(1, char('0' + c - 'a')));
    JobSystem jobs(JobSystemDesc{.workerThreads = 2});
    AssetManager manager(jobs);
    registerNumbers(manager, {13, AssetManager::kUnlimited.gpu}); // two of the 6-byte files
    {
        AssetRef<NumberAsset> keep = manager.load<NumberAsset>("d.txt");
        for (const char* path : {"a.txt", "b.txt", "c.txt"}) {
            AssetRef<NumberAsset> ref = manager.load<NumberAsset>(path);
            REQUIRE(settle(manager), "load of %s did not finish", path);
            // ref goes away here: a, then b, then c become unreferenced, in this order.
        }
        manager.update();
        // 24 bytes cached against 13: a and b go (least recently used); d is referenced and never evicted.
        CHECK(!manager.find<NumberAsset>(assetId("a.txt")) && !manager.find<NumberAsset>(assetId("b.txt")),
              "LRU assets kept");
        CHECK(manager.find<NumberAsset>(assetId("c.txt")) && keep.ready(), "wrong assets evicted");
        const AssetManager::Stats stats = manager.stats();
        CHECK(stats.evictions == 2 && stats.types[0].memory.cpu == 12 && stats.types[0].unreferenced == 1,
              "evictions %llu, %llu bytes", (unsigned long long)stats.evictions,
              (unsigned long long)stats.types[0].memory.cpu);
    }
    // The referenced one alone may exceed the budget; it goes once released.
    manager.setBudget("number", {0, 0});
    manager.update();
    CHECK(manager.stats().types[0].count == 0, "%u assets left over a zero budget", manager.stats().types[0].count);
}

QUENCH_TEST(assets_upload_budget_and_priority) {
    useDir("assets_upload_budget_and_priority");
    for (int i = 0; i < 5; ++i) writeFile("n" + std::to_string(i) + ".txt", "12345" + std::to_string(i));
    forEachMode([](JobSystem& jobs, int workers) {
        std::vector<std::string> order;
        AssetManagerSettings settings;
        settings.uploadBytesPerFrame = 10; // one 6-byte file per update
        settings.maxLoadsInFlight = 8;
        AssetManager manager(jobs, settings);
        registerNumbers(manager, AssetManager::kUnlimited, &order);
        std::vector<AssetRef<NumberAsset>> refs;
        for (int i = 0; i < 5; ++i)
            refs.push_back(manager.load<NumberAsset>("n" + std::to_string(i) + ".txt", float(5 - i)));
        manager.update(); // starts the five loads
        jobs.waitAll();
        // Five decoded loads at once: one finish per update, the lowest priority value first.
        int updates = 0;
        while (!manager.idle() && updates < 50) {
            manager.update();
            ++updates;
            CHECK(manager.stats().finishedLastUpdate <= 1, "%d workers: %u finished in one update", workers,
                  manager.stats().finishedLastUpdate);
        }
        REQUIRE(manager.idle(), "%d workers: loads did not finish", workers);
        REQUIRE(order.size() == 5, "%d workers: %zu finished", workers, order.size());
        CHECK(order.front() == "n4.txt" && order.back() == "n0.txt", "%d workers: order %s ... %s", workers,
              order.front().c_str(), order.back().c_str());
    });
    fs::setBackend(nullptr);
}

QUENCH_TEST(assets_upload_larger_than_budget) {
    useDir("assets_upload_larger_than_budget");
    writeFile("big.txt", "123456789");
    JobSystem jobs(JobSystemDesc{.workerThreads = 1});
    AssetManagerSettings settings;
    settings.uploadBytesPerFrame = 1;
    AssetManager manager(jobs, settings);
    registerNumbers(manager);
    AssetRef<NumberAsset> big = manager.load<NumberAsset>("big.txt");
    REQUIRE(settle(manager), "an upload above the budget never finished");
    CHECK(big.ready() && big->value == 123456789, "%s", stateName(big.state()));
}

QUENCH_TEST(assets_reload_keeps_old_data_on_failure) {
    useDir("assets_reload_keeps_old_data_on_failure");
    writeFile("a.txt", "1");
    forEachMode([](JobSystem& jobs, int workers) {
        writeFile("a.txt", "1");
        AssetManager manager(jobs);
        registerNumbers(manager);
        AssetRef<NumberAsset> a = manager.load<NumberAsset>("a.txt");
        REQUIRE(settle(manager), "%d workers: load", workers);
        writeFile("a.txt", "5");
        manager.reload(a->id());
        CHECK(a->reloading() && a.ready() && a->value == 1, "%d workers: old data gone during the reload", workers);
        REQUIRE(settle(manager), "%d workers: reload", workers);
        CHECK(a.ready() && a->value == 5 && a->version() == 2 && manager.stats().reloads == 1,
              "%d workers: after reload %s %d, version %u", workers, stateName(a.state()), a->value, a->version());
        writeFile("a.txt", "broken");
        manager.reload(a->id());
        REQUIRE(settle(manager), "%d workers: failed reload", workers);
        CHECK(a.ready() && a->value == 5 && a->version() == 2 && a->failures == 0,
              "%d workers: a failed reload changed the asset: %s %d", workers, stateName(a.state()), a->value);
    });
    fs::setBackend(nullptr);
}

QUENCH_TEST(assets_hot_reload) {
    useDir("assets_hot_reload");
    writeFile("a.txt", "1");
    writeFile("fixed.txt", "oops");
    JobSystem jobs(JobSystemDesc{.workerThreads = 1});
    AssetManagerSettings settings;
    settings.hotReload = true;
    settings.hotReloadInterval = 0.0;
    AssetManager manager(jobs, settings);
    registerNumbers(manager);
    AssetRef<NumberAsset> a = manager.load<NumberAsset>("a.txt");
    AssetRef<NumberAsset> fixed = manager.load<NumberAsset>("fixed.txt");
    REQUIRE(settle(manager), "load");
    REQUIRE(a.ready() && fixed.state() == AssetState::Failed, "first load");
    // An explicit later modification time: file systems with a coarse clock could keep the old one.
    const auto later = std::filesystem::last_write_time(g_assets / "a.txt") + std::chrono::seconds(2);
    writeFile("a.txt", "2");
    writeFile("fixed.txt", "3");
    std::filesystem::last_write_time(g_assets / "a.txt", later);
    std::filesystem::last_write_time(g_assets / "fixed.txt", later);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((a->value != 2 || !fixed.ready()) && std::chrono::steady_clock::now() < end) {
        manager.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(a->value == 2 && a->version() == 2, "changed file not reloaded: %d", a->value);
    CHECK(fixed.ready() && fixed->value == 3, "fixed file not loaded: %s", stateName(fixed.state()));
    fs::setBackend(nullptr);
}

// Files copied into the asset overlay (a device's user directory) reload a loaded asset, and so does their removal.
QUENCH_TEST(assets_hot_reload_from_overlay) {
    useDir("assets_hot_reload_from_overlay");
    const std::filesystem::path overlay = g_assets.parent_path() / "user" / "asset_overlay";
    fs::setBackend(
        fs::makeOverlayBackend(fs::makeStdBackend(g_assets, g_assets.parent_path() / "user"), "asset_overlay"));
    writeFile("numbers/a.txt", "1");
    JobSystem jobs(JobSystemDesc{.workerThreads = 1});
    AssetManager manager(jobs, AssetManagerSettings{.hotReload = true, .hotReloadInterval = 0.0});
    registerNumbers(manager);
    AssetRef<NumberAsset> a = manager.load<NumberAsset>("numbers/a.txt");
    REQUIRE(settle(manager) && a.ready() && a->value == 1, "first load");
    const auto waitFor = [&](int value) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (a->value != value && std::chrono::steady_clock::now() < end) {
            manager.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return a->value == value;
    };
    std::filesystem::create_directories(overlay / "numbers");
    std::ofstream(overlay / "numbers" / "a.txt", std::ios::binary) << "2";
    std::filesystem::last_write_time(overlay / "numbers" / "a.txt",
                                     std::filesystem::last_write_time(g_assets / "numbers" / "a.txt") +
                                         std::chrono::seconds(2));
    CHECK(waitFor(2) && a->version() == 2, "the overlay's file not loaded: %d", a->value);
    std::filesystem::remove(overlay / "numbers" / "a.txt");
    CHECK(waitFor(1) && a->version() == 3, "the bundled file not loaded again: %d", a->value);
    CHECK(manager.stats().reloads == 2, "reloads %llu", (unsigned long long)manager.stats().reloads);
    fs::setBackend(nullptr);
}

QUENCH_TEST(assets_latency_and_cancel) {
    useDir("assets_latency_and_cancel");
    writeFile("a.txt", "1");
    writeFile("b.txt", "2");
    JobSystem jobs(JobSystemDesc{.workerThreads = 1});
    AssetManagerSettings settings;
    settings.simulatedLatency = 0.05;
    AssetManager manager(jobs, settings);
    registerNumbers(manager);
    const auto start = std::chrono::steady_clock::now();
    AssetRef<NumberAsset> a = manager.load<NumberAsset>("a.txt");
    manager.update();
    CHECK(a.state() == AssetState::Loading && manager.stats().queued == 1, "load started before its latency");
    REQUIRE(settle(manager), "load");
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(50) && a.ready(),
          "latency not applied");
    // A load released before it starts is dropped without reading the file.
    { AssetRef<NumberAsset> b = manager.load<NumberAsset>("b.txt"); }
    manager.update();
    CHECK(!manager.find<NumberAsset>(assetId("b.txt")) && manager.stats().loadsStarted == 1 && manager.idle(),
          "released queued load not dropped");
}

QUENCH_TEST(assets_destroy_while_loading) {
    useDir("assets_destroy_while_loading");
    for (int i = 0; i < 8; ++i) writeFile("s" + std::to_string(i) + ".txt", "slow " + std::to_string(i));
    forEachMode([](JobSystem& jobs, int) {
        AssetManager manager(jobs);
        registerNumbers(manager);
        std::vector<AssetRef<NumberAsset>> refs;
        for (int i = 0; i < 8; ++i) refs.push_back(manager.load<NumberAsset>("s" + std::to_string(i) + ".txt"));
        manager.update(); // starts four jobs of 20 ms
        manager.update();
        refs.clear();
    });
    fs::setBackend(nullptr);
}

QUENCH_TEST(assets_dependent_assets) {
    useDir("assets_dependent_assets");
    writeFile("n.txt", "42");
    writeFile("link.txt", "n.txt");
    JobSystem jobs(JobSystemDesc{.workerThreads = 2});
    AssetManager manager(jobs, {});
    registerNumbers(manager, {0, 0}); // unreferenced numbers are evicted at once
    manager.registerType<LinkAsset>("link", [&manager](std::string_view) {
        auto asset = std::make_unique<LinkAsset>();
        asset->manager = &manager;
        return asset;
    });
    AssetRef<LinkAsset> link = manager.load<LinkAsset>("link.txt");
    REQUIRE(settle(manager), "load");
    REQUIRE(link.ready() && link->target.ready() && link->target->value == 42, "the linked number did not load");
    manager.update();
    CHECK(manager.find<NumberAsset>(assetId("n.txt")), "a number referenced by a link was evicted");
    link = {};
    manager.evictUnreferenced();
    CHECK(manager.stats().types[0].count == 0 && manager.stats().types[1].count == 0, "evicting the link left %u + %u",
          manager.stats().types[0].count, manager.stats().types[1].count);
    // Unregistering a type unloads its assets, which releases what they hold.
    link = manager.load<LinkAsset>("link.txt");
    REQUIRE(settle(manager), "second load");
    link = {};
    manager.unregisterType<LinkAsset>();
    AssetManager::Stats stats = manager.stats();
    CHECK(stats.types.size() == 1 && stats.types[0].name == "number" && stats.types[0].unreferenced == 1,
          "after unregisterType: %zu types", stats.types.size());
    // The manager's destructor unloads a cached link before the number it holds.
    manager.registerType<LinkAsset>("link", [&manager](std::string_view) {
        auto asset = std::make_unique<LinkAsset>();
        asset->manager = &manager;
        return asset;
    });
    link = manager.load<LinkAsset>("link.txt");
    REQUIRE(settle(manager), "third load");
    link = {};
    fs::setBackend(nullptr);
}
