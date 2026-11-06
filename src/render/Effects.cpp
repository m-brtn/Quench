#include "Effects.h"

#include <quench/render/Cascades.h>

#include <algorithm>

namespace quench::render::detail {

ActiveEffects selectEffects(const RenderSettings& settings, const DeviceSupport& support) {
    ActiveEffects e;
    const bool sunShadows = settings.shadowCascades > 0;
    e.sunShadows = settings.sunShadows == SunShadows::RayTraced && support.rayTracing ? SunShadows::RayTraced
                                                                                      : SunShadows::ShadowMaps;
    const bool maps = e.sunShadows == SunShadows::ShadowMaps;
    e.shadowCascades = maps && support.shadowArray ? std::clamp(settings.shadowCascades, 0, kMaxCascades) : 0;
    e.shadowDepthClamp = e.shadowCascades > 0 && support.depthClamp;
    e.translucentShadows = settings.translucentShadows == TranslucentShadows::Map && support.transmittanceArray
                               ? TranslucentShadows::Map
                               : TranslucentShadows::Dithered;
    if (!sunShadows) e.sunShadows = SunShadows::ShadowMaps;
    e.ambientOcclusion = settings.ambientOcclusion == AmbientOcclusion::RayTraced && support.rayTracing
                             ? AmbientOcclusion::RayTraced
                             : AmbientOcclusion::None;
    e.antiAliasing = settings.antiAliasing;
    const bool temporalInputs = support.sampledDepth && support.velocityTarget;
    if (e.antiAliasing == AntiAliasing::Taa && !temporalInputs) e.antiAliasing = AntiAliasing::Fxaa;
    switch (settings.upscaling) {
    case Upscaling::Bilinear: break;
    case Upscaling::Spatial:
        if (support.spatialUpscaling && settings.renderScale < 1.0f) e.upscaling = Upscaling::Spatial;
        break;
    case Upscaling::Temporal:
        if (support.temporalUpscaling && temporalInputs) {
            e.upscaling = Upscaling::Temporal;
            e.antiAliasing = AntiAliasing::Taa; // its jitter and motion; the upscaler resolves
        }
        break;
    }
    e.bloom = settings.bloom && settings.bloomStrength > 0.0f;
    return e;
}

} // namespace quench::render::detail
