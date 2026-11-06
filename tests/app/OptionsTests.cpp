#include "Test.h"

#include "app/Options.h"

#include <quench/core/Parse.h>

#include <string_view>
#include <vector>

using namespace quench;
using app::detail::parseOptions;

namespace {

Result<app::detail::Options> parse(std::vector<std::string_view> args) { return parseOptions(args); }

} // namespace

QUENCH_TEST(options_engine_flags) {
    const auto options = parse({"--quit-after", "2.5", "--capture=shot.png", "--capture-at", "1", "--script", "s.txt",
                                "--window", "800x600", "--frame-size=1280x720", "--gpu-driver", "vulkan", "--gpu-debug",
                                "--present-mode", "mailbox"});
    REQUIRE(options.ok(), "%s", options.error().c_str());
    CHECK(options->quitAfter == 2.5, "%f", options->quitAfter);
    CHECK(options->capture == "shot.png", "%s", options->capture.c_str());
    CHECK(options->captureAt == 1.0, "%f", options->captureAt);
    CHECK(options->script == "s.txt", "%s", options->script.c_str());
    CHECK(options->windowWidth == 800 && options->windowHeight == 600, "%dx%d", options->windowWidth,
          options->windowHeight);
    CHECK(options->frameWidth == 1280 && options->frameHeight == 720, "%dx%d", options->frameWidth,
          options->frameHeight);
    CHECK(options->gpuDriver == "vulkan", "%s", options->gpuDriver.c_str());
    CHECK(!options->gpuBackend, "no backend given: AppDesc's");
    CHECK(options->gpuDebug, "gpu debug");
    CHECK(options->presentMode == gpu::PresentMode::Mailbox, "present mode");
    CHECK(options->deterministic(), "script and capture runs are deterministic");
    CHECK(options->gameArgs.empty(), "%zu game args", options->gameArgs.size());
}

QUENCH_TEST(options_audio_flags) {
    const auto none = parse({});
    REQUIRE(none.ok(), "%s", none.error().c_str());
    CHECK(!none->noAudio && !none->audioDevice && none->audioCapture.empty(), "a device by default");
    const auto options = parse({"--no-audio", "--audio-device", "null", "--audio-capture=mix.wav"});
    REQUIRE(options.ok(), "%s", options.error().c_str());
    CHECK(options->noAudio, "--no-audio");
    CHECK(options->audioDevice == "null", "--audio-device");
    CHECK(options->audioCapture == "mix.wav", "%s", options->audioCapture.c_str());
    CHECK(!options->deterministic(), "an audio capture alone does not step the app time");
    CHECK(!parse({"--audio-device"}).ok() && !parse({"--audio-device="}).ok(), "--audio-device needs a name");
    CHECK(!parse({"--audio-capture"}).ok(), "--audio-capture needs a path");
    CHECK(parse({"--no-audio=1"})->gameArgs.size() == 1, "--no-audio takes no value");
}

QUENCH_TEST(options_defaults_and_game_args) {
    const auto options = parse({"--level", "two", "--no-intro"});
    REQUIRE(options.ok(), "%s", options.error().c_str());
    CHECK(options->quitAfter < 0.0, "no quit by default");
    CHECK(options->captureAt == 3.0, "%f", options->captureAt);
    CHECK(!options->deterministic(), "interactive by default");
    REQUIRE(options->gameArgs.size() == 3, "%zu", options->gameArgs.size());
    CHECK(options->gameArgs[0] == "--level" && options->gameArgs[1] == "two" && options->gameArgs[2] == "--no-intro",
          "game args in order");
}

QUENCH_TEST(options_errors) {
    CHECK(!parse({"--quit-after"}).ok(), "missing value");
    CHECK(!parse({"--quit-after", "soon"}).ok(), "not a number");
    CHECK(!parse({"--quit-after", "-1"}).ok(), "negative seconds");
    CHECK(!parse({"--capture-at", "1s"}).ok(), "suffix");
    CHECK(!parse({"--window", "800"}).ok(), "no height");
    CHECK(!parse({"--window", "0x600"}).ok(), "zero width");
    CHECK(!parse({"--frame-size", "1280x720x2"}).ok(), "three numbers");
    CHECK(!parse({"--present-mode", "fast"}).ok(), "unknown present mode");
    CHECK(parse({"--gpu-backend", "metal"})->gpuBackend == gpu::Backend::Metal, "--gpu-backend metal");
    CHECK(parse({"--gpu-backend=sdl"})->gpuBackend == gpu::Backend::SdlGpu, "--gpu-backend=sdl");
    CHECK(!parse({"--gpu-backend", "vulkan"}).ok(), "unknown backend");
    CHECK(parse({"--orientation", "portrait"})->orientation == "Portrait", "--orientation portrait");
    CHECK(parse({"--orientation=landscape"})->orientation == "LandscapeLeft LandscapeRight", "--orientation landscape");
    CHECK(parse({})->orientation.empty(), "the app's orientations by default");
    CHECK(!parse({"--orientation", "sideways"}).ok(), "unknown orientation");
    CHECK(parse({"--user-dir", "/tmp/user"})->userDir == "/tmp/user", "--user-dir");
    CHECK(!parse({"--user-dir="}).ok(), "--user-dir needs a directory");
    CHECK(!parse({"--capture="}).ok(), "empty path");
    CHECK(parse({"--version"})->printVersion, "--version");
    CHECK(parse({"--help"})->printHelp, "--help");
}

QUENCH_TEST(parse_numbers) {
    CHECK(parseDouble("1.5") == 1.5, "1.5");
    CHECK(parseDouble("-2e3") == -2000.0, "exponent");
    CHECK(!parseDouble(""), "empty");
    CHECK(!parseDouble(" 1"), "leading space");
    CHECK(!parseDouble("1 "), "trailing space");
    CHECK(!parseDouble("nan"), "nan");
    CHECK(!parseDouble("inf"), "inf");
    CHECK(!parseDouble("1e999"), "overflow");
    CHECK(parseInt("42") == 42, "42");
    CHECK(parseInt("-7") == -7, "-7");
    CHECK(!parseInt("4.2"), "fraction");
    CHECK(!parseInt("99999999999999999999"), "overflow");
}
