#include "GpuTest.h"

#include "core/Platform.h"
#include "render_shaders.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>

using namespace quench;
using namespace quench::gpu;

// On iOS a window needs the app lifecycle: the device test app has it (SDL_main), executables spawned by simctl do not.
#if QUENCH_PLATFORM_MACOS || QUENCH_PLATFORM_WINDOWS || QUENCH_PLATFORM_LINUX || defined(QUENCH_TEST_SDL_MAIN)
#define QUENCH_TEST_WINDOWS 1
#else
#define QUENCH_TEST_WINDOWS 0
#endif

#if QUENCH_TEST_WINDOWS
namespace {

// The cases below show a real window (a swapchain needs one on the screen: SDL's Vulkan driver gives a hidden window
// no swapchain texture). They run only on request, with QUENCH_WINDOW_TESTS=1 in the environment; the device test
// app on a phone runs them always.
bool windowTestsWanted() {
#if defined(QUENCH_TEST_SDL_MAIN)
    return true;
#else
    const char* wanted = std::getenv("QUENCH_WINDOW_TESTS");
    return wanted && std::strcmp(wanted, "1") == 0;
#endif
}

void drawFrames(Device& device, SDL_Window* window) {
    const TextureFormat format = device.swapchainFormat(window);
    REQUIRE(format != TextureFormat::Invalid, "no swapchain format");
    std::printf("  swapchain %s\n", formatInfo(format).name);
    CHECK(device.setPresentMode(window, PresentMode::Vsync) == PresentMode::Vsync, "Vsync refused");

    const ColorTargetDesc color{.format = format};
    Result<GraphicsPipeline> pipeline = device.createPipeline({.vertexShader = shaders::render.find("fullscreen_vs"),
                                                               .fragmentShader = shaders::render.find("uniform_fs"),
                                                               .colorTargets = {&color, 1}});
    REQUIRE(pipeline, "%s", pipeline.error().c_str());

    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    int presented = 0;
    for (int frame = 0; frame < 3; ++frame) {
        SDL_PumpEvents();
        CommandBuffer commands = device.acquireCommandBuffer();
        REQUIRE(commands, "no command buffer");
        const TextureRef swapchain = commands.acquireSwapchainTexture(window);
        if (swapchain) {
            CHECK(int(swapchain.desc.width) == width && int(swapchain.desc.height) == height,
                  "swapchain %u x %u, window %d x %d", swapchain.desc.width, swapchain.desc.height, width, height);
            RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = swapchain});
            pass.bindPipeline(*pipeline);
            pass.pushUniforms(ShaderStage::Fragment, 0, Color{0.1f, 0.3f * float(frame), 0.6f, 1.0f});
            pass.draw(3);
            ++presented;
        }
        const Result<void> submitted = commands.submit();
        CHECK(submitted, "submit: %s", submitted.error().c_str());
    }
    CHECK(presented == 3, "%d of 3 frames had a swapchain texture", presented);
}

} // namespace
#endif

// A real window: claim it, draw into its swapchain texture for a few frames, present, release. Opens a small window
// for a moment.
QUENCH_TEST(window_present) {
#if !QUENCH_TEST_WINDOWS
    QUENCH_SKIP("windows need the app lifecycle on this platform");
#else
    if (!windowTestsWanted()) QUENCH_SKIP("shows a window: only with QUENCH_WINDOW_TESTS=1");
    QUENCH_GPU_DEVICE(device);
    SDL_Window* window = SDL_CreateWindow("quench_gpu_device_tests", 160, 90, 0);
    REQUIRE(window, "SDL_CreateWindow: %s", SDL_GetError());
    const Result<void> claimed = device.claimWindow(window);
    CHECK(claimed, "claimWindow: %s", claimed.error().c_str());
    if (claimed) {
        drawFrames(device, window);
        device.releaseWindow(window);
    }
    SDL_DestroyWindow(window);
#endif
}

// Swapchain compositions: what the backend reports for a window, switching to each supported one (the swapchain
// format follows, frames are presented) and the SDR fallback for an unsupported one.
QUENCH_TEST(window_swapchain_compositions) {
#if !QUENCH_TEST_WINDOWS
    QUENCH_SKIP("windows need the app lifecycle on this platform");
#else
    if (!windowTestsWanted()) QUENCH_SKIP("shows a window: only with QUENCH_WINDOW_TESTS=1");
    QUENCH_GPU_DEVICE(device);
    SDL_Window* window = SDL_CreateWindow("quench_gpu_device_tests", 160, 90, 0);
    REQUIRE(window, "SDL_CreateWindow: %s", SDL_GetError());
    const Result<void> claimed = device.claimWindow(window);
    REQUIRE(claimed, "claimWindow: %s", claimed.error().c_str());
    CHECK(device.composition(window) == SwapchainComposition::Sdr, "a claimed window starts in SDR");
    CHECK(device.supportsComposition(window, SwapchainComposition::Sdr), "SDR is always supported");
    const TextureFormat expected[] = {TextureFormat::Bgra8Unorm, TextureFormat::Bgra8UnormSrgb,
                                      TextureFormat::Rgba16Float, TextureFormat::Rgb10a2Unorm};
    for (const SwapchainComposition c : {SwapchainComposition::SdrLinear, SwapchainComposition::HdrExtendedLinear,
                                         SwapchainComposition::Hdr10, SwapchainComposition::Sdr}) {
        const bool supported = device.supportsComposition(window, c);
        std::printf("  %s: %s\n", swapchainCompositionName(c), supported ? "supported" : "not supported");
        const SwapchainComposition applied = device.setComposition(window, c);
        CHECK(applied == (supported ? c : SwapchainComposition::Sdr) && device.composition(window) == applied,
              "%s: applied %s", swapchainCompositionName(c), swapchainCompositionName(applied));
        const TextureFormat format = device.swapchainFormat(window);
        CHECK(format == expected[int(applied)], "%s: swapchain %s", swapchainCompositionName(applied),
              formatInfo(format).name);
        if (supported) drawFrames(device, window);
    }
    device.releaseWindow(window);
    SDL_DestroyWindow(window);
#endif
}
