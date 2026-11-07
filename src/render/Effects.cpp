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
    const bool screenSpace = support.screenSpace && support.sampledDepth;
    const bool screenOcclusion =
        settings.ambientOcclusion == AmbientOcclusion::ScreenSpace ||
        (settings.ambientOcclusion == AmbientOcclusion::RayTraced && settings.screenSpaceFallbacks);
    e.ambientOcclusion = settings.ambientOcclusion == AmbientOcclusion::RayTraced && support.rayTracing
                             ? AmbientOcclusion::RayTraced
                         : screenOcclusion && screenSpace ? AmbientOcclusion::ScreenSpace
                                                          : AmbientOcclusion::None;
    e.localLightShadows = support.rayTracing ? std::clamp(settings.localLightShadows, 0, kMaxRayLocalShadows) : 0;
    e.localShadowMaps = !support.rayTracing && support.shadowArray && settings.localShadowMapSize > 0
                            ? std::clamp(settings.localLightShadows, 0, kMaxRayLocalShadows)
                            : 0;
    const bool lighting = support.rayTracing && support.rayLighting;
    const bool screenReflections = settings.reflections == Reflections::ScreenSpace ||
                                   (settings.reflections == Reflections::RayTraced && settings.screenSpaceFallbacks);
    e.reflections = settings.reflections == Reflections::RayTraced && lighting ? Reflections::RayTraced
                    : screenReflections && screenSpace                         ? Reflections::ScreenSpace
                                                                               : Reflections::Sky;
    e.indirectLight = lighting ? settings.indirectLight : IndirectLight::Sky;
    e.pathTracer = lighting && settings.pathTracer;
    e.antiAliasing = settings.antiAliasing;
    const bool temporalInputs = support.sampledDepth && support.velocityTarget;
    const bool traced = e.sunShadows == SunShadows::RayTraced || e.ambientOcclusion == AmbientOcclusion::RayTraced ||
                        e.localLightShadows > 0 || e.reflections == Reflections::RayTraced ||
                        e.indirectLight != IndirectLight::Sky;
    e.rayDenoiser =
        settings.rayDenoiser == RayDenoiser::Temporal && temporalInputs ? RayDenoiser::Temporal : RayDenoiser::Spatial;
    if (e.antiAliasing == AntiAliasing::Taa && !temporalInputs) e.antiAliasing = AntiAliasing::Fxaa;
    switch (settings.upscaling) {
    case Upscaling::Bilinear: break;
    case Upscaling::Spatial:
        if (support.spatialUpscaling && settings.renderScale < 1.0f) e.upscaling = Upscaling::Spatial;
        break;
    case Upscaling::TemporalDenoised:
        if (support.denoisedUpscaling && traced && temporalInputs) {
            e.upscaling = Upscaling::TemporalDenoised;
            e.antiAliasing = AntiAliasing::Taa;
            break;
        }
        [[fallthrough]]; // the temporal upscaler, with the engine's denoiser
    case Upscaling::Temporal:
        if (support.temporalUpscaling && temporalInputs) {
            e.upscaling = Upscaling::Temporal;
            e.antiAliasing = AntiAliasing::Taa; // its jitter and motion; the upscaler resolves
        }
        break;
    }
    // The path tracer accumulates jittered samples itself: no TAA, and nothing for a temporal upscaler to resolve.
    if (e.pathTracer) {
        e.antiAliasing = AntiAliasing::None;
        if (e.upscaling == Upscaling::Temporal || e.upscaling == Upscaling::TemporalDenoised)
            e.upscaling =
                support.spatialUpscaling && settings.renderScale < 1.0f ? Upscaling::Spatial : Upscaling::Bilinear;
    }
    e.bloom = settings.bloom && settings.bloomStrength > 0.0f;
    if (settings.sky && settings.clouds != Clouds::Off && support.cloudLayer)
        e.clouds = settings.clouds == Clouds::Volumetric && support.cloudVolume ? Clouds::Volumetric : Clouds::Layer;
    e.skyCube = e.clouds != Clouds::Off && settings.skyCubeSize > 0 && support.skyCube;
    // The path tracer's frame has no rain or snow: it is the reference of the real-time lighting.
    if (support.rain && !e.pathTracer) {
        e.rain = settings.rain;
        e.snow = settings.snow;
    }
    e.rainShafts = (e.rain != Rain::Off || e.snow != Snow::Off) && settings.rainShafts && e.clouds != Clouds::Off;
    return e;
}

} // namespace quench::render::detail
