#include "Options.h"
#include "ThermalState.h"

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
           "  --offscreen          no window on the screen: a hidden window, frames rendered offscreen (at\n"
           "                       --frame-size, else at the window size in points), no Dock icon, no focus; the\n"
           "                       default on desktops with --script, --capture or --quit-after\n"
           "  --onscreen           a visible window also with --script, --capture or --quit-after (benchmarks)\n"
           "  --gpu-backend NAME   metal (the native Metal backend: Apple platforms, the default there; elsewhere\n"
           "                       SDL GPU) or sdl (SDL GPU, the default elsewhere); a native device that fails\n"
           "                       to start falls back to SDL GPU\n"
           "  --gpu-driver NAME    metal, vulkan or direct3d12 (SDL GPU's driver; vulkan and direct3d12 select\n"
           "                       SDL GPU)\n"
           "  --gpu-debug          GPU validation (backend debug mode)\n"
           "  --gpu-pass-timing    GPU time per pass in every frame (native Metal backend); otherwise only while the\n"
           "                       game shows it or a scenario's perf region runs\n"
           "  --present-mode MODE  vsync, immediate or mailbox\n"
           "  --max-frame-rate HZ  at most HZ frames per second on the screen, whole display refreshes (60 on a\n"
           "                       120 Hz display: every second one); 0: the display's rate (the default)\n"
           "  --thermal-state NAME nominal, fair, serious, critical or unknown in place of the system's thermal\n"
           "                       state, to check a game's reaction to heat (App::thermalGovernor) without a hot\n"
           "                       device; the scenario command thermal_state changes it, `thermal_state system`\n"
           "                       ends it\n"
           "  --hdr MODE           HDR output: off (SDR), on (where the GPU backend supports it) or auto (while\n"
           "                       the display has HDR range); default: the game's choice, normally off\n"
           "  --hdr-headroom N     peak brightness of HDR output in multiples of paper white (N >= 1) instead of\n"
           "                       the display's (repeatable HDR captures)\n"
           "  --orientation NAME   portrait, landscape or any: the screen orientations of the window on phones and\n"
           "                       tablets (default: the app's); ignored on desktops\n"
           "  --worker-threads N   worker threads of the job system (default: hardware threads - 1; 0: jobs run on\n"
           "                       the main thread, as in builds without threads)\n"
           "  --asset-latency S    every asset load waits S seconds before it starts (to see the placeholders)\n"
           "  --hot-reload         load asset files again when they change (cooked files in the build tree, an\n"
           "                       overlay on a device); on desktops also shaders (their sources in the build)\n"
           "  --no-audio           no audio output: the mix runs offline on the frame clock and is discarded\n"
           "  --audio-device NAME  the output whose name contains NAME; default: the system's; null: a device\n"
           "                       that discards the mix\n"
           "  --audio-capture FILE.wav  no audio output: the mix runs offline on the frame clock (with --script or\n"
           "                       --capture: exactly one fixed step per frame) and is written as a 16-bit WAV at\n"
           "                       the end; relative paths go to the user directory\n"
           "  --user-dir DIR       the user directory (settings, saves, captures) instead of the system's, created if\n"
           "                       missing (tests and scripted runs)\n"
           "  --version            print the version and exit\n"
           "  --help               print this help and exit\n"
           "With --script or --capture every frame advances the app time by one fixed step (deterministic runs):\n"
           "on the screen never faster than real time, offscreen as fast as frames render (in real time while\n"
           "assets load); a scenario's perf_begin .. perf_end region runs in real time.\n"
           "Runs with --script take input only from the script, offscreen runs none from real devices: keyboard,\n"
           "mouse, touch and gamepads are ignored. Offscreen the mix runs offline (no sound) unless --audio-device\n"
           "names a device.\n";
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
        } else if (flag == "--capture" || flag == "--script" || flag == "--gpu-driver" || flag == "--audio-capture" ||
                   flag == "--user-dir") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            if (v->empty()) return Error{std::format("{} needs a value", flag)};
            (flag == "--capture"         ? options.capture
             : flag == "--script"        ? options.script
             : flag == "--audio-capture" ? options.audioCapture
             : flag == "--user-dir"      ? options.userDir
                                         : options.gpuDriver) = *v;
        } else if (flag == "--audio-device") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            if (v->empty()) return Error{std::format("{} needs a value", flag)};
            options.audioDevice = std::string(*v);
        } else if (flag == "--no-audio" && !inlineValue) {
            options.noAudio = true;
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
        } else if (flag == "--max-frame-rate") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            const auto hz = parseDouble(*v);
            if (!hz || *hz < 0.0 || *hz > 1000.0)
                return Error{std::format("--max-frame-rate: '{}' is not a number from 0 to 1000", *v)};
            options.maxFrameRate = *hz;
        } else if (flag == "--thermal-state") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            options.thermalState = parseThermalState(*v);
            if (!options.thermalState)
                return Error{
                    std::format("--thermal-state: '{}' is not nominal, fair, serious, critical or unknown", *v)};
        } else if (flag == "--gpu-backend") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            if (*v == "sdl")
                options.gpuBackend = gpu::Backend::SdlGpu;
            else if (*v == "metal")
                options.gpuBackend = gpu::Backend::Metal;
            else
                return Error{std::format("--gpu-backend: '{}' is not sdl or metal", *v)};
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
        } else if (flag == "--orientation") {
            Result<std::string_view> v = value();
            if (!v) return Error{v.error()};
            if (*v == "portrait")
                options.orientation = "Portrait";
            else if (*v == "landscape")
                options.orientation = "LandscapeLeft LandscapeRight";
            else if (*v == "any")
                options.orientation = "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown";
            else
                return Error{std::format("--orientation: '{}' is not portrait, landscape or any", *v)};
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
        } else if ((flag == "--offscreen" || flag == "--onscreen") && !inlineValue) {
            options.offscreen = flag == "--offscreen";
        } else if (flag == "--gpu-debug" && !inlineValue) {
            options.gpuDebug = true;
        } else if (flag == "--gpu-pass-timing" && !inlineValue) {
            options.gpuPassTiming = true;
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
