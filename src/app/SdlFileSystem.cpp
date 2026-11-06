#include <quench/app/SdlFileSystem.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace quench::app {
namespace {

std::string withSeparator(std::string dir) {
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir;
}

Error sdlError(const std::string& what) { return Error{what + ": " + SDL_GetError()}; }

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
    SdlBackend(std::string assetRoot, std::string userRoot)
        : m_assetRoot(std::move(assetRoot)), m_userRoot(std::move(userRoot)) {}

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
        return {};
    }

    // SDL_GetPathInfo works on directories of the file system (the bundle, the user directory), not inside an APK.
    std::optional<std::int64_t> modifiedTime(fs::Root root, std::string_view path) override {
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(resolve(root, path).c_str(), &info) || info.type != SDL_PATHTYPE_FILE) return std::nullopt;
        return std::int64_t(info.modify_time);
    }

private:
    std::string resolve(fs::Root root, std::string_view path) const {
        return (root == fs::Root::Assets ? m_assetRoot : m_userRoot) + std::string(path);
    }

    std::string m_assetRoot; // both end with a separator
    std::string m_userRoot;
};

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
    if (userRoot.empty()) {
        if (desc.app.empty()) return Error{"the user directory needs an app name"};
        char* pref = SDL_GetPrefPath(desc.org.c_str(), desc.app.c_str());
        if (!pref) return sdlError("no user directory");
        userRoot = pref;
        SDL_free(pref);
    } else if (!SDL_CreateDirectory(userRoot.c_str())) {
        return sdlError("cannot create " + userRoot);
    }
    return std::unique_ptr<fs::Backend>(
        std::make_unique<SdlBackend>(std::move(assetRoot), withSeparator(std::move(userRoot))));
}

} // namespace quench::app
