#include "Test.h"

#include <quench/app/SdlFileSystem.h>
#include <quench/core/FileSystem.h>

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace quench;

namespace {

// Assets live below the executable's directory (SDL_GetBasePath()); each case uses its own directories.
std::filesystem::path baseDir() {
    const char* base = SDL_GetBasePath();
    return base ? std::filesystem::path(base) : std::filesystem::path();
}

std::filesystem::path freshDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

struct BackendScope {
    ~BackendScope() { fs::setBackend(nullptr); }
};

} // namespace

QUENCH_TEST(sdl_fs_assets) {
    const std::filesystem::path assets = freshDir(baseDir() / "sdl_fs_assets");
    std::filesystem::create_directories(assets / "dir");
    std::ofstream(assets / "dir/a.txt") << "asset";

    Result<std::unique_ptr<fs::Backend>> backend = app::makeSdlFileSystem(
        {.assetDir = "sdl_fs_assets", .userDir = std::string(QUENCH_TEST_TMP_DIR) + "/sdl_fs_assets_user"});
    REQUIRE(backend, "%s", backend.error().c_str());
    BackendScope scope;
    fs::setBackend(std::move(backend).value());

    Result<std::string> text = fs::readAssetText("dir/a.txt");
    CHECK(text && *text == "asset", "read: %s", text ? text->c_str() : text.error().c_str());
    CHECK(fs::assetExists("dir/a.txt"), "exists");
    CHECK(!fs::assetExists("dir/missing.txt"), "a missing file exists");
    Result<fs::Bytes> missing = fs::readAsset("dir/missing.txt");
    CHECK(!missing && missing.error().find("dir/missing.txt") != std::string::npos, "missing: %s",
          missing ? "read" : missing.error().c_str());
    CHECK(!fs::readAsset("../sdl_fs_assets/dir/a.txt"), "'..' accepted");
    CHECK(!fs::userExists("dir/a.txt"), "assets visible as user files");
}

QUENCH_TEST(sdl_fs_user_files) {
    const std::filesystem::path user = freshDir(std::filesystem::path(QUENCH_TEST_TMP_DIR) / "sdl_fs_user");
    Result<std::unique_ptr<fs::Backend>> backend = app::makeSdlFileSystem({.userDir = user.string()});
    REQUIRE(backend, "%s", backend.error().c_str());
    BackendScope scope;
    fs::setBackend(std::move(backend).value());

    CHECK(!fs::userExists("saves/slot1.json"), "exists before the write");
    Result<void> written = fs::writeUserText("saves/slot1.json", "{}");
    REQUIRE(written, "write: %s", written.error().c_str());
    written = fs::writeUserText("saves/slot1.json", "{\"level\": 2}");
    REQUIRE(written, "overwrite: %s", written.error().c_str());
    Result<std::string> text = fs::readUserText("saves/slot1.json");
    CHECK(text && *text == "{\"level\": 2}", "read: %s", text ? text->c_str() : text.error().c_str());
    CHECK(fs::userExists("saves/slot1.json"), "exists");
    CHECK(!std::filesystem::exists(user / "saves/slot1.json.tmp"), "temporary file left behind");

    written = fs::writeUserText("empty.txt", "");
    REQUIRE(written, "empty write: %s", written.error().c_str());
    Result<fs::Bytes> empty = fs::readUser("empty.txt");
    CHECK(empty && empty->empty(), "empty file");
}

QUENCH_TEST(sdl_fs_rejects_bad_setup) {
    CHECK(!app::makeSdlFileSystem({.assetDir = "../assets", .userDir = QUENCH_TEST_TMP_DIR}), "'..' asset dir");
    CHECK(!app::makeSdlFileSystem({}), "no user directory and no org/app");
}
