#include "Options.h"

#include <quench/core/Parse.h>

#include <format>

namespace quench::app::detail {
namespace {

std::optional<double> parseSeconds(std::string_view text) {
    const auto value = parseDouble(text);
    if (!value || *value < 0.0) return std::nullopt;
    return value;
}

bool parseSize(std::string_view text, int& width, int& height) {
    const auto x = text.find('x');
    if (x == std::string_view::npos) return false;
    const auto w = parseInt(text.substr(0, x)), h = parseInt(text.substr(x + 1));
    if (!w || !h || *w < 1 || *h < 1 || *w > 16384 || *h > 16384) return false;
    width = int(*w);
    height = int(*h);
    return true;
}

} // namespace

std::optional<HdrMode> parseHdrMode(std::string_view text) {
    if (text == "off") return HdrMode::Off;
    if (text == "on") return HdrMode::On;
    if (text == "auto") return HdrMode::Auto;
    return std::nullopt;
}

const char* optionsHelp() {
    return "Engine options:\n"
           "  --quit-after S       quit after S seconds of app time\n"
           "  --capture FILE.png   save one frame (with ImGui) as PNG; relative paths go to the user directory\n"
           "  --capture-at S       app time of the capture (default 3)\n"
           "  --script FILE        run a verification scenario; the exit code is 1 if a check failed. A host\n"
           "                       path; a relative path not found there is read from the user directory\n"
           "  --window WxH         window size in points (desktop)\n"
           "  --frame-size WxH     render every frame offscreen at W x H pixels and scale it to the window, so\n"
           "                       captures do not depend on the display density\n"
           "  --gpu-driver NAME    metal, vulkan or direct3d12\n"
           "  --gpu-debug          GPU validation (backend debug mode)\n"
           "  --present-mode MODE  vsync, immediate or mailbox\n"
           "  --hdr MODE           HDR output: off (SDR), on (where the GPU backend supports it) or auto (while\n"
           "                       the display has HDR range); default: the game's choice, normally off\n"
           "  --hdr-headroom N     peak brightness of HDR output in multiples of paper white (N >= 1) instead of\n"
           "                       the display's (repeatable HDR captures)\n"
           "  --worker-threads N   worker threads of the job system (default: hardware threads - 1; 0: jobs run on\n"
           "                       the main thread, as in builds without threads)\n"
           "  --asset-latency S    every asset load waits S seconds before it starts (to see the placeholders)\n"
           "  --hot-reload         load asset files again when they change (cooked files in the build tree)\n"
           "  --version            print the version and exit\n"
           "  --help               print this help and exit\n"
           "With --script or --capture every frame advances the app time by one fixed step (deterministic runs),\n"
           "never faster than real time; a scenario's perf_begin .. perf_end region runs in real time.\n";
}

Result<Options> parseOptions(std::span<const std::string_view> args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view flag = args[i];
        std::optional<std::string_view> inlineValue;
        if (flag.starts_with("--")) {
            if (const auto eq = flag.find('='); eq != std::string_view::npos) {
                inlineValue = flag.substr(eq + 1);
                flag = flag.substr(0, eq);
            }
        }
        const auto value = [&]() -> Result<std::string_view> {
            if (inlineValue) return *inlineValue;
            if (i + 1 >= args.size()) return Error{std::format("{} needs a value", flag)};
            return args[++i];
        };
        const auto seconds = [&](double& out) -> Result<void> {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            const auto parsed = parseSeconds(*v);
            if (!parsed) return Error{std::format("{}: '{}' is not a number of seconds", flag, *v)};
            out = *parsed;
            return {};
        };

        if (flag == "--quit-after") {
            if (Result<void> r = seconds(options.quitAfter); !r) return Error{r.error()};
        } else if (flag == "--capture-at") {
            if (Result<void> r = seconds(options.captureAt); !r) return Error{r.error()};
        } else if (flag == "--capture" || flag == "--script" || flag == "--gpu-driver") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            if (v->empty()) return Error{std::format("{} needs a value", flag)};
            (flag == "--capture" ? options.capture : flag == "--script" ? options.script : options.gpuDriver) = *v;
        } else if (flag == "--window" || flag == "--frame-size") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            const bool window = flag == "--window";
            if (!parseSize(*v, window ? options.windowWidth : options.frameWidth,
                           window ? options.windowHeight : options.frameHeight))
                return Error{std::format("{}: '{}' is not WIDTHxHEIGHT", flag, *v)};
        } else if (flag == "--present-mode") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            if (*v == "vsync")
                options.presentMode = gpu::PresentMode::Vsync;
            else if (*v == "immediate")
                options.presentMode = gpu::PresentMode::Immediate;
            else if (*v == "mailbox")
                options.presentMode = gpu::PresentMode::Mailbox;
            else
                return Error{std::format("--present-mode: '{}' is not vsync, immediate or mailbox", *v)};
        } else if (flag == "--hdr") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            const std::optional<HdrMode> mode = parseHdrMode(*v);
            if (!mode) return Error{std::format("--hdr: '{}' is not off, on or auto", *v)};
            options.hdr = *mode;
        } else if (flag == "--hdr-headroom") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            const auto headroom = parseDouble(*v);
            if (!headroom || *headroom < 1.0 || *headroom > 1000.0)
                return Error{std::format("--hdr-headroom: '{}' is not a number from 1 to 1000", *v)};
            options.hdrHeadroom = float(*headroom);
        } else if (flag == "--worker-threads") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            const auto count = parseDouble(*v);
            if (!count || *count < 0.0 || *count > 64.0 || *count != double(int(*count)))
                return Error{std::format("--worker-threads: '{}' is not a whole number from 0 to 64", *v)};
            options.workerThreads = int(*count);
        } else if (flag == "--asset-latency") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            const auto seconds = parseDouble(*v);
            if (!seconds || *seconds < 0.0 || *seconds > 60.0)
                return Error{std::format("--asset-latency: '{}' is not a number of seconds from 0 to 60", *v)};
            options.assetLatency = *seconds;
        } else if (flag == "--hot-reload" && !inlineValue) {
            options.hotReload = true;
        } else if (flag == "--gpu-debug" && !inlineValue) {
            options.gpuDebug = true;
        } else if (flag == "--version" && !inlineValue) {
            options.printVersion = true;
        } else if ((flag == "--help" || flag == "-h") && !inlineValue) {
            options.printHelp = true;
        } else {
            options.gameArgs.emplace_back(args[i]);
        }
    }
    return options;
}

} // namespace quench::app::detail
