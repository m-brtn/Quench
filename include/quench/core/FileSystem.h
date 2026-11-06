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
};

// Standard library backend; an empty root is the current directory for assets and refuses user writes.
std::unique_ptr<Backend> makeStdBackend(std::filesystem::path assetRoot, std::filesystem::path userRoot);

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

Result<Bytes> readUser(std::string_view path);
Result<std::string> readUserText(std::string_view path);
bool userExists(std::string_view path);
Result<void> writeUser(std::string_view path, std::span<const std::uint8_t> data);
Result<void> writeUserText(std::string_view path, std::string_view text);

} // namespace quench::fs
