#pragma once

// The choice of effects from the settings and what the device supports (gpu::Caps and format queries): a pure
// function, so the fallbacks are tested without a device.

#include <quench/render/Renderer.h>

namespace quench::render::detail {

struct DeviceSupport {
    bool shadowArray = false;        // a depth Texture2DArray that can be sampled and rendered to
    bool depthClamp = false;         // gpu::Caps::depthClamp
    bool sampledDepth = false;       // the scene depth format can be sampled (motion vectors, TAA)
    bool velocityTarget = false;     // Rg16Float can be rendered to and sampled
    bool transmittanceArray = false; // an Rgba16Float Texture2DArray that can be rendered to and sampled
    bool rayTracing = false;         // gpu::Caps::rayTracing and the ray query shaders in the build
    bool rayLighting = false;        // also the reflection, indirect light and path tracer shaders
    bool screenSpace = false;        // sampledDepth, 16-bit float targets and the screen-space effects' pipelines
    bool spatialUpscaling = false;   // gpu::Caps::spatialUpscaling
    bool temporalUpscaling = false;  // gpu::Caps::temporalUpscaling
    bool denoisedUpscaling = false;  // gpu::Caps::denoisedUpscaling
    bool cloudLayer = false;         // the cloud pipelines (the 2D layer)
    bool cloudVolume = false;        // also a renderable Rgba8Unorm 3D texture and the volumetric pipelines
    bool skyCube = false;            // an Rgba16Float cube that can be rendered to and sampled
};

ActiveEffects selectEffects(const RenderSettings& settings, const DeviceSupport& support);

} // namespace quench::render::detail
