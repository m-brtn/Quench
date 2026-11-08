// A plant for the render tests: the emissive color of MaterialDesc::params[0], bent by the weather's wind with
// quench/Wind.hlsli (height, stiffness and flutter from InstanceDesc::custom.xyz; custom.w 1: rooted below each vertex,
// as a patch of grass is).

#include "quench/Surface.hlsli"
#include "quench/Wind.hlsli"

void sway(inout QuenchVertexIn v, InstanceData instance) {
    if (instance.custom.w > 0.5)
        quenchWindSwayAbout(v.position, v.normal, instance, float3(v.position.x, 0.0, v.position.z), instance.custom.x,
                            instance.custom.y, instance.custom.z);
    else
        quenchWindSway(v.position, v.normal, instance, instance.custom.x, instance.custom.y, instance.custom.z);
}

#if defined(QUENCH_STAGE_FRAGMENT)
QuenchSurface glow(QuenchSurfaceInput i) {
    QuenchSurface s = quenchStandardSurface(i);
    s.albedo = float3(0.0, 0.0, 0.0);
    s.roughness = 1.0;
    s.metallic = 0.0;
    s.emissive = quenchMaterial.params[0].rgb;
    return s;
}
#endif

#define QUENCH_SURFACE glow
#define QUENCH_SURFACE_VERTEX sway
#include "quench/SurfaceEntries.hlsli"
