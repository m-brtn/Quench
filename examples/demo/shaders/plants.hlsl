// The demo's plants (sections/WeatherSection.cpp): grass, bushes and trees that sway in the weather's wind
// (quench/Wind.hlsli), lit by the engine's standard shading. InstanceDesc::custom: x the plant's height (m), y its
// stiffness, z its leaves' flutter (m), w 1 for a patch of grass (each blade turns about the ground below it), 0 for
// one plant (about its origin).

#include "quench/Surface.hlsli"
#include "quench/Wind.hlsli"

void plantSway(inout QuenchVertexIn v, InstanceData instance) {
    const float3 root = instance.custom.w > 0.5 ? float3(v.position.x, 0.0, v.position.z) : float3(0.0, 0.0, 0.0);
    quenchWindSwayAbout(v.position, v.normal, instance, root, instance.custom.x, instance.custom.y, instance.custom.z);
}

#define QUENCH_SURFACE_VERTEX plantSway
#include "quench/SurfaceEntries.hlsli"
