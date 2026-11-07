// The rain and snow (src/render/Rain.cpp, docs/weather.md, "Rain" and "Snow"): the rain occlusion map copied into
// the weather maps, the streaks and splashes of the drops and the snow's flakes near the camera (drawn after TAA and
// upscaling into the output-size image, tested against the scene's depth here), and the shafts of rain and snow with
// the rainbow (marched at half or a quarter of the render size after the HDR pass, then blended into the render-size
// HDR image).
//
// The streaks are procedural: a fixed set of drops in a box around the camera, each falling with the rain's velocity
// from a place of its own and wrapped around the box, so nothing is simulated or stored. A drop is drawn where the
// weather's rain there (its precipitation times the share that falls as rain) is above its own random threshold and
// the rain occlusion map lets the rain reach it. Its streak is the distance it falls during an exposure, at least one
// pixel wide (its opacity scaled by its width below that), lit by the sky and the ground it refracts and the sun it
// sends forward. The splashes land on the highest surface the occlusion map has at a random place near the camera,
// for a short lifetime each. The flakes are the same with the snow's velocity, a flutter of their own and the
// snowfall's share: small soft discs, drawn out a little along their motion, lit as white diffuse scatterers.
//
// Bindings: uniform slot 0 FrameUniforms (quench/Frame.hlsli), slot 1 RainUniforms (RainUniforms.h), vertex and
// fragment; rain_shafts_fs also slot 2 WeatherUniforms (quench/WeatherField.h); textures per entry below.

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

#if defined(QUENCH_ENTRY_rain_streak_vs) || defined(QUENCH_ENTRY_rain_splash_vs) || defined(QUENCH_ENTRY_rain_flake_vs)
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
    if (rainRandom(id + 4u) >= weather.b * weather.a) return rainCulled();
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
// The share of the sun's irradiance a splash's droplets send toward the camera (a white diffuse surface facing the sun
// sends 1 / pi of it).
static const float kSplashSun = 0.1;

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
    if (rainRandom(seed + 2u) >= weather.b * weather.a) return rainCulled();
    const float3 toCamera = quenchFrame.cameraPos.xyz - ground;
    const float dist = length(toCamera);
    const float fade = saturate(1.0 - dist / rain.splashes.y) * saturate(dist - 0.5);
    if (fade <= 0.0) return rainCulled();

    // An upright quad facing the camera, a little toward it so that the ground does not cut it.
    const float3 V = toCamera / dist;
    const float3 right = normalize(cross(float3(0.0, 1.0, 0.0), V) + float3(1e-4, 0.0, 0.0));
    const float size = rain.splashes.w * lerp(0.7, 1.3, rainRandom(seed + 3u)) * (0.6 + 0.4 * age);
    const float2 corner = kRainCorners[vid];
    const float3 world = ground + V * 0.05 + right * (corner.x * size) + float3(0.0, corner.y * size, 0.0);
    RainDropOut o;
    o.position = mul(rain.viewProjStable, float4(world, 1.0));
    o.coord = float3(corner.x, corner.y, age);
    // Water against the wet ground: brighter than the light it refracts alone (the sky's sheen on the crown), and
    // glinting in the sun from every side, which the puddles' mirror of the sky around it does not.
    const float opacity = 0.9 * (1.0 - age * age) * fade;
    float3 light = rainDropLight(mapsTex, mapsSampler, ground) * 2.0;
    [branch] if (rain.sun.w > 0.0) {
        light += rain.sun.rgb * quenchCloudShadow(mapsTex, mapsSampler, ground + float3(0.0, size, 0.0)) * kSplashSun;
    }
    o.color = float4(light * opacity, opacity);
    return o;
}
#endif

#if defined(QUENCH_ENTRY_rain_flake_vs)
// Flakes are drawn out along their motion over this long (s), and at least this many pixels across.
static const float kFlakeExposure = 0.012;
static const float kFlakeMinPixels = 1.5;

RainDropOut rain_flake_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const uint id = iid * 16u + 2654435769u;
    const float extent = rain.flakes.y;
    const float side = extent * 2.0;
    const float3 centre = quenchFrame.cameraPos.xyz + float3(0.0, extent * 0.3, 0.0);
    // Each flake falls at a speed of its own with the wind, and flutters on a small loop as it falls.
    const float t = rain.fall.w;
    const float3 velocity =
        float3(rain.snowFall.x, rain.snowFall.y * lerp(0.7, 1.3, rainRandom(id + 3u)), rain.snowFall.z);
    const float rate = lerp(1.2, 2.6, rainRandom(id + 5u));
    const float phase = rainRandom(id + 6u) * 6.2831853;
    const float reach = lerp(0.08, 0.3, rainRandom(id + 7u));
    const float3 flutter = float3(sin(t * rate + phase), 0.0, cos(t * rate * 0.8 + phase * 1.3)) * reach;
    const float3 start = float3(rainRandom(id), rainRandom(id + 1u), rainRandom(id + 2u)) * side;
    const float3 moved = start + velocity * t + flutter - centre;
    const float3 P = centre + (frac(moved / side) - 0.5) * side;
    const float dist = length(P - quenchFrame.cameraPos.xyz);
    const float fade = saturate(1.0 - dist * dist / (extent * extent)) * saturate((dist - 0.2) * 3.0);
    if (fade <= 0.0) return rainCulled();
    const float4 weather = quenchWetnessAt(mapsTex, mapsSampler, P.xz);
    if (rainRandom(id + 4u) >= weather.b * (1.0 - weather.a)) return rainCulled();
    if (quenchRainExposure(mapsTex, mapsSampler, P) < 0.5) return rainCulled();

    // Its motion now (the fall, the wind and the flutter's) over the exposure, in pixels on the screen.
    const float3 now = velocity + float3(cos(t * rate + phase), 0.0, -0.8 * sin(t * rate * 0.8 + phase * 1.3)) *
                                      (reach * rate);
    const float4 c0 = mul(rain.viewProjStable, float4(P, 1.0));
    const float4 c1 = mul(rain.viewProjStable, float4(P - now * kFlakeExposure, 1.0));
    if (c0.w < 0.1 || c1.w < 0.1) return rainCulled();
    const float2 halfSize = 0.5 * rain.target.xy;
    float2 axis = (c1.xy / c1.w - c0.xy / c0.w) * halfSize;
    const float streak = sqrt(dot(axis, axis));
    axis = streak > 1e-3 ? axis / streak : float2(0.0, -1.0);
    const float2 across = float2(-axis.y, axis.x);
    const float pixels = rain.flakes.z * lerp(0.6, 1.4, rainRandom(id + 8u)) * rain.light.w / c0.w;
    const float radius = max(pixels, kFlakeMinPixels) * 0.5;

    // A capsule from the flake back along its motion: x across (-1..1 of its radius), y along (in radii, 0 at the
    // flake, z at the end of its motion).
    const float2 corner = kRainCorners[vid];
    const float along = lerp(-radius, streak + radius, corner.y);
    float4 position = c0;
    position.xy += (across * (corner.x * radius) + axis * along) / halfSize * position.w;
    RainDropOut o;
    o.position = position;
    o.coord = float3(corner.x, along / radius, streak / radius);
    // Opaque, thinned below its least size by its area and spread along its motion.
    const float small = min(pixels / kFlakeMinPixels, 1.0);
    const float spread = sqrt(2.0 * radius / (2.0 * radius + streak));
    const float opacity = 0.9 * small * small * spread * fade;
    float3 light = rain.flakeLight.rgb;
    [branch] if (rain.sun.w > 0.0) light += rain.sun.rgb * quenchCloudShadow(mapsTex, mapsSampler, P) * rain.flakes.w;
    o.color = float4(light * opacity, opacity);
    return o;
}
#endif

#if defined(QUENCH_ENTRY_rain_streak_fs) || defined(QUENCH_ENTRY_rain_splash_fs) || defined(QUENCH_ENTRY_rain_flake_fs)
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
// A crown: a rim rising and spreading with age, broken into droplets, and droplets thrown up and out of it that fall
// back as it fades. The quad is as high as it is half wide: coord is (x -1..1, y 0..1) in units of that.
float4 rain_splash_fs(RainDropOut vin) : SV_Target0 {
    if (rainHidden(vin.position)) discard;
    const float age = vin.coord.z;
    const float2 p = float2(vin.coord.x, vin.coord.y * 1.6);
    const float r = length(p);
    const float rim = saturate(1.0 - abs(r - lerp(0.3, 0.85, age)) * 5.0);
    const float lobes = 0.6 + 0.4 * cos(atan2(p.y, p.x) * 11.0);
    float a = rim * lobes * saturate(vin.coord.y * 8.0);
    [unroll] for (int k = 0; k < 4; ++k) {
        const float outward = (float(k) - 1.5) * 0.4;
        const float2 c = float2(outward * (0.6 + age), 0.06 + 2.4 * age * (1.0 - age) * (1.0 - abs(outward) * 0.6));
        a += saturate(1.0 - length(vin.coord.xy - c) * 14.0) * 1.5;
    }
    return vin.color * saturate(a);
}
#endif

#if defined(QUENCH_ENTRY_rain_flake_fs)
// A soft disc drawn out into a capsule along the flake's motion.
float4 rain_flake_fs(RainDropOut vin) : SV_Target0 {
    if (rainHidden(vin.position)) discard;
    const float y = vin.coord.y;
    const float dy = y < 0.0 ? y : max(y - vin.coord.z, 0.0);
    const float d = saturate(1.0 - sqrt(vin.coord.x * vin.coord.x + dy * dy));
    return vin.color * (d * d * (3.0 - 2.0 * d));
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
#include "quench/WeatherField.h"

Texture2D<float4> weatherTex : QUENCH_TEXTURE(1);
SamplerState weatherSampler : QUENCH_SAMPLER(1);
Texture2D<float4> mapsTex : QUENCH_TEXTURE(2);
SamplerState mapsSampler : QUENCH_SAMPLER(2);
cbuffer WeatherBlock : QUENCH_UNIFORM(2) {
    WeatherUniforms field;
};

// quenchWeatherSnow read from the block in place: passed to it by value, the field is copied whole in every step,
// which makes the march about three times slower. Keep the two the same.
float shaftSnow(float3 P) {
    const float t = field.climate.x - field.climate.y * max(P.y, 0.0);
    float snow = saturate((QUENCH_RAIN_ABOVE - t) / (QUENCH_RAIN_ABOVE - QUENCH_SNOW_BELOW));
    [branch] if (field.climate.z > 0.0) {
        const int count = int(field.layer.w);
        for (int i = 0; i < count; ++i) {
            const float4 a = field.cells[i * 2];
            const float own = field.cells[i * 2 + 1].w;
            if (own < 0.0) continue;
            const float2 d = (P.xz - a.xy) / a.z;
            snow = lerp(snow, own, 1.0 - smoothstep(0.35, 1.0, sqrt(dot(d, d))));
        }
    }
    return snow;
}

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

// The veil of the rain and snow falling under the clouds, marched from rain.shafts.w metres to the scene or the cloud
// base: single scattering of the sky's and the ground's light (and the sun's, with the rainbow where it is on) by the
// precipitation of the clouds' weather map. Snow (its share by the field's temperature at each step) hides more than
// rain does (a heavy snowfall's visibility is about 800 m against the rain's 2.5 km), falls in smoother curtains and
// sends more of the sun's light toward the camera; the part of the precipitation whose kind is not drawn (rain or
// snow off) is left out. Drawn at a reduced size (rain.rainbow.w target pixels per texel), each texel for the target
// pixel at its block's centre; rain_shafts_up_fs blends it as color + destination * transmittance.
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
    // Snow where it can fall: a cell's own, or the air colder than rain at the cloud base.
    const bool snowy = field.climate.z > 0.0 || field.climate.x - field.climate.y * base < QUENCH_RAIN_ABOVE;
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
        float p = weather.b;
        if (p <= 0.01) continue;
        // Fall streaks: the rain is denser in curtains a few hundred metres apart, the same at every height; snow in
        // wider, smoother ones.
        const float large = valueNoise(P.xz / 220.0);
        float snow = 0.0;
        [branch] if (snowy) snow = shaftSnow(P);
        float density = lerp(3.0, 5.0, large);
        [branch] if (snow < 1.0) {
            const float streaks = large * 0.65 + valueNoise(P.xz / 70.0 + 7.3) * 0.35;
            density = lerp(lerp(0.25, 1.75, streaks), density, snow);
        }
        const float3 phase = lerp(sunShare + bow, (float3)(sunShare * 1.5), snow);
        p *= lerp(rain.flakeLight.w, rain.snowFall.w, snow);
        const float sigma = rain.shafts.z * p * density * saturate((base - P.y) / 150.0);
        float3 light = rain.light.rgb * ((1.0 - 0.65 * weather.r) / openHere);
        if (sun) light += rain.sun.rgb * quenchCloudShadow(mapsTex, mapsSampler, P) * phase;
        const float stepT = exp(-sigma * dt);
        color += T * light * (1.0 - stepT);
        T *= stepT;
        // Beyond this the rest adds less than one part in 200 (a heavy snowfall's veil is opaque within kilometres).
        if (T < 0.005) break;
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
