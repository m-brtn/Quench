#include "GpuTest.h"

#include "core/Platform.h"

#define SDL_MAIN_HANDLED // SDL_SetMainReady only; the harness has its own main()
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace quench::test {

std::optional<gpu::Device> createGpuDevice() {
#if QUENCH_PLATFORM_IOS
    // Test executables have a plain main() (spawned through simctl), not SDL_main; apps get this from SDL's main
    // callbacks.
    SDL_SetMainReady();
#endif
    const char* expected = std::getenv("QUENCH_TEST_GPU_DRIVER");
    Result<gpu::Device> device = gpu::Device::create({.debug = true});
    if (!device) {
        CHECK(!expected, "the %s device failed: %s", expected, device.error().c_str());
        if (!expected) std::printf("  no GPU device: %s\n", device.error().c_str());
        return std::nullopt;
    }
    CHECK(!expected || device->caps().driver == expected, "driver %s, expected %s", device->caps().driver.c_str(),
          expected);
    return std::move(device).value();
}

std::vector<Rgba8> readRgba8(gpu::Device& device, const gpu::Texture& texture) {
    CHECK(texture.desc().format == gpu::TextureFormat::Rgba8Unorm, "format %s",
          gpu::formatInfo(texture.desc().format).name);
    Result<std::vector<std::byte>> bytes = device.download({.texture = texture});
    if (!bytes) {
        CHECK(false, "download: %s", bytes.error().c_str());
        return {};
    }
    std::vector<Rgba8> pixels(bytes->size() / 4);
    std::memcpy(pixels.data(), bytes->data(), pixels.size() * 4);
    return pixels;
}

bool near(Rgba8 a, Rgba8 b, int tolerance) {
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance &&
           std::abs(a.a - b.a) <= tolerance;
}

} // namespace quench::test
