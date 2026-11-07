// The rain (src/render/Rain.cpp, docs/weather.md, "Rain"): the rain occlusion map copied into the weather maps, the
// streaks and splashes of the drops near the camera (drawn after TAA and upscaling into the output-size image, tested
// against the scene's depth here), and the rain shafts with the rainbow (marched at half or a quarter of the render
// size after the HDR pass, then blended into the render-size HDR image).
//
// The streaks are procedural: a fixed set of drops in a box around the camera, each falling with the rain's velocity
// from a place of its own and wrapped around the box, so nothing is simulated or stored. A drop is drawn where the
// weather's precipitation there is above its own random threshold and the rain occlusion map lets the rain reach it.
// Its streak is the distance it falls during an exposure, at least one pixel wide (its opacity scaled by its width
// below that), lit by the sky and the ground it refracts and the sun it sends forward. The splashes land on the
// highest surface the occlusion map has at a random place near the camera, for a short lifetime each.
//
// Bindings: uniform slot 0 FrameUniforms (quench/Frame.hlsli), slot 1 RainUniforms (RainUniforms.h), vertex and
// fragment; textures per entry below.

#include "quench/Bindings.hlsli"
#include "quench/Clouds.hlsli"
#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/Rain.hlsli"
#include "quench/SceneUniforms.h"

#include "RainUniforms.h"

cbuffer RainBlock : QUENCH_UNIFORM(1) {
    RainUniforms rain;
};

// ---------------------------------------------------------------- the occlusion map

#if defined(QUENCH_ENTRY_rain_map_fs)
Texture2D<float> occlusionTex : QUENCH_TEXTURE(0);
SamplerState occlusionSampler : QUENCH_SAMPLER(0);

// A texel of the rain occlusion map's region of the weather maps: the depth along the rain of the highest surface.
float4 rain_map_fs(FullscreenOut vin) : SV_Target0 {
    return float4(occlusionTex.SampleLevel(occlusionSampler, vin.uv, 0.0), 0.0, 0.0, 1.0);
}
#endif

// ---------------------------------------------------------------- the drops

// 0..1 from an integer (PCG).
float rainRandom(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}

struct RainDropOut {
    float4 position : SV_Position;
    float3 coord : TEXCOORD0; // streaks: x across (-1..1), y along (0 head, 1 tail); splashes: xy in the quad, z age
    float4 color : TEXCOORD1; // the light it sends toward the camera, its opacity
};

// The two triangles of a quad: x -1..1, y 0..1.
static const float2 kRainCorners[6] = {float2(-1.0, 0.0), float2(1.0, 0.0), float2(-1.0, 1.0),
                                       float2(-1.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0)};

// The light of a drop at P: the sky and the ground it refracts toward the camera, and the sun it scatters forward
// where the clouds let the sun through.
float3 rainDropLight(Texture2D<float4> maps, SamplerState s, float3 P) {
    float3 light = rain.light.rgb;
    [branch] if (rain.sun.w > 0.0) {
        const float3 toCamera = normalize(quenchFrame.cameraPos.xyz - P);
        const float forward = phaseHG(dot(-toCamera, quenchFrame.sunDir.xyz), 0.7) * rain.sun.w;
        light += rain.sun.rgb * quenchCloudShadow(maps, s, P) * forward;
    }
    return light;
}

RainDropOut rainCulled() {
    RainDropOut o;
    o.position = float4(-2.0, -2.0, -2.0, 1.0); // outside the clip volume: all six corners, no area
    o.coord = float3(0.0, 0.0, 0.0);
    o.color = float4(0.0, 0.0, 0.0, 0.0);
    return o;
}

#if defined(QUENCH_ENTRY_rain_streak_vs) || defined(QUENCH_ENTRY_rain_splash_vs)
Texture2D<float4> mapsTex : QUENCH_TEXTURE(0);
SamplerState mapsSampler : QUENCH_SAMPLER(0);
#endif

#if defined(QUENCH_ENTRY_rain_streak_vs)
RainDropOut rain_streak_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const uint id = iid * 8u;
    const float side = rain.box.w * 2.0;
    const float3 velocity = rain.fall.xyz * lerp(0.85, 1.15, rainRandom(id + 3u));
    // A fixed place in a lattice of the box's size, moved by the fall and wrapped around the box's centre.
    const float3 start = float3(rainRandom(id), rainRandom(id + 1u), rainRandom(id + 2u)) * side;
    const float3 moved = start + velocity * rain.fall.w - rain.box.xyz;
    const float3 head = rain.box.xyz + (frac(moved / side) - 0.5) * side;
    const float dist = length(head - quenchFrame.cameraPos.xyz);
    const float fade = saturate(1.0 - dist * dist / (rain.box.w * rain.box.w)) * saturate((dist - 0.3) * 2.0);
    if (fade <= 0.0) return rainCulled();
    const float4 weather = quenchWetnessAt(mapsTex, mapsSampler, head.xz);
    if (rainRandom(id + 4u) >= weather.b) return rainCulled();
    if (quenchRainExposure(mapsTex, mapsSampler, head) < 0.5) return rainCulled();

    const float3 tail = head - velocity * rain.streaks.y;
    const float4 c0 = mul(rain.viewProjStable, float4(head, 1.0));
    const float4 c1 = mul(rain.viewProjStable, float4(tail, 1.0));
    if (c0.w < 0.1 || c1.w < 0.1) return rainCulled();
    // In pixels: the streak's axis, and its width (at least a pixel, its opacity scaled by its width below that).
    const float2 halfSize = 0.5 * rain.target.xy;
    float2 axis = (c1.xy / c1.w - c0.xy / c0.w) * halfSize;
    const float pixels = sqrt(dot(axis, axis));
    axis = pixels > 1e-3 ? axis / pixels : float2(0.0, -1.0);
    const float2 across = float2(-axis.y, axis.x);
    const float width = rain.streaks.z * rain.light.w / c0.w;
    const float drawn = max(width, 1.0);

    const float2 corner = kRainCorners[vid];
    float4 position = corner.y < 0.5 ? c0 : c1;
    const float2 offset = across * (corner.x * drawn * 0.5) + axis * ((corner.y * 2.0 - 1.0) * 0.5);
    position.xy += offset / halfSize * position.w;

    RainDropOut o;
    o.position = position;
    o.coord = float3(corner, 0.0);
    const float opacity = rain.streaks.w * min(width, 1.0) * fade;
    o.color = float4(rainDropLight(mapsTex, mapsSampler, head) * opacity, opacity);
    return o;
}
#endif

#if defined(QUENCH_ENTRY_rain_splash_vs)
RainDropOut rain_splash_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    // A splash lives for a lifetime at a place of its own, then starts again elsewhere.
    const float phase = rain.fall.w / rain.splashes.z + rainRandom(iid * 4u);
    const float epoch = floor(phase);
    const float age = phase - epoch;
    const uint seed = iid * 9781u + uint(int(epoch)) * 6271u;
    const float2 xz =
        quenchFrame.cameraPos.xz + (float2(rainRandom(seed), rainRandom(seed + 1u)) * 2.0 - 1.0) * rain.splashes.y;
    // Where the rain through that place meets the highest surface.
    const float3 m = quenchRainMapPos(float3(xz.x, quenchFrame.cameraPos.y, xz.y));
    if (any(m.xy < 0.0) || any(m.xy > 1.0)) return rainCulled();
    const float top = quenchRainOccluder(mapsTex, mapsSampler, m.xy);
    if (top > 0.999) return rainCulled();
    const float4 h = mul(rain.invRainViewProj, float4(m.x * 2.0 - 1.0, 1.0 - m.y * 2.0, top, 1.0));
    const float3 ground = h.xyz / h.w;
    const float4 weather = quenchWetnessAt(mapsTex, mapsSampler, ground.xz);
    if (rainRandom(seed + 2u) >= weather.b) return rainCulled();
    const float3 toCamera = quenchFrame.cameraPos.xyz - ground;
    const float dist = length(toCamera);
    const float fade = saturate(1.0 - dist / rain.splashes.y) * saturate(dist - 0.5);
    if (fade <= 0.0) return rainCulled();

    // An upright quad facing the camera, a little toward it so that the ground does not cut it.
    const float3 V = toCamera / dist;
    const float3 right = normalize(cross(float3(0.0, 1.0, 0.0), V) + float3(1e-4, 0.0, 0.0));
    const float size = rain.splashes.w * (0.6 + 0.4 * age);
    const float2 corner = kRainCorners[vid];
    const float3 world = ground + V * 0.05 + right * (corner.x * size) + float3(0.0, corner.y * size * 0.7, 0.0);
    RainDropOut o;
    o.position = mul(rain.viewProjStable, float4(world, 1.0));
    o.coord = float3(corner.x, corner.y, age);
    const float opacity = 0.8 * (1.0 - age) * (1.0 - age) * fade;
    o.color = float4(rainDropLight(mapsTex, mapsSampler, ground) * opacity, opacity);
    return o;
}
#endif

#if defined(QUENCH_ENTRY_rain_streak_fs) || defined(QUENCH_ENTRY_rain_splash_fs)
Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);

// Whether the scene is in front of a fragment (reversed depth: nearer is greater).
bool rainHidden(float4 position) {
    return depthTex.SampleLevel(depthSampler, position.xy * rain.target.zw, 0.0) > position.z;
}
#endif

#if defined(QUENCH_ENTRY_rain_streak_fs)
float4 rain_streak_fs(RainDropOut vin) : SV_Target0 {
    if (rainHidden(vin.position)) discard;
    const float across = saturate(1.0 - abs(vin.coord.x));
    const float along = sqrt(saturate(sin(vin.coord.y * QUENCH_PI)));
    return vin.color * (across * along);
}
#endif

#if defined(QUENCH_ENTRY_rain_splash_fs)
// A crown: a thin rim rising and spreading with age, broken into droplets.
float4 rain_splash_fs(RainDropOut vin) : SV_Target0 {
    if (rainHidden(vin.position)) discard;
    const float age = vin.coord.z;
    const float2 p = float2(vin.coord.x, vin.coord.y * 1.4);
    const float r = length(p);
    const float rim = saturate(1.0 - abs(r - lerp(0.35, 0.95, age)) * 7.0);
    const float droplets = 0.55 + 0.45 * cos(atan2(p.y, p.x) * 11.0);
    const float a = rim * droplets * saturate(vin.coord.y * 6.0);
    return vin.color * a;
}
#endif

// ---------------------------------------------------------------- the shafts and the rainbow

#if defined(QUENCH_ENTRY_rain_shafts_fs) || defined(QUENCH_ENTRY_rain_shafts_up_fs)
Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);

// The target pixel (its centre) a shaft texel is marched for: the one at its block's centre, inside the target.
float2 shaftPixel(float2 texel) {
    const float scale = rain.rainbow.w;
    return min(texel * scale + floor(scale * 0.5), rain.target.xy - 1.0) + 0.5;
}

// The distance (m) from the camera to the scene at a target uv, 30 km where the sky is.
float shaftDistance(float2 uv) {
    const float depth = depthTex.SampleLevel(depthSampler, uv, 0.0);
    if (depth <= 0.0) return 30000.0;
    const float4 w = mul(quenchFrame.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return min(length(w.xyz / w.w - quenchFrame.cameraPos.xyz), 30000.0);
}
#endif

#if defined(QUENCH_ENTRY_rain_shafts_fs)
Texture2D<float4> weatherTex : QUENCH_TEXTURE(1);
SamplerState weatherSampler : QUENCH_SAMPLER(1);
Texture2D<float4> mapsTex : QUENCH_TEXTURE(2);
SamplerState mapsSampler : QUENCH_SAMPLER(2);

// The colors of a rainbow's band at t (0 its violet inner edge, 1 its red outer edge).
float3 rainbowColor(float t) {
    const float3 c = saturate(1.0 - abs(t - float3(0.85, 0.5, 0.15)) * 3.0);
    return c * saturate(t * (1.0 - t) * 8.0);
}

// The light the drops send toward the camera from the sun at an angle theta (radians) from the antisolar point: the
// primary bow between 40.6 and 42.3 degrees, the secondary, fainter and reversed, between 50.4 and 53.4.
float3 rainbow(float theta) {
    const float deg = theta * (180.0 / QUENCH_PI);
    const float3 primary = rainbowColor((deg - 40.4) / 2.2);
    const float3 secondary = rainbowColor(1.0 - (deg - 50.2) / 3.4) * 0.2;
    return (primary + secondary) * rain.rainbow.y;
}

// The veil of the rain falling under the clouds, marched from rain.shafts.w metres to the scene or the cloud base:
// single scattering of the sky's and the ground's light (and the sun's, with the rainbow where it is on) by the
// precipitation of the clouds' weather map. Drawn at a reduced size (rain.rainbow.w target pixels per texel), each
// texel for the target pixel at its block's centre; rain_shafts_up_fs blends it as color + destination *
// transmittance.
float4 rain_shafts_fs(FullscreenOut vin) : SV_Target0 {
    const float2 pixel = shaftPixel(floor(vin.position.xy));
    const float2 uv = pixel * rain.target.zw;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 camera = quenchFrame.cameraPos.xyz;
    const float4 far = mul(quenchFrame.invViewProj, float4(ndc, 1.0, 1.0));
    const float3 dir = normalize(far.xyz / far.w - camera);
    const float sceneDistance = shaftDistance(uv);
    // The part of the ray below the cloud base.
    const float base = rain.shafts.y;
    float t0 = rain.shafts.w, t1 = sceneDistance;
    if (camera.y < base) {
        if (dir.y > 1e-4) t1 = min(t1, (base - camera.y) / dir.y);
    } else {
        if (dir.y >= -1e-4) return float4(0.0, 0.0, 0.0, 1.0);
        t0 = max(t0, (base - camera.y) / dir.y);
    }
    if (t1 <= t0) return float4(0.0, 0.0, 0.0, 1.0);

    const int steps = int(rain.shafts.x);
    const float jitter = frac(ign(vin.position.xy) + rain.rainbow.z);
    const bool sun = rain.sun.w > 0.0;
    float3 bow = float3(0.0, 0.0, 0.0);
    if (sun && rain.rainbow.x > 0.0) bow = rainbow(acos(clamp(dot(dir, -quenchFrame.sunDir.xyz), -1.0, 1.0)));
    const float sunShare = 1.0 / (4.0 * QUENCH_PI);
    // The sky's light under a cloud is dimmer than in the open: rain.light is the camera's, scaled by how much of the
    // sky the clouds above each step hide against how much they hide above the camera.
    const float openHere =
        1.0 - 0.65 * weatherTex.SampleLevel(weatherSampler, (camera.xz - rain.weatherMap.xy) * rain.weatherMap.z, 2.0).r;
    float3 color = float3(0.0, 0.0, 0.0);
    float T = 1.0;
    float previous = t0;
    for (int i = 0; i < steps; ++i) {
        // Steps growing with the distance (with the square of the step's number): the rain near the camera in
        // more detail, still a few steps through a shaft kilometres away.
        const float u = (float(i) + jitter) / float(steps), uEnd = (float(i) + 1.0) / float(steps);
        const float t = t0 + (t1 - t0) * u * u;
        const float tEnd = t0 + (t1 - t0) * uEnd * uEnd;
        const float dt = tEnd - previous;
        previous = tEnd;
        const float3 P = camera + dir * t;
        const float2 uv = (P.xz - rain.weatherMap.xy) * rain.weatherMap.z;
        // The precipitation over a part of the step's length (the map's mips): far steps are long, a point would miss
        // a shaft; the jitter over frames covers the rest.
        const float4 weather =
            weatherTex.SampleLevel(weatherSampler, uv, max(log2(max(dt * rain.weatherMap.w, 1.0)) - 1.0, 0.0));
        const float p = weather.b;
        if (p <= 0.01) continue;
        // Fall streaks: the rain is denser in curtains a few hundred metres apart, the same at every height.
        const float streaks = valueNoise(P.xz / 220.0) * 0.65 + valueNoise(P.xz / 70.0 + 7.3) * 0.35;
        const float sigma = rain.shafts.z * p * lerp(0.25, 1.75, streaks) * saturate((base - P.y) / 150.0);
        float3 light = rain.light.rgb * ((1.0 - 0.65 * weather.r) / openHere);
        if (sun) light += rain.sun.rgb * quenchCloudShadow(mapsTex, mapsSampler, P) * (sunShare + bow);
        const float stepT = exp(-sigma * dt);
        color += T * light * (1.0 - stepT);
        T *= stepT;
    }
    return float4(color, T);
}
#endif

#if defined(QUENCH_ENTRY_rain_shafts_up_fs)
Texture2D<float4> shaftsTex : QUENCH_TEXTURE(1);
SamplerState shaftsSampler : QUENCH_SAMPLER(1);

// The shafts at the target's size: the four nearest shaft texels, bilinear, each weighed down by how far the depth
// it was marched for is from this pixel's (so that a near edge does not take the veil of the distance behind it).
// The reversed depth is near / view depth, so the depths' ratio is the view depths' inverse ratio. Blended as
// color + destination * transmittance.
float4 rain_shafts_up_fs(FullscreenOut vin) : SV_Target0 {
    const float scale = rain.rainbow.w;
    const float2 shaftSize = ceil(rain.target.xy / scale);
    const float here = max(depthTex.SampleLevel(depthSampler, vin.uv, 0.0), 1e-7);
    const float2 q = (vin.position.xy - floor(scale * 0.5) - 0.5) / scale;
    const float2 base = floor(q), f = q - base;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weights = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float2 texel = clamp(base + float2(float(i & 1), float(i >> 1)), 0.0, shaftSize - 1.0);
        const float2 b = float2((i & 1) != 0 ? f.x : 1.0 - f.x, (i >> 1) != 0 ? f.y : 1.0 - f.y);
        const float there = max(depthTex.SampleLevel(depthSampler, shaftPixel(texel) * rain.target.zw, 0.0), 1e-7);
        const float w = b.x * b.y / (1e-3 + abs(here / there - 1.0));
        sum += shaftsTex.SampleLevel(shaftsSampler, (texel + 0.5) / shaftSize, 0.0) * w;
        weights += w;
    }
    return weights > 0.0 ? sum / weights : float4(0.0, 0.0, 0.0, 1.0);
}
#endif
