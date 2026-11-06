// A game-style surface shader for the render tests: no lighting response of its own, only the emissive color from
// MaterialDesc::params[0], and a vertex hook that lifts the mesh by InstanceDesc::custom.y. With params[1].x > 0 the
// half with u < 0.5 is transparent (opacity 0), the other half opaque: a cut-out of the surface's own.

#include "quench/Surface.hlsli"

void lift(inout QuenchVertexIn v, InstanceData instance) { v.position.y += instance.custom.y; }

#if defined(QUENCH_STAGE_FRAGMENT)
QuenchSurface flat(QuenchSurfaceInput i) {
    QuenchSurface s = quenchStandardSurface(i);
    s.albedo = float3(0.0, 0.0, 0.0);
    s.roughness = 1.0;
    s.metallic = 0.0;
    s.emissive = quenchMaterial.params[0].rgb;
    if (quenchMaterial.params[1].x > 0.0) s.opacity = i.uv.x < 0.5 ? 0.0 : 1.0;
    return s;
}
#endif

#define QUENCH_SURFACE flat
#define QUENCH_SURFACE_VERTEX lift
#include "quench/SurfaceEntries.hlsli"
