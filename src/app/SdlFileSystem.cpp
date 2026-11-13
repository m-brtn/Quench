#include <quench/app/SdlFileSystem.h>

#include "core/Platform.h"

#include <quench/core/Log.h>

#include <SDL3/SDL.h>

#if QUENCH_PLATFORM_WEB
#include <emscripten/emscripten.h>
#include <emscripten/wget.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#if QUENCH_PLATFORM_WEB
// The default user directory of a browser build is kept in IndexedDB through Emscripten's IDBFS, mounted at the
// directory itself, so that the database is named after it ("/libsdl/<organization>/<name>": one per app, also
// where several apps share an origin). Loaded once before the program reads a user file, stored after every write.
// The state is Module.quenchUserFiles: {dir, kept, busy, again, failed, entries, ms, error} (read by
// tools/web_check.mjs). No arrow functions or strict comparisons (=== and !==) in here: clang-format would put
// spaces into them.
EM_JS_DEPS(quench_user_files, "$FS,$IDBFS,$UTF8ToString,$stringToNewUTF8");

// 1: the stored files are in the directory; 0: no IndexedDB in this context; -1: it failed; -2: no answer within
// the time limit. Anything but 1 leaves the directory in memory (Module.quenchUserFiles.error says why).
EM_ASYNC_JS(int, quench_user_files_load, (const char* dir, int timeoutMs), {
    var state = {
        dir : UTF8ToString(dir),
        kept : false,
        busy : false,
        again : false,
        failed : false,
        entries : 0,
        ms : 0,
        error : ""
    };
    Module["quenchUserFiles"] = state;
    var available = false;
    try {
        available = typeof indexedDB != "undefined" && !!indexedDB;
    } catch (e) {
        available = false; // some browsers throw on access when storage is disabled
    }
    if (!available) {
        state.error = "this browser has no IndexedDB here";
        return 0;
    }
    var mount;
    try {
        mount = FS.mount(IDBFS, {}, state.dir).mount;
    } catch (e) {
        state.error = "cannot mount IDBFS at " + state.dir + ": " + e;
        return -1;
    }
    var start = Date.now();
    var result = await new Promise(function(done) {
        var settled = false;
        var timer = 0;
        function finish(code, error) {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            if (error) state.error = String(error);
            done(code);
        }
        timer =
            setTimeout(function() { finish(-2, "no answer from IndexedDB within " + timeoutMs + " ms"); }, timeoutMs);
        // IDBFS.syncfs(mount, true, ...) in steps, so that an answer after the time limit changes nothing (unless it
        // comes while the files are being copied in).
        try {
            IDBFS.getLocalSet(
                mount, function(error, local) {
                    if (error) return finish(-1, error);
                    IDBFS.getRemoteSet(
                        mount, function(error, remote) {
                            if (error) return finish(-1, error);
                            if (settled) return;
                            state.entries = Object.keys(remote.entries).length;
                            IDBFS.reconcile(remote, local, function(error) { finish(error ? -1 : 1, error); });
                        });
                });
        } catch (e) { finish(-1, e); }
    });
    state.ms = Date.now() - start;
    if (result != 1) {
        try {
            FS.unmount(state.dir);
        } catch (e) {}
        return result;
    }
    state.kept = true;
    return 1;
});

// Stores the directory in IndexedDB (IDBFS.syncfs(mount, false, ...)), one store at a time: a request while one runs
// runs again when it ends. A failure is logged once until a store succeeds again; it never fails the write.
EM_JS(void, quench_user_files_store, (), {
    var state = Module["quenchUserFiles"];
    if (!state || !state.kept) return;
    if (state.busy) {
        state.again = true;
        return;
    }
    var mount = FS.lookupPath(state.dir).node.mount;
    function run() {
        state.busy = true;
        state.again = false;
        IDBFS.syncfs(
            mount, false, function(error) {
                if (error && !state.failed) {
                    state.failed = true;
                    err("WARNING: fs: the user files were not stored in IndexedDB (kept until the page closes): " +
                        error);
                } else if (!error && state.failed) {
                    state.failed = false;
                    err("INFO: fs: the user files are stored in IndexedDB again");
                }
                if (state.again) {
                    run();
                } else {
                    state.busy = false;
                }
            });
    }
    run();
});
#endif

namespace quench::app {
namespace {

std::string withSeparator(std::string dir) {
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir;
}

Error sdlError(const std::string& what) { return Error{what + ": " + SDL_GetError()}; }

#if QUENCH_PLATFORM_WEB
// Deferred assets (quench_add_game's DEFERRED_ASSETS; docs/assets.md, "Deferred assets"). The build lists them in
// /quench/deferred_assets.txt inside the page's package: a line "folder <name>" (the folder beside the page that
// holds them, <OUTPUT_NAME>.assets), then a line "<size> <path>" per file. A request queues the file; at most
// kMaxDownloads are downloaded at a time with emscripten_async_wget2 (XMLHttpRequest, written into the virtual file
// system at the asset root when complete; its callbacks run on the main thread between frames), the highest
// priority first. A failed download is tried once more, then the file is Failed. One instance for the program's
// whole life: a download's callback may come after a backend was replaced.
class DeferredAssets {
public:
    static DeferredAssets& get() {
        static DeferredAssets* instance = new DeferredAssets; // never destroyed (callbacks hold pointers into it)
        return *instance;
    }

    // Reads the manifest once; `assetRoot` ends with '/'.
    void load(const std::string& assetRoot) {
        if (m_loaded) return;
        m_loaded = true;
        m_root = assetRoot;
        std::size_t length = 0;
        char* data = static_cast<char*>(SDL_LoadFile(kManifest, &length));
        if (!data) return; // no deferred assets in this build
        std::istringstream in(std::string(data, length));
        SDL_free(data);
        std::uint64_t bytes = 0;
        for (std::string line; std::getline(in, line);) {
            if (line.starts_with("folder ")) {
                m_folder = line.substr(7);
                continue;
            }
            const std::size_t space = line.find(' ');
            if (space == std::string::npos || line.starts_with("#")) continue;
            Entry entry;
            entry.size = std::strtoull(line.c_str(), nullptr, 10);
            std::string path = line.substr(space + 1);
            if (!fs::checkPath(path).empty()) continue;
            bytes += entry.size;
            m_entries.emplace(std::move(path), entry);
        }
        if (m_entries.empty() || m_folder.empty()) {
            m_entries.clear();
            return;
        }
        log::info("assets", "{} deferred assets ({}) beside the page in {}/, fetched when asked for", m_entries.size(),
                  sizeText(bytes), m_folder);
    }

    // Empty for a file that is not deferred.
    std::optional<fs::AssetState> state(std::string_view path) const {
        const auto it = m_entries.find(path);
        if (it == m_entries.end()) return std::nullopt;
        return it->second.state;
    }

    // False for a file that is not deferred.
    bool request(std::string_view path, int priority) {
        const auto it = m_entries.find(path);
        if (it == m_entries.end()) return false;
        Entry& entry = it->second;
        if (entry.state == fs::AssetState::Deferred) {
            entry.state = fs::AssetState::Fetching;
            entry.priority = priority;
            entry.order = m_nextOrder++;
            pump();
        } else if (entry.state == fs::AssetState::Fetching && !entry.active) {
            entry.priority = std::max(entry.priority, priority);
        }
        return true;
    }

private:
    static constexpr const char* kManifest = "/quench/deferred_assets.txt";
    static constexpr int kMaxDownloads = 2;
    static constexpr int kMaxTries = 2;

    struct Entry {
        std::uint64_t size = 0;
        fs::AssetState state = fs::AssetState::Deferred;
        int priority = 0;
        std::uint64_t order = 0; // when it was asked for: first among equal priorities
        int tries = 0;
        bool active = false;  // being downloaded
        double started = 0.0; // emscripten_get_now() of this try, ms
    };
    using Item = std::pair<const std::string, Entry>;

    static std::string sizeText(std::uint64_t bytes) {
        if (bytes >= 1000 * 1000) return std::format("{:.1f} MB", double(bytes) / 1e6);
        if (bytes >= 1000) return std::format("{:.1f} kB", double(bytes) / 1e3);
        return std::format("{} B", bytes);
    }

    // The URL relative to the page: the folder, then the path with everything but unreserved characters and '/'
    // percent-encoded.
    std::string url(const std::string& path) const {
        std::string out = m_folder + "/";
        for (const char c : path) {
            const unsigned char u = static_cast<unsigned char>(c);
            if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || c == '-' || c == '.' ||
                c == '_' || c == '~' || c == '/')
                out += c;
            else
                out += std::format("%{:02X}", unsigned(u));
        }
        return out;
    }

    // Starts the waiting downloads that fit.
    void pump() {
        while (m_active < kMaxDownloads) {
            Item* next = nullptr;
            for (Item& item : m_entries) {
                const Entry& e = item.second;
                if (e.state != fs::AssetState::Fetching || e.active) continue;
                if (!next || e.priority > next->second.priority ||
                    (e.priority == next->second.priority && e.order < next->second.order))
                    next = &item;
            }
            if (!next) return;
            start(*next);
        }
    }

    void start(Item& item) {
        Entry& entry = item.second;
        entry.active = true;
        ++entry.tries;
        entry.started = emscripten_get_now();
        ++m_active;
        const std::string file = m_root + item.first;
        emscripten_async_wget2(url(item.first).c_str(), file.c_str(), "GET", "", &item, &DeferredAssets::onLoad,
                               &DeferredAssets::onError, nullptr);
    }

    static void onLoad(unsigned handle, void* userdata, const char* file) {
        (void)handle;
        (void)file;
        Item& item = *static_cast<Item*>(userdata);
        Entry& entry = item.second;
        DeferredAssets& self = get();
        entry.active = false;
        --self.m_active;
        entry.state = fs::AssetState::Present;
        log::info("assets", "fetched {} ({}, {:.0f} ms)", item.first, sizeText(entry.size),
                  emscripten_get_now() - entry.started);
        self.pump();
    }

    static void onError(unsigned handle, void* userdata, int status) {
        (void)handle;
        Item& item = *static_cast<Item*>(userdata);
        Entry& entry = item.second;
        DeferredAssets& self = get();
        entry.active = false;
        --self.m_active;
        const std::string why = status > 0 ? std::format("HTTP {}", status) : std::string("no answer");
        if (entry.tries < kMaxTries) {
            log::info("assets", "{}: {}, trying once more", item.first, why);
        } else {
            entry.state = fs::AssetState::Failed;
            log::warning("assets", "{} could not be fetched ({}, {} tries): not there for this session", item.first,
                         why, entry.tries);
        }
        self.pump();
    }

    std::map<std::string, Entry, std::less<>> m_entries; // map nodes do not move: the callbacks keep pointers
    std::string m_root;
    std::string m_folder;
    std::uint64_t m_nextOrder = 0;
    int m_active = 0;
    bool m_loaded = false;
};
#endif

class SdlReader final : public fs::Reader {
public:
    SdlReader(SDL_IOStream* io, std::uint64_t size, std::string name)
        : m_io(io), m_size(size), m_name(std::move(name)) {}
    ~SdlReader() override { SDL_CloseIO(m_io); }
    SdlReader(const SdlReader&) = delete;
    SdlReader& operator=(const SdlReader&) = delete;

    std::uint64_t size() const override { return m_size; }

    Result<std::size_t> read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (offset >= m_size) return std::size_t(0);
        const std::size_t count = std::min<std::size_t>(out.size(), std::size_t(m_size - offset));
        if (SDL_SeekIO(m_io, Sint64(offset), SDL_IO_SEEK_SET) < 0) return sdlError("cannot seek " + m_name);
        std::size_t got = 0;
        while (got < count) {
            const std::size_t n = SDL_ReadIO(m_io, out.data() + got, count - got);
            if (n == 0) break;
            got += n;
        }
        if (got < count && SDL_GetIOStatus(m_io) == SDL_IO_STATUS_ERROR) return sdlError("cannot read " + m_name);
        return got;
    }

private:
    SDL_IOStream* m_io;
    std::uint64_t m_size;
    std::string m_name;
};

class SdlBackend final : public fs::Backend {
public:
    SdlBackend(std::string assetRoot, std::string userRoot, bool storeUserFiles)
        : m_assetRoot(std::move(assetRoot)), m_userRoot(std::move(userRoot)), m_storeUserFiles(storeUserFiles) {}

    Result<fs::Bytes> read(fs::Root root, std::string_view path) override {
        const std::string full = resolve(root, path);
        SDL_IOStream* io = SDL_IOFromFile(full.c_str(), "rb");
        if (!io) return sdlError("cannot open " + full);
        const Sint64 size = SDL_GetIOSize(io);
        if (size < 0) {
            // A stream of unknown size: read to the end.
            std::size_t length = 0;
            void* data = SDL_LoadFile_IO(io, &length, true);
            if (!data) return sdlError("cannot read " + full);
            fs::Bytes bytes(length);
            if (length > 0) std::memcpy(bytes.data(), data, length);
            SDL_free(data);
            return bytes;
        }
        fs::Bytes bytes(static_cast<std::size_t>(size));
        const std::size_t got = bytes.empty() ? 0 : SDL_ReadIO(io, bytes.data(), bytes.size());
        const bool closed = SDL_CloseIO(io);
        if (got != bytes.size() || !closed) return sdlError("cannot read " + full);
        return bytes;
    }

    Result<std::unique_ptr<fs::Reader>> open(fs::Root root, std::string_view path) override {
        const std::string full = resolve(root, path);
        SDL_IOStream* io = SDL_IOFromFile(full.c_str(), "rb");
        if (!io) return sdlError("cannot open " + full);
        const Sint64 size = SDL_GetIOSize(io);
        if (size < 0) {
            // A stream of unknown size cannot be read in pieces: read it whole.
            SDL_CloseIO(io);
            return fs::Backend::open(root, path);
        }
        return std::unique_ptr<fs::Reader>(std::make_unique<SdlReader>(io, std::uint64_t(size), full));
    }

    bool exists(fs::Root root, std::string_view path) override {
        // Opening works for APK assets too, where SDL_GetPathInfo does not.
        SDL_IOStream* io = SDL_IOFromFile(resolve(root, path).c_str(), "rb");
        if (!io) return false;
        SDL_CloseIO(io);
        return true;
    }

    Result<void> writeUser(std::string_view path, std::span<const std::uint8_t> data) override {
        const std::string full = resolve(fs::Root::User, path);
        const std::string parent = full.substr(0, full.find_last_of("/\\"));
        if (!SDL_CreateDirectory(parent.c_str())) return sdlError("cannot create " + parent);
        const std::string temp = full + ".tmp";
        SDL_IOStream* io = SDL_IOFromFile(temp.c_str(), "wb");
        if (!io) return sdlError("cannot open " + temp);
        const std::size_t written = data.empty() ? 0 : SDL_WriteIO(io, data.data(), data.size());
        const bool flushed = SDL_FlushIO(io);
        const bool closed = SDL_CloseIO(io);
        if (written != data.size() || !flushed || !closed) {
            Error error = sdlError("cannot write " + temp);
            SDL_RemovePath(temp.c_str());
            return error;
        }
        if (!SDL_RenamePath(temp.c_str(), full.c_str())) {
            Error error = sdlError("cannot replace " + full);
            SDL_RemovePath(temp.c_str());
            return error;
        }
#if QUENCH_PLATFORM_WEB
        if (m_storeUserFiles) quench_user_files_store();
#endif
        return {};
    }

    // SDL_GetPathInfo works on directories of the file system (the bundle, the user directory), not inside an APK.
    std::optional<std::int64_t> modifiedTime(fs::Root root, std::string_view path) override {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(resolve(root, path).c_str(), &info) || info.type != SDL_PATHTYPE_FILE) return std::nullopt;
        return std::int64_t(info.modify_time);
    }

    fs::AssetState assetState(std::string_view path) override {
#if QUENCH_PLATFORM_WEB
        if (const std::optional<fs::AssetState> state = DeferredAssets::get().state(path)) return *state;
#endif
        return exists(fs::Root::Assets, path) ? fs::AssetState::Present : fs::AssetState::Missing;
    }

    void requestAsset(std::string_view path, int priority) override {
#if QUENCH_PLATFORM_WEB
        DeferredAssets::get().request(path, priority);
#else
        (void)path;
        (void)priority;
#endif
    }

private:
    std::string resolve(fs::Root root, std::string_view path) const {
        return (root == fs::Root::Assets ? m_assetRoot : m_userRoot) + std::string(path);
    }

    std::string m_assetRoot; // both end with a separator
    std::string m_userRoot;
    [[maybe_unused]] bool m_storeUserFiles; // the web: the directory is kept in IndexedDB, stored after each write
};

#if QUENCH_PLATFORM_WEB
// The load of the stored user files at startup waits at most this long for IndexedDB (a blocked or very slow one);
// then the program starts with the directory in memory.
constexpr int kUserFilesLoadTimeoutMs = 5000;

// Mounts IDBFS at the default user directory and waits for its stored files (the program waits through Asyncify;
// this runs inside SDL_AppInit). True when the directory is kept in IndexedDB.
bool loadStoredUserFiles(const std::string& prefPath) {
    std::string dir = prefPath;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    const int result = quench_user_files_load(dir.c_str(), kUserFilesLoadTimeoutMs);
    const auto field = [](const char* name) {
        char* text = static_cast<char*>(EM_ASM_PTR(
            {
                const state = Module["quenchUserFiles"] || {};
                return stringToNewUTF8(String(state[UTF8ToString($0)]));
            },
            name));
        std::string value = text ? text : "";
        std::free(text);
        return value;
    };
    if (result == 1) {
        log::info("fs", "user files: kept in this browser (IndexedDB database {}, {} entries loaded in {} ms)", dir,
                  field("entries"), field("ms"));
        return true;
    }
    log::warning("fs", "user files: in memory only, lost when the page closes: {}", field("error"));
    return false;
}
#endif

} // namespace

Result<std::unique_ptr<fs::Backend>> makeSdlFileSystem(const SdlFileSystemDesc& desc) {
    if (!desc.assetDir.empty()) {
        if (std::string reason = fs::checkPath(desc.assetDir); !reason.empty())
            return Error{"asset directory: " + reason + ": " + desc.assetDir};
    }
    const char* base = SDL_GetBasePath();
    if (!base) return sdlError("no base path");
    std::string assetRoot = withSeparator(std::string(base) + desc.assetDir);

    std::string userRoot = desc.userDir;
    bool storeUserFiles = false;
    if (userRoot.empty()) {
        if (desc.app.empty()) return Error{"the user directory needs an app name"};
        char* pref = SDL_GetPrefPath(desc.org.c_str(), desc.app.c_str());
        if (!pref) return sdlError("no user directory");
        userRoot = pref;
        SDL_free(pref);
#if QUENCH_PLATFORM_WEB
        storeUserFiles = loadStoredUserFiles(userRoot);
#endif
    } else if (!SDL_CreateDirectory(userRoot.c_str())) {
        return sdlError("cannot create " + userRoot);
    }
#if QUENCH_PLATFORM_WEB
    DeferredAssets::get().load(assetRoot);
#endif
    return std::unique_ptr<fs::Backend>(
        std::make_unique<SdlBackend>(std::move(assetRoot), withSeparator(std::move(userRoot)), storeUserFiles));
}

} // namespace quench::app
