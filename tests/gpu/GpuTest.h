#pragma once

// Shared setup of the GPU device tests. A case runs on the driver SDL picks (SDL_GPU_DRIVER overrides it) with the
// backend's validation, and is skipped when no device can be created, unless QUENCH_TEST_GPU_DRIVER names the driver
// that must work: then it fails.

#include "Test.h"

#include <quench/gpu/Device.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace quench::test {

std::optional<gpu::Device> createGpuDevice();

struct Rgba8 {
    std::uint8_t r, g, b, a;
};

// Level 0 of an Rgba8Unorm texture, row by row from the top.
std::vector<Rgba8> readRgba8(gpu::Device& device, const gpu::Texture& texture);

// Every channel within tolerance.
bool near(Rgba8 a, Rgba8 b, int tolerance = 1);

} // namespace quench::test

// QUENCH_GPU_DEVICE(device): declares `gpu::Device& device` or skips (fails) the case.
#define QUENCH_GPU_DEVICE(name)                                                                                        \
    std::optional<::quench::gpu::Device> name##_holder = ::quench::test::createGpuDevice();                            \
    if (!name##_holder) QUENCH_SKIP("no GPU device");                                                                  \
    ::quench::gpu::Device& name = *name##_holder
