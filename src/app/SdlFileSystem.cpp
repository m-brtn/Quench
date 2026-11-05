#include <quench/app/SdlFileSystem.h>

#include <SDL3/SDL.h>

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
