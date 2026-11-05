#pragma once

// The choice of effects from the settings and what the device supports (gpu::Caps and format queries): a pure
// function, so the fallbacks are tested without a device.

#include <quench/render/Renderer.h>

namespace quench::render::detail {

struct DeviceSupport {
    bool shadowArray = false;    // a depth Texture2DArray that can be sampled and rendered to
    bool depthClamp = false;     // gpu::Caps::depthClamp
    bool sampledDepth = false;   // the scene depth format can be sampled (motion vectors, TAA)
    bool velocityTarget = false; // Rg16Float can be rendered to and sampled
};

ActiveEffects selectEffects(const RenderSettings& settings, const DeviceSupport& support);

} // namespace quench::render::detail
