#include "Test.h"

#include <quench/core/FileSystem.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace quench;

namespace {

// A clean directory per test case (CTest may run cases in parallel), with assets/ and user/ under it.
std::filesystem::path testDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::path(quench::test::tmpDir()) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "assets" / "models");
    return dir;
}

} // namespace

QUENCH_TEST(fs_check_path) {
    for (const char* bad : {"", "/etc/passwd", "../x", "a/../b", "a//b", "./a", "a/", "a\\b", "C:/x"})
        CHECK(!fs::checkPath(bad).empty(), "'%s' accepted", bad);
    for (const char* good : {"a", "models/rock.glb", "a.b/c_d-e.txt", ".hidden"})
        CHECK(fs::checkPath(good).empty(), "'%s' refused: %s", good, fs::checkPath(good).c_str());
}

QUENCH_TEST(fs_read_asset) {
    const std::filesystem::path dir = testDir("fs_read_asset");
    std::ofstream(dir / "assets" / "models" / "rock.txt", std::ios::binary) << "granite\n";
    fs::setBackend(fs::makeStdBackend(dir / "assets", {}));
    const Result<std::string> text = fs::readAssetText("models/rock.txt");
    CHECK(text.ok() && *text == "granite\n", "read: %s", text.ok() ? text->c_str() : text.error().c_str());
    CHECK(fs::assetExists("models/rock.txt") && !fs::assetExists("models/none.txt"), "assetExists");
    const Result<fs::Bytes> missing = fs::readAsset("models/none.txt");
    CHECK(!missing && missing.error().find("none.txt") != std::string::npos, "error '%s'", missing.error().c_str());
    const Result<fs::Bytes> escape = fs::readAsset("../user/x");
    CHECK(!escape && escape.error().find("'..'") != std::string::npos, "error '%s'", escape.error().c_str());
    fs::setBackend(nullptr);
}

QUENCH_TEST(fs_open_asset) {
    const std::filesystem::path dir = testDir("fs_open_asset");
    std::ofstream(dir / "assets" / "models" / "rock.txt", std::ios::binary) << "granite\n";
    fs::setBackend(fs::makeStdBackend(dir / "assets", {}));
    Result<std::unique_ptr<fs::Reader>> reader = fs::openAsset("models/rock.txt");
    REQUIRE(reader, "open: %s", reader.error().c_str());
    CHECK((*reader)->size() == 8, "size %llu", (unsigned long long)(*reader)->size());
    std::uint8_t piece[4] = {};
    Result<std::size_t> got = (*reader)->read(4, piece);
    CHECK(got && *got == 4 && std::string(piece, piece + 4) == "ite\n", "read at 4");
    got = (*reader)->read(1, std::span(piece, 2));
    CHECK(got && *got == 2 && std::string(piece, piece + 2) == "ra", "read at 1 after a later read");
    got = (*reader)->read(7, piece);
    CHECK(got && *got == 1, "short read at the end: %zu", got ? *got : 0);
    got = (*reader)->read(100, piece);
    CHECK(got && *got == 0, "read past the end");
    CHECK(!fs::openAsset("models/none.txt"), "a missing file opened");
    CHECK(!fs::openAsset("../x"), "'..' accepted");

    std::unique_ptr<fs::Reader> memory = fs::makeMemoryReader({1, 2, 3});
    got = memory->read(1, piece);
    CHECK(memory->size() == 3 && got && *got == 2 && piece[0] == 2 && piece[1] == 3, "memory reader");
    fs::setBackend(nullptr);
}

QUENCH_TEST(fs_write_user) {
    const std::filesystem::path dir = testDir("fs_write_user");
    fs::setBackend(fs::makeStdBackend(dir / "assets", dir / "user"));
    CHECK(fs::writeUserText("saves/slot1.json", "{\"v\": 1}").ok(), "first write");
    CHECK(fs::writeUserText("saves/slot1.json", "{\"v\": 2}").ok(), "overwrite");
    const Result<std::string> text = fs::readUserText("saves/slot1.json");
    CHECK(text.ok() && *text == "{\"v\": 2}", "read back: %s", text.ok() ? text->c_str() : text.error().c_str());
    CHECK(!std::filesystem::exists(dir / "user" / "saves" / "slot1.json.tmp"), "temporary file left");
    CHECK(fs::userExists("saves/slot1.json") && !fs::assetExists("saves/slot1.json"), "roots mixed up");
    fs::setBackend(nullptr);
}

QUENCH_TEST(fs_no_user_dir) {
    fs::setBackend(nullptr);
    const Result<void> r = fs::writeUserText("x.txt", "x");
    CHECK(!r && r.error() == "no user directory", "error '%s'", r.error().c_str());
}
