// A game-style surface shader for the render tests: no lighting response of its own, only the emissive color from
// MaterialDesc::params[0], and a vertex hook that lifts the mesh by InstanceDesc::custom.y.

#include "quench/Surface.hlsli"

void lift(inout QuenchVertexIn v, InstanceData instance) { v.position.y += instance.custom.y; }

#if defined(QUENCH_STAGE_FRAGMENT)
QuenchSurface flat(QuenchSurfaceInput i) {
    QuenchSurface s = quenchStandardSurface(i);
    s.albedo = float3(0.0, 0.0, 0.0);
    s.roughness = 1.0;
    s.metallic = 0.0;
    s.emissive = quenchMaterial.params[0].rgb;
    return s;
}
#endif

#define QUENCH_SURFACE flat
#define QUENCH_SURFACE_VERTEX lift
#include "quench/SurfaceEntries.hlsli"
