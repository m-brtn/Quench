#pragma once

// The quench::fs backend on SDL_IOStream. Assets are read from SDL_GetBasePath(): the bundle's resources on Apple
// platforms, the APK on Android, the preloaded file system on the web. User files go to SDL_GetPrefPath(org, app).

#include <quench/core/FileSystem.h>
#include <quench/core/Result.h>

#include <memory>
#include <string>

namespace quench::app {

struct SdlFileSystemDesc {
    std::string org;      // for SDL_GetPrefPath
    std::string app;      // for SDL_GetPrefPath
    std::string assetDir; // directory of the assets below SDL_GetBasePath(); empty: the base path itself
    std::string userDir;  // empty: SDL_GetPrefPath(org, app), created if missing; else this directory (tests, tools)
};

Result<std::unique_ptr<fs::Backend>> makeSdlFileSystem(const SdlFileSystemDesc& desc);

} // namespace quench::app
