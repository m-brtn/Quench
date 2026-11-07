// A custom surface shader of the demo: a procedural checker for grounds, lit by the engine's standard shading
// (quench/Surface.hlsli). MaterialDesc::params[0] and [1]: the two colors (linear rgb, a: roughness); params[2].x: the
// cell size (m).

#include "quench/Surface.hlsli"

#if defined(QUENCH_STAGE_FRAGMENT)
// The integral of a +-1 square wave of period 2 (+1 on [0, 1)): a triangle wave between 0 and 1.
float2 squareWaveIntegral(float2 x) { return 1.0 - abs(2.0 * frac(x * 0.5) - 1.0); }

QuenchSurface checker(QuenchSurfaceInput i) {
    QuenchSurface s = quenchStandardSurface(i);
    const float2 p = i.world.xz / max(quenchMaterial.params[2].x, 1e-3);
#if QUENCH_SURFACE_HIT
    // A ray hit has no pixel footprint: the square waves themselves.
    const float2 f = 1.0 - 2.0 * step(0.5, frac(p * 0.5));
#else
    // The checker is the product of two square waves; each is box filtered over the pixel footprint w through its
    // integral, so cells smaller than a pixel fade to the average color instead of aliasing.
    const float2 w = max(abs(ddx(p)), abs(ddy(p))) + 1e-4;
    const float2 f = (squareWaveIntegral(p + 0.5 * w) - squareWaveIntegral(p - 0.5 * w)) / w;
#endif
    const float t = 0.5 - 0.5 * f.x * f.y;
    const float4 a = quenchMaterial.params[0];
    const float4 b = quenchMaterial.params[1];
    s.albedo = lerp(a.rgb, b.rgb, t) * i.color.rgb;
    s.roughness = lerp(a.a, b.a, t);
    return s;
}
#endif

#define QUENCH_SURFACE checker
#include "quench/SurfaceEntries.hlsli"
