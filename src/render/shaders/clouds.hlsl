// The scene's clouds (src/render/Clouds.cpp, docs/weather.md): the weather map, the clouds' shadow on the ground, the
// raymarched clouds and their temporal accumulation, the sky with the clouds in the HDR pass, the 2D cloud layer (the
// fallback) and the faces of the sky cube for reflections.
//
// The volumetric clouds follow Schneider's cloudscapes for Horizon Zero Dawn ("The real-time volumetric cloudscapes
// of Horizon Zero Dawn", SIGGRAPH 2015; "Nubis: authoring real-time volumetric cloudscapes with the Decima engine",
// 2017): coverage and type from a weather map, a height profile per type, a Perlin-Worley shape eroded by Worley
// detail, Beer-Lambert extinction with a powder term, octaves of multiple scattering (Wrenninge, "Oz: the great and
// volumetric", 2013) and two Henyey-Greenstein lobes; the scattering is integrated per step as in Hillaire's
// "Physically based sky, atmosphere and cloud rendering in Frostbite" (2016). The layer sits on a sphere of the planet's radius, so
// the clouds meet the horizon. The sun's light and the sky's come from the scene's sky (FrameUniforms), so sunsets
// tint the clouds.
//
// Bindings: fragment uniform slot 0 FrameUniforms (quench/Frame.hlsli), slot 1 CloudUniforms (CloudUniforms.h), slot
// 2 WeatherUniforms (weather_map_fs only); textures per entry below.

#include "quench/Bindings.hlsli"
#include "quench/Common.hlsli"
#include "quench/Cube.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/SceneUniforms.h"
#include "quench/Sky.hlsli"
#include "quench/WeatherField.h"

#include "CloudUniforms.h"

cbuffer CloudBlock : QUENCH_UNIFORM(1) {
    CloudUniforms cloud;
};

// ---------------------------------------------------------------- the weather map

#if defined(QUENCH_ENTRY_weather_map_fs)
cbuffer WeatherBlock : QUENCH_UNIFORM(2) {
    WeatherUniforms weather;
};

// One texel of the weather map: the field of quench_weather at its world position (row 0 at the map's smallest z).
float4 weather_map_fs(FullscreenOut vin) : SV_Target0 {
    return quenchWeatherSample(weather, cloud.weatherMap.xy + vin.uv * cloud.weatherMap.w);
}
#endif

// ---------------------------------------------------------------- resources of the cloud entries

#if defined(QUENCH_ENTRY_cloud_march_fs) || defined(QUENCH_ENTRY_sky_cube_fs) || defined(QUENCH_ENTRY_cloud_shadow_fs)
#define QUENCH_CLOUD_VOLUME 1
#endif
#if defined(QUENCH_ENTRY_sky_layer_fs) || defined(QUENCH_ENTRY_sky_cube_layer_fs) || \
    defined(QUENCH_ENTRY_cloud_shadow_layer_fs)
#define QUENCH_CLOUD_LAYER 1
#endif

#if defined(QUENCH_CLOUD_VOLUME) || defined(QUENCH_CLOUD_LAYER)
Texture2D<float4> weatherTex : QUENCH_TEXTURE(0);
SamplerState weatherSampler : QUENCH_SAMPLER(0);
#endif
#if defined(QUENCH_CLOUD_VOLUME)
Texture3D<float4> shapeTex : QUENCH_TEXTURE(1);
SamplerState shapeSampler : QUENCH_SAMPLER(1);
Texture3D<float4> detailTex : QUENCH_TEXTURE(2);
SamplerState detailSampler : QUENCH_SAMPLER(2);
Texture2D<float4> layerTex : QUENCH_TEXTURE(3);
SamplerState layerSampler : QUENCH_SAMPLER(3);
#elif defined(QUENCH_CLOUD_LAYER)
Texture2D<float4> layerTex : QUENCH_TEXTURE(1);
SamplerState layerSampler : QUENCH_SAMPLER(1);
#endif

#if defined(QUENCH_CLOUD_VOLUME) || defined(QUENCH_CLOUD_LAYER)

static const float kInvFourPi = 0.0795775;

// Distances (near, far) along d to the sphere at height h above the planet from a point at height y on the vertical
// of the planet's centre (the camera: x = z = 0 in its own frame); (-1, -1) without a hit. Written without the
// cancellation of |o|^2 - r^2 at a planet's radius.
float2 cloudSphere(float y, float3 d, float h) {
    const float R = cloud.layer.w;
    const float b = (y + R) * d.y;
    const float c = (y - h) * (y + 2.0 * R + h);
    const float disc = b * b - c;
    if (disc < 0.0) return float2(-1.0, -1.0);
    const float s = sqrt(disc);
    const float q = -b - (b >= 0.0 ? s : -s);
    const float t0 = q;
    const float t1 = abs(q) > 1e-6 ? c / q : 0.0;
    return float2(min(t0, t1), max(t0, t1));
}

// Height above the planet of a point at horizontal distance sqrt(r2) from the camera's vertical and height y.
float cloudAltitude(float r2, float y) {
    const float R = cloud.layer.w;
    return (r2 + y * (y + 2.0 * R)) / (sqrt(r2 + (y + R) * (y + R)) + R);
}

// The weather map at a world xz (coverage, type, precipitation, cirrus); coverage and cirrus fade out at its edges.
float4 cloudWeather(float2 xz, float lod) {
    const float2 uv = (xz - cloud.weatherMap.xy) * cloud.weatherMap.z;
    const float2 e = abs(uv - 0.5);
    const float fade = saturate((0.5 - max(e.x, e.y)) * 16.0);
    float4 w = weatherTex.SampleLevel(weatherSampler, uv, lod);
    w.x *= fade;
    w.w *= fade;
    return w;
}

// The height of the cloud tops of a type (Weather::cloudTop): stratus 500 m thick, cumulus 1.5 km, towering cumulus
// 5 km, cumulonimbus up to the layer's top.
float cloudTopOf(float type) {
    const float base = cloud.layer.x;
    const float span = cloud.layer.y - base;
    const float t = saturate(type) * 3.0;
    const float big = max(span, 5000.0);
    float thick;
    if (t < 1.0)
        thick = lerp(500.0, 1500.0, t);
    else if (t < 2.0)
        thick = lerp(1500.0, 5000.0, t - 1.0);
    else
        thick = lerp(5000.0, big, t - 2.0);
    return base + min(thick, span);
}

// A blend of three values over the type scale at stratus, cumulus and cumulonimbus (towering in between).
float byType(float type, float stratus, float cumulus, float towering, float cumulonimbus) {
    const float t = saturate(type) * 3.0;
    if (t < 1.0) return lerp(stratus, cumulus, t);
    if (t < 2.0) return lerp(cumulus, towering, t - 1.0);
    return lerp(towering, cumulonimbus, t - 2.0);
}

// The density profile over a cloud's own height (0 base .. 1 top): flat bases, rounded tops for cumulus, an even layer
// for stratus, straight sides up to the anvil for cumulonimbus.
float cloudProfile(float ht, float type) {
    const float bottom = byType(type, 0.25, 0.08, 0.05, 0.04);
    const float top = byType(type, 0.55, 0.3, 0.5, 0.85);
    return smoothstep(0.0, bottom, ht) * (1.0 - smoothstep(top, 1.0, ht));
}

float remapClamped(float v, float lo, float hi, float newLo, float newHi) {
    return saturate(newLo + (v - lo) / max(hi - lo, 1e-5) * (newHi - newLo));
}

// Two Henyey-Greenstein lobes, forward (the silver lining toward the sun) and a little backward, per steradian.
float cloudPhase(float mu, float k) {
    return lerp(phaseHG(mu, 0.8 * k), phaseHG(mu, -0.3 * k), 0.3) * kInvFourPi;
}

// Fades distant clouds into the sky behind them (aerial perspective; grey under a closing cover) and returns the
// premultiplied result.
float4 cloudAerial(float3 L, float T, float distance, float3 dir) {
    const float haze = 1.0 - exp(-distance / 45000.0);
    const float3 sky = lerp(skyRadiance(dir), quenchFrame.overcast.rgb, quenchFrame.overcast.w);
    return float4(lerp(L, sky * (1.0 - T), haze * 0.85), T);
}

// The layer's 2D clouds and the cirrus read the layer noise in the air's coordinates (they drift with the wind).
float2 cloudAirXz(float2 xz) { return xz - cloud.air.xy; }

// High cirrus behind the other clouds: thin ice streaks along the wind at the cirrus height.
float4 cloudCirrus(float3 dir) {
    if (dir.y <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);
    const float tc = cloudSphere(quenchFrame.cameraPos.y, dir, cloud.layer.z).y;
    if (tc <= 0.0 || tc > 250000.0) return float4(0.0, 0.0, 0.0, 1.0);
    const float2 xz = quenchFrame.cameraPos.xz + dir.xz * tc;
    const float amount = cloudWeather(xz, 1.0).w;
    if (amount <= 0.002) return float4(0.0, 0.0, 0.0, 1.0);
    const float2 pa = cloudAirXz(xz);
    const float along = dot(pa, cloud.air.zw);
    const float across = dot(pa, float2(-cloud.air.w, cloud.air.z));
    // Bands along the wind (the layer noise's Perlin fbms, each about normal around 0.5 with a deviation near 0.09,
    // here scaled to about 0.6) over a threshold that falls as the amount grows, with fine fibres along them.
    const float4 n = layerTex.SampleLevel(layerSampler, float2(along / 30000.0, across / 7000.0), 0.0);
    const float4 m = layerTex.SampleLevel(layerSampler, float2(along / 6000.0, across / 1400.0) + 0.37, 0.0);
    const float4 f = layerTex.SampleLevel(layerSampler, float2(along / 12000.0, across / 500.0) + 0.71, 0.0);
    const float z = (n.r - 0.5) * 6.0 + (m.b - 0.5) * 4.0;
    const float streak = saturate((z - (1.0 - amount) * 1.5) * 1.5) * saturate(0.5 + (f.b - 0.5) * 5.0);
    const float a = streak * 0.6;
    const float mu = dot(dir, quenchFrame.sunDir.xyz);
    // Thin ice: the sun's forward lobe and its light scattered on within the streak, the sky's from above.
    const float3 E = cloud.sun.rgb * (phaseHG(mu, 0.6) * kInvFourPi * 1.6 + 0.12) +
                     cloud.ambientTop.rgb * (1.0 / QUENCH_PI);
    return cloudAerial(E * a, 1.0 - a * 0.85, tc, dir);
}

// Puts `back` behind `front` (both premultiplied radiance with transmittance).
float4 cloudOver(float4 front, float4 back) { return float4(front.rgb + front.a * back.rgb, front.a * back.a); }

#endif

// ---------------------------------------------------------------- the volumetric clouds

#if defined(QUENCH_CLOUD_VOLUME)

// The cloud density (0..1) at a world xz and an altitude: the weather map's coverage under the type's profile, the
// anvil of cumulonimbus at the top, the Perlin-Worley shape and, with `detail`, the Worley erosion of the edges.
float cloudDensity(float2 xz, float alt, bool detail, float detailFade, out float ht, out float thickness,
                   out float rain) {
    const float base = cloud.layer.x;
    ht = 0.0;
    thickness = 0.0;
    rain = 0.0;
    if (alt < base || alt > cloud.params.w) return 0.0;
    const float4 w = cloudWeather(xz, 0.0);
    const float type = w.y;
    const float top = cloudTopOf(type);
    thickness = max(top - base, 1.0);
    ht = (alt - base) / thickness;
    rain = w.z;
    float coverage = ht < 1.0 ? w.x * cloudProfile(ht, type) : 0.0;
    // The anvil: near the layer's top a cumulonimbus spreads over the blurred map (a wider footprint).
    if (alt > cloud.layer.y - 1800.0) {
        const float4 wide = cloudWeather(xz, 3.0);
        const float band = smoothstep(cloud.layer.y - 1800.0, cloud.layer.y - 900.0, alt) *
                           (1.0 - smoothstep(cloud.layer.y - 350.0, cloud.layer.y, alt));
        coverage = max(coverage, smoothstep(0.8, 1.0, wide.y) * saturate(wide.x * 1.6) * band);
    }
    if (coverage <= 0.002) return 0.0;

    // The air's coordinates, tops leaning downwind, the shape rising slowly (it boils as it evolves). The shape is
    // spread evenly over 0..1 (cloudnoise.hlsl): the share of the air above 1 - coverage holds cloud, with edges
    // over a fifth of the shape's range; from coverage 0.8 on the gaps close (an overcast deck has no holes).
    const float tau = cloud.shape.z;
    float3 pa = float3(xz.x - cloud.air.x, alt, xz.y - cloud.air.y);
    pa.xz += cloud.air.zw * min(alt - base, 3000.0) * 0.15;
    const float shape =
        shapeTex.SampleLevel(shapeSampler, (pa + float3(0.0, tau * 1500.0, 0.0)) * cloud.shape.x, 0.0).r;
    float c = saturate((shape - (1.0 - coverage) + 0.12 * smoothstep(0.8, 1.0, coverage)) * 5.0);
    if (detail && c > 0.0 && detailFade > 0.0) {
        const float3 q = (pa + float3(tau * 400.0, tau * 1100.0, -tau * 300.0)) * cloud.shape.y;
        const float4 dn = detailTex.SampleLevel(detailSampler, q, 0.0);
        const float d = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
        // Wispy at the base, billowing higher up.
        const float erosion = lerp(d, 1.0 - d, saturate(ht * 4.0)) * 0.35 * detailFade;
        c = saturate((c - erosion) / max(1.0 - erosion, 1e-3));
    }
    return c;
}

float cloudDensity(float2 xz, float alt, bool detail, float detailFade) {
    float ht, thickness, rain;
    return cloudDensity(xz, alt, detail, detailFade, ht, thickness, rain);
}

// Optical depth toward the sun from a point in the clouds: six steps growing to about 3 km.
float cloudSunDepth(float2 xz, float alt, float3 sun, int steps) {
    float od = 0.0;
    float dist = 0.0;
    float step = 60.0;
    for (int i = 0; i < steps; ++i) {
        const float mid = dist + step * 0.5;
        od += cloudDensity(xz + sun.xz * mid, alt + sun.y * mid, i < 2, 1.0) * step;
        dist += step;
        step *= 1.9;
    }
    return od;
}

// The light a cloud sample scatters toward the camera (per unit of extinction): the sun through the octaves of
// multiple scattering with the powder term, and the sky's light from above and the ground's from below by the
// sample's height in its own cloud (ht of `thickness`), the light from above dimmed by the cloud over the sample (an
// estimate from the local density) and the bases of raining clouds darker.
float3 cloudScatter(float2 xz, float alt, float density, float ht, float thickness, float rain, float mu,
                    int lightSteps) {
    const float above = (1.0 - saturate(ht)) * thickness * density * cloud.shape.w * 0.15;
    const float3 top = cloud.ambientTop.rgb * lerp(0.3, 1.0, exp(-above * 0.2));
    float3 S = lerp(cloud.ambientBottom.rgb, top, saturate(ht * 1.2 + 0.1)) * (1.0 / QUENCH_PI);
    S *= 1.0 - 0.5 * rain * (1.0 - saturate(ht * 2.0));
    if (cloud.sun.w > 0.0) {
        const float3 sun = quenchFrame.sunDir.xyz;
        const float od = cloudSunDepth(xz, alt, sun, lightSteps) * cloud.shape.w;
        float ms = 0.0;
        float a = 1.0;
        float b = 1.0;
        float k = 1.0;
        for (int i = 0; i < 4; ++i) {
            ms += a * cloudPhase(mu, k) * exp(-od * b);
            a *= 0.7;
            b *= 0.3;
            k *= 0.5;
        }
        // Powder: the edges facing away from the sun are darker (light has not yet scattered toward the camera).
        const float powder = 1.0 - exp(-2.0 * (od + density * 30.0 * cloud.shape.w));
        S += cloud.sun.rgb * ms * lerp(powder, 1.0, saturate(mu * 0.5 + 0.5)) * 2.4;
    }
    return S;
}

// The ray's distances inside the cloud layer from the camera: (start, end); end <= start: none.
float2 cloudSegment(float3 dir) {
    const float y = quenchFrame.cameraPos.y;
    const float base = cloud.layer.x;
    const float top = cloud.params.w;
    const float2 inner = cloudSphere(y, dir, base);
    const float2 outer = cloudSphere(y, dir, top);
    float2 seg = float2(0.0, -1.0);
    if (y < base) {
        // Below the clouds: from the base to the top; rays below the horizon meet the ground first.
        if (dir.y < -0.01) return seg;
        seg = float2(inner.y, outer.y);
    } else if (y < top) {
        seg = float2(0.0, inner.x > 0.0 ? inner.x : outer.y);
    } else if (outer.x > 0.0) {
        seg = float2(outer.x, inner.x > 0.0 ? inner.x : outer.y);
    }
    seg.x = max(seg.x, 0.0);
    seg.y = min(seg.y, seg.x + cloud.march.z);
    return seg;
}

// Marches the clouds along a view ray: premultiplied radiance and transmittance (behind the clouds: the cirrus);
// `depth` is the distance of the clouds' weight along the ray (for the reprojection). Without `detail` the shapes are
// not eroded (the sky cube's blurred reflections).
float4 marchClouds(float3 dir, float jitter, int steps, int lightSteps, bool detail, out float depth) {
    const float2 seg = cloudSegment(dir);
    const float4 cirrus = cloudCirrus(dir);
    depth = 60000.0;
    if (seg.y <= seg.x) return cirrus;
    // Steps that grow with the distance, each a constant share of it: fine steps through near clouds, coarse ones
    // through far clouds, which are smaller on the screen.
    const float t0 = max(seg.x, 100.0);
    const float growth = exp(log(max(seg.y, t0 + 1.0) / t0) / float(steps));
    const float mu = dot(dir, quenchFrame.sunDir.xyz);
    const float sigma = cloud.shape.w;
    const float3 cam = quenchFrame.cameraPos.xyz;
    float T = 1.0;
    float3 L = float3(0.0, 0.0, 0.0);
    float depthSum = 0.0;
    float weight = 0.0;
    float ta = t0;
    for (int i = 0; i < steps; ++i) {
        if (T < 0.01) break;
        const float dt = ta * (growth - 1.0);
        const float t = ta + dt * jitter;
        ta += dt;
        const float2 off = dir.xz * t;
        const float alt = cloudAltitude(dot(off, off), cam.y + dir.y * t);
        const float2 xz = cam.xz + off;
        // Detail fades out far away, where it would only alias.
        float ht, thickness, rain;
        const float d = cloudDensity(xz, alt, detail, 1.0 - smoothstep(20000.0, 45000.0, t), ht, thickness, rain);
        if (d > 0.002) {
            const float Tr = exp(-d * sigma * dt);
            const float3 S = cloudScatter(xz, alt, d, ht, thickness, rain, mu, lightSteps);
            const float absorbed = T * (1.0 - Tr);
            L += absorbed * S;
            depthSum += t * absorbed;
            weight += absorbed;
            T *= Tr;
        }
    }
    depth = weight > 1e-4 ? depthSum / weight : seg.y;
    return cloudOver(cloudAerial(L, T, depth, dir), cirrus);
}

#endif

// ---------------------------------------------------------------- the 2D layer (the fallback)

#if defined(QUENCH_CLOUD_LAYER)

static const float kLayerHeight = 400.0; // above the cloud base

// The layer's density at a world xz under the weather map's coverage.
float layerDensity(float2 xz, float4 w) {
    if (w.x <= 0.002) return 0.0;
    const float2 pa = cloudAirXz(xz);
    const float tau = cloud.shape.z;
    const float4 n = layerTex.SampleLevel(layerSampler, pa / 9000.0 + float2(tau * 0.05, 0.0), 0.0);
    const float4 m = layerTex.SampleLevel(layerSampler, pa / 2300.0 - float2(0.0, tau * 0.11), 0.0);
    // About normal with mean 0.486 and deviation 0.058 (measured over the tile), spread evenly over 0..1 like the
    // volume's shape: the share above 1 - coverage holds cloud; the fine Worley noise frays the edges.
    const float shape = n.r * 0.5 + n.g * 0.3 + m.g * 0.2;
    const float even = 1.0 / (1.0 + exp(-(shape - 0.486) * 29.2));
    const float c = saturate((even - (1.0 - w.x) + 0.12 * smoothstep(0.8, 1.0, w.x)) * 4.0);
    const float fray = (1.0 - m.a) * 0.3;
    return saturate((c - fray) / (1.0 - fray));
}

float4 cloudLayer(float3 dir) {
    // Seen from under the layer above the horizon; from over it (a camera flying over a sea of clouds) below it,
    // lit on its tops.
    const float height = cloud.layer.x + kLayerHeight;
    const bool over = quenchFrame.cameraPos.y > height;
    const float4 cirrus = cloudCirrus(dir);
    if (over ? dir.y >= 0.0 : dir.y < 0.0) return cirrus;
    const float2 hit = cloudSphere(quenchFrame.cameraPos.y, dir, height);
    const float t = over ? hit.x : hit.y;
    if (t <= 0.0) return cirrus;
    const float2 xz = quenchFrame.cameraPos.xz + dir.xz * t;
    const float4 w = cloudWeather(xz, 0.0);
    const float d = layerDensity(xz, w);
    if (d <= 0.002) return cirrus;
    const float thickness = byType(w.y, 300.0, 1200.0, 2500.0, 4000.0);
    const float od = d * thickness * cloud.shape.w * 0.25;
    const float T = exp(-od);
    const float mu = dot(dir, quenchFrame.sunDir.xyz);
    float3 S = lerp(cloud.ambientBottom.rgb, cloud.ambientTop.rgb, 0.6) * (1.0 / QUENCH_PI) *
               lerp(1.0, 0.45, saturate(od * 0.08));
    if (over) {
        // The tops: the clear sky's light and the sun's, diffused by the cloud and a little toward it.
        S = cloud.ambientTop.rgb * (1.0 / QUENCH_PI);
        if (cloud.sun.w > 0.0)
            S += cloud.sun.rgb * (saturate(quenchFrame.sunDir.y) * 0.25 + 0.03 + cloudPhase(mu, 1.0) * 0.6);
    } else if (cloud.sun.w > 0.0) {
        const float lightOd = od * lerp(1.6, 0.6, saturate(quenchFrame.sunDir.y * 2.0));
        S += cloud.sun.rgb * (cloudPhase(mu, 1.0) * exp(-lightOd * 0.5) + 0.06 * exp(-lightOd * 0.08)) * 2.4;
    }
    return cloudOver(cloudAerial(S * (1.0 - T), T, t, dir), cirrus);
}

#endif

// ---------------------------------------------------------------- cloud shadows

// The transmittance of the cloud layer above a point of the ground plane y = 0 along the sun's direction (the map is
// read by quenchCloudShadow in quench/Clouds.hlsli).
#if defined(QUENCH_ENTRY_cloud_shadow_fs) || defined(QUENCH_ENTRY_cloud_shadow_layer_fs)
float4 cloudShadowTexel(float2 uv) {
    // The texel's point on the map's plane (y = shadowMap.w); only the clouds above it count.
    const float plane = cloud.shadowMap.w;
    const float2 xz = cloud.shadowMap.xy + uv / cloud.shadowMap.z;
    const float3 sun = quenchFrame.sunDir.xyz;
    if (quenchFrame.sunDir.w <= 0.0 || sun.y <= 0.0) return float4(1.0, 1.0, 1.0, 1.0);
    const float sy = max(sun.y, 0.05);
    const float2 slope = sun.xz / sy; // xz per metre of height
#if defined(QUENCH_ENTRY_cloud_shadow_fs)
    const float base = max(cloud.layer.x, plane);
    const float top = cloud.params.w;
    if (top <= base) return float4(1.0, 1.0, 1.0, 1.0);
    const int steps = 16;
    const float dh = (top - base) / float(steps);
    float od = 0.0;
    for (int i = 0; i < steps; ++i) {
        const float h = base + (float(i) + 0.5) * dh;
        od += cloudDensity(xz + slope * (h - plane), h, false, 0.0) * dh / sy;
    }
    const float T = exp(-od * cloud.shape.w);
#else
    const float h = cloud.layer.x + kLayerHeight;
    if (h <= plane) return float4(1.0, 1.0, 1.0, 1.0);
    const float2 p = xz + slope * (h - plane);
    const float4 w = cloudWeather(p, 0.0);
    const float d = layerDensity(p, w);
    const float T = exp(-d * byType(w.y, 300.0, 1200.0, 2500.0, 4000.0) * cloud.shape.w * 0.25 / sy);
#endif
    return float4(T, T, T, 1.0);
}
#endif

#if defined(QUENCH_ENTRY_cloud_shadow_fs)
float4 cloud_shadow_fs(FullscreenOut vin) : SV_Target0 { return cloudShadowTexel(vin.uv); }
#endif
#if defined(QUENCH_ENTRY_cloud_shadow_layer_fs)
float4 cloud_shadow_layer_fs(FullscreenOut vin) : SV_Target0 { return cloudShadowTexel(vin.uv); }
#endif

// ---------------------------------------------------------------- the march, its accumulation and the sky

// A view ray without the TAA jitter through a uv of the target (top left 0, 0), read at depth 0 as quenchViewRay is.
float3 cloudViewRay(float2 uv) {
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(mul(cloud.invViewProjStable, float4(ndc, 0.0, 1.0)).xyz);
}

#if defined(QUENCH_ENTRY_cloud_march_fs)
struct CloudMarchOut {
    float4 color : SV_Target0; // premultiplied radiance, transmittance
    float4 depth : SV_Target1; // r: the clouds' distance (m)
};

CloudMarchOut cloud_march_fs(FullscreenOut vin) {
    const float3 dir = cloudViewRay(vin.uv);
    // A different start in every step for every pixel and frame: the temporal accumulation averages them.
    const float jitter = frac(ign(vin.position.xy) + cloud.march.w);
    float depth;
    CloudMarchOut o;
    o.color = marchClouds(dir, jitter, int(cloud.march.x), int(cloud.march.y), true, depth);
    o.depth = float4(depth, 0.0, 0.0, 1.0);
    return o;
}
#endif

#if defined(QUENCH_ENTRY_cloud_temporal_fs)
Texture2D<float4> currentTex : QUENCH_TEXTURE(0);
SamplerState currentSampler : QUENCH_SAMPLER(0);
Texture2D<float4> depthTex : QUENCH_TEXTURE(1);
SamplerState depthSampler : QUENCH_SAMPLER(1);
Texture2D<float4> historyTex : QUENCH_TEXTURE(2);
SamplerState historySampler : QUENCH_SAMPLER(2);

// The march reprojected over the frames: the history where the clouds were last frame (by their distance), kept within
// the range of this frame's neighbourhood, blended with this frame.
float4 cloud_temporal_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    const float4 current = currentTex.Load(int3(p, 0));
    if (cloud.temporal.y <= 0.0) return current;
    float4 lo = current;
    float4 hi = current;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const float4 n = currentTex.Load(int3(clamp(p + int2(x, y), int2(0, 0), int2(cloud.target.xy) - 1), 0));
            lo = min(lo, n);
            hi = max(hi, n);
        }
    const float depth = depthTex.Load(int3(p, 0)).r;
    const float3 world = quenchFrame.cameraPos.xyz + cloudViewRay(vin.uv) * depth;
    const float4 clip = mul(cloud.prevViewProjStable, float4(world, 1.0));
    if (clip.w <= 0.0) return current;
    const float2 ndc = clip.xy / clip.w;
    const float2 prevUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(prevUv < 0.0) || any(prevUv > 1.0)) return current;
    const float4 history = clamp(historyTex.SampleLevel(historySampler, prevUv, 0.0), lo, hi);
    return lerp(history, current, cloud.temporal.x);
}
#endif

// The sky behind the clouds, as sky_fs draws it (quench/Sky.hlsli), with the clouds over it and the horizon haze
// after them, so that distant clouds meet the fog. Within a few degrees of the horizon, beyond the clouds of the
// weather map, the sky turns toward the cloud bases' grey as the cover closes (the clouds go on past the map).
float3 cloudySkyColor(float3 dir, float4 clouds, bool sunDisc) {
    float3 col = skyRadiance(dir);
    if (sunDisc) col = quenchSunDisc(col, dir);
    col = lerp(col, quenchFrame.overcast.rgb, quenchFrame.overcast.w * saturate(1.0 - abs(dir.y) * 10.0));
    // Under the cloud base the clouds are above the horizon; over it (a camera flying over a sea of clouds) below it
    // too.
    const bool seen = dir.y > -0.02 || quenchFrame.cameraPos.y > cloud.layer.x;
    if (seen) col = col * clouds.a + clouds.rgb;
    // A flash of lightning lights the clouds around it from within, and the air a little everywhere.
    [branch] if (quenchFrame.lightning.w > 0.0 && seen) {
        const float around = exp((dot(dir, quenchFrame.lightning.xyz) - 1.0) * 6.0);
        col += quenchFrame.lightningColor.rgb * (quenchFrame.lightning.w * (around * (1.0 - clouds.a) + 0.1));
    }
    if (quenchFrame.fogParams.x > 0.0) {
        const float haze = pow(1.0 - saturate(abs(dir.y)), 6.0) * 0.6 * quenchFrame.fogColor.w;
        col = lerp(col, hazeColor(dir), haze);
    }
    return col;
}

#if defined(QUENCH_ENTRY_sky_clouds_fs)
Texture2D<float4> cloudTex : QUENCH_TEXTURE(0);
SamplerState cloudSampler : QUENCH_SAMPLER(0);

// The sky pass with the volumetric clouds: their accumulated march, upsampled.
float4 sky_clouds_fs(FullscreenOut vin) : SV_Target0 {
    const float3 dir = quenchViewRay(vin.uv);
    return float4(cloudySkyColor(dir, cloudTex.SampleLevel(cloudSampler, vin.uv, 0.0), true), 1.0);
}
#endif

#if defined(QUENCH_ENTRY_sky_layer_fs)
// The sky pass with the 2D cloud layer.
float4 sky_layer_fs(FullscreenOut vin) : SV_Target0 {
    const float3 dir = quenchViewRay(vin.uv);
    return float4(cloudySkyColor(dir, cloudLayer(dir), true), 1.0);
}
#endif

#if defined(QUENCH_ENTRY_sky_cube_fs)
// A face of the sky cube: the sky and the volumetric clouds without the sun disc (surfaces add the sun themselves).
float4 sky_cube_fs(FullscreenOut vin) : SV_Target0 {
    const float3 dir = quenchCubeDirection(int(cloud.params.y), vin.uv);
    float depth;
    const float4 clouds = marchClouds(dir, 0.5, int(cloud.march.x), int(cloud.march.y), false, depth);
    return float4(cloudySkyColor(dir, clouds, false), 1.0);
}
#endif

#if defined(QUENCH_ENTRY_sky_cube_layer_fs)
float4 sky_cube_layer_fs(FullscreenOut vin) : SV_Target0 {
    const float3 dir = quenchCubeDirection(int(cloud.params.y), vin.uv);
    return float4(cloudySkyColor(dir, cloudLayer(dir), false), 1.0);
}
#endif
