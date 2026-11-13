#pragma once

#include <quench/core/Result.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// All runtime file access: read-only assets and a writable user directory. Paths are relative to their root, use '/'
// and contain no "." or ".." segments; anything else is refused with an error. The default backend uses the C++
// standard library (host tests and tools); the app installs one on SDL_IOStream, which also reads from the app
// bundle, the APK and the browser's file system.
namespace quench::fs {

enum class Root { Assets, User };

using Bytes = std::vector<std::uint8_t>;

// Where an asset is (assetState). Deferred and Fetching exist only in a browser build, for the files a game's
// quench_add_game(... DEFERRED_ASSETS ...) left out of the page's package (docs/assets.md, "Deferred assets").
enum class AssetState {
    Missing,  // not there and not to be fetched
    Present,  // there: readAsset, openAsset and assetExists see it
    Deferred, // on the server beside the page, not asked for yet (requestAsset)
    Fetching, // asked for: waiting in the queue or being downloaded
    Failed,   // its download failed twice: not there for the rest of the session
};

// A file opened for reading in pieces (streamed audio). One thread at a time, which may change between calls (a job,
// then the next job).
class Reader {
public:
    virtual ~Reader() = default;
    virtual std::uint64_t size() const = 0;
    // Up to `out.size()` bytes from `offset`: the count read, 0 at or past the end.
    virtual Result<std::size_t> read(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
};

// A reader over bytes in memory.
std::unique_ptr<Reader> makeMemoryReader(Bytes bytes);

class Backend {
public:
    virtual ~Backend() = default;
    // `path` is already validated. Implementations must be safe to call from several threads.
    virtual Result<Bytes> read(Root root, std::string_view path) = 0;
    // Opens a file for reading in pieces. The default reads the whole file into memory.
    virtual Result<std::unique_ptr<Reader>> open(Root root, std::string_view path) {
        Result<Bytes> bytes = read(root, path);
        if (!bytes) return Error{bytes.error()};
        return makeMemoryReader(std::move(bytes).value());
    }
    virtual bool exists(Root root, std::string_view path) = 0;
    // Replaces the file as a whole (written next to it, then renamed), creating parent directories.
    virtual Result<void> writeUser(std::string_view path, std::span<const std::uint8_t> data) = 0;
    // Last modification time in nanoseconds of an unspecified epoch, for change detection (hot reload); empty when
    // the file does not exist or the backend cannot tell.
    virtual std::optional<std::int64_t> modifiedTime(Root root, std::string_view path) {
        (void)root;
        (void)path;
        return std::nullopt;
    }
    // Deferred assets (fs::assetState, fs::requestAsset). The defaults have none: a file that exists is Present,
    // anything else Missing, and a request does nothing.
    virtual AssetState assetState(std::string_view path) {
        return exists(Root::Assets, path) ? AssetState::Present : AssetState::Missing;
    }
    virtual void requestAsset(std::string_view path, int priority) {
        (void)path;
        (void)priority;
    }
};

// Standard library backend; an empty root is the current directory for assets and refuses user writes.
std::unique_ptr<Backend> makeStdBackend(std::filesystem::path assetRoot, std::filesystem::path userRoot);

// Development builds: an asset whose path exists below `overlayDir` in the user directory is read from there instead
// of `inner`'s assets (files copied onto a device replace bundled ones, and hot reload sees their times). User files
// are `inner`'s. `overlayDir` is a relative path (fs::checkPath).
std::unique_ptr<Backend> makeOverlayBackend(std::unique_ptr<Backend> inner, std::string overlayDir);

// Installs a backend (nullptr restores the default standard library backend over the current directory, without a
// user directory). Not thread-safe: call before any file access, at startup.
void setBackend(std::unique_ptr<Backend> backend);

// Empty when `path` is acceptable, else the reason.
std::string checkPath(std::string_view path);

Result<Bytes> readAsset(std::string_view path);
Result<std::string> readAssetText(std::string_view path);
Result<std::unique_ptr<Reader>> openAsset(std::string_view path);
bool assetExists(std::string_view path);
std::optional<std::int64_t> assetModifiedTime(std::string_view path);

// Deferred assets: in a browser build, files that quench_add_game(... DEFERRED_ASSETS ...) put beside the page
// instead of into its package are downloaded while the program runs, into the assets, after which every function
// above sees them. On every other platform (and for any other file) an asset that exists is Present, anything else
// Missing, and requestAsset does nothing. A browser build fetches at most two files at a time, the highest
// `priority` first (in the order asked among equal ones), tries a failed download once more and then reports
// Failed; asking again raises a waiting file's priority. Call both from the main thread; neither waits.
AssetState assetState(std::string_view path);
void requestAsset(std::string_view path, int priority = 0);

Result<Bytes> readUser(std::string_view path);
Result<std::string> readUserText(std::string_view path);
bool userExists(std::string_view path);
Result<void> writeUser(std::string_view path, std::span<const std::uint8_t> data);
Result<void> writeUserText(std::string_view path, std::string_view text);

} // namespace quench::fs
