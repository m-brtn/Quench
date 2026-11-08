#ifndef QUENCH_WIND_HLSLI
#define QUENCH_WIND_HLSLI

// The weather's wind in the render module's shaders (docs/weather.md, "Wind"): the field of quench_weather with its
// gusts and eddies (quench/WeatherField.h), the same a game asks with weather::Weather::windAt, from the frame
// uniforms. quenchWindSway bends a vertex of grass, a bush or a tree's crown with it, for a surface shader's
// QUENCH_SURFACE_VERTEX function (quench/Surface.hlsli; build surface_shadow_vs too, so that the shadows sway):
//
//   #include "quench/Surface.hlsli"
//   #include "quench/Wind.hlsli"
//   void grassVertex(inout QuenchVertexIn v, InstanceData instance) {
//       quenchWindSway(v.position, v.normal, instance, 0.6, 1.0, 0.01); // 0.6 m tall, stiffness 1, a little flutter
//   }
//   #define QUENCH_SURFACE_VERTEX grassVertex
//   #include "quench/SurfaceEntries.hlsli"
//
// The plant's root is its object's origin and its stem the world's up. A vertex turns about the root, more toward
// the top (by an angle that grows with its height above the root), downwind as far as the wind at the plant's top
// pushes it: about 0.08 radians per m/s at stiffness 1 (grass), less for stiffer plants, at most 1.3 radians. Gusts
// bend it further and cross a field of it as waves; eddies shake it; its leaves shiver by `flutter` metres in a
// 10 m/s wind. Without a weather (no Scene::setWeather) or in a calm nothing moves. Heights are above y = 0, as the
// weather's wind profile is. The motion vectors and the traced effects see the plant unbent (docs/weather.md).
// quenchWindSwayAbout takes the root per vertex instead: a patch of grass in one instance, each blade turning about
// the ground below its vertex (root float3(p.x, 0, p.z)), so that a meadow is a few dozen instances, not thousands.

#include "quench/Frame.hlsli"
#include "quench/SceneUniforms.h"
#include "quench/WeatherField.h"

WindUniforms quenchFrameWind() {
    WindUniforms w;
    w.air = quenchFrame.windAir;
    w.speed = quenchFrame.windSpeed;
    w.time = quenchFrame.windTime;
    return w;
}

// The wind at a world point: mean, gust (mean + gust is the air's velocity, m/s) and the gusts' offset (m).
QuenchWind quenchWindAt(float3 p) { return quenchWeatherWind(quenchFrameWind(), p); }

// Bends p (object space) and its normal n with the wind, about `rootObject` (object space). height: the plant's height
// (m, world); stiffness: 1 grass, about 3 a bush, 8 or more a tree's crown; flutter: m.
void quenchWindSwayAbout(inout float3 p, inout float3 n, InstanceData instance, float3 rootObject, float height,
                         float stiffness, float flutter) {
    const float3 root = mul(instance.model, float4(rootObject, 1.0)).xyz;
    const float3 world = mul(instance.model, float4(p, 1.0)).xyz;
    const float above = world.y - root.y;
    if (above <= 0.0 || height <= 0.0 || quenchFrame.windSpeed.x <= 0.0) return;
    const QuenchWind w = quenchWindAt(float3(root.x, root.y + height, root.z));
    const float3 v = w.mean + w.gust;
    const float speed = length(v.xz);
    if (speed < 1e-3) return;
    const float r = saturate(above / height);
    const float2 downwind = v.xz / speed;
    const float bend = min(speed * 0.08 / max(stiffness, 0.01), 1.3) * r;
    // Turned in the upright plane along the wind, about the root.
    const float s = sin(bend), c = cos(bend);
    const float3 rel = world - root;
    const float along = dot(rel.xz, downwind);
    float3 moved = rel;
    moved.xz += downwind * (rel.y * s + along * (c - 1.0));
    moved.y = rel.y * c - along * s;
    // The leaves' shiver: fast, small, out of step from plant to plant (whole cycles in the wind's period, so that it
    // goes on where the period starts again); its phase changes smoothly with the root, so that the vertices of a
    // blade in a patch, whose roots differ a little, shiver together.
    const float t = quenchFrame.windTime.x;
    const float phase = dot(root.xz, float2(5.3, 3.7));
    const float shiver = flutter * saturate(speed / 10.0) * r;
    moved += float3(sin(6.2831853 * frac(t * 2.5) + phase), 0.4 * sin(6.2831853 * frac(t * 3.25) + phase * 1.7),
                    cos(6.2831853 * frac(t * 2.75) + phase * 0.6)) *
             shiver;
    // Back into object space: the inverse of the model's 3 x 3 is the transpose of the normal matrix's.
    p += mul(moved - rel, (float3x3)instance.normalMatrix);
    // The normal turns with the stem.
    float3 nw = mul((float3x3)instance.normalMatrix, n);
    const float nOut = dot(nw.xz, downwind);
    nw.xz += downwind * (nw.y * s + nOut * (c - 1.0));
    nw.y = nw.y * c - nOut * s;
    n = mul(transpose((float3x3)instance.model), nw);
}

// About the object's origin: one plant per instance.
void quenchWindSway(inout float3 p, inout float3 n, InstanceData instance, float height, float stiffness,
                    float flutter) {
    quenchWindSwayAbout(p, n, instance, float3(0.0, 0.0, 0.0), height, stiffness, flutter);
}

#endif
