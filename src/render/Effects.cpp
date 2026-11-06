#include "Effects.h"

#include <quench/render/Cascades.h>

#include <algorithm>

namespace quench::render::detail {

ActiveEffects selectEffects(const RenderSettings& settings, const DeviceSupport& support) {
    ActiveEffects e;
    e.shadowCascades = support.shadowArray ? std::clamp(settings.shadowCascades, 0, kMaxCascades) : 0;
    e.shadowDepthClamp = e.shadowCascades > 0 && support.depthClamp;
    e.translucentShadows = settings.translucentShadows == TranslucentShadows::Map && support.transmittanceArray
                               ? TranslucentShadows::Map
                               : TranslucentShadows::Dithered;
    e.antiAliasing = settings.antiAliasing;
    if (e.antiAliasing == AntiAliasing::Taa && !(support.sampledDepth && support.velocityTarget))
        e.antiAliasing = AntiAliasing::Fxaa;
    e.bloom = settings.bloom && settings.bloomStrength > 0.0f;
    return e;
}

} // namespace quench::render::detail
