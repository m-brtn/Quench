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

// 1 inside the weather map, falling to 0 over the outer eighth of its half width: where the clouds drawn end.
float cloudEdgeFade(float2 xz) {
    const float2 e = abs((xz - cloud.weatherMap.xy) * cloud.weatherMap.z - 0.5);
    return saturate((0.5 - max(e.x, e.y)) * 16.0);
}

// The weather map at a world xz (coverage, type, precipitation, cirrus); the cirrus fades out at its edges. The
// coverage does not: what is drawn of the clouds thins out there (cloudEdgeFade on the density a view ray meets, or on
// the 2D layer's coverage), while the clouds that shade them from the sun go on past the map. Fading the coverage
// instead broke a deck's rim into bits with the sun on them, a row of bright dots along the horizon.
float4 cloudWeather(float2 xz, float lod) {
    float4 w = weatherTex.SampleLevel(weatherSampler, (xz - cloud.weatherMap.xy) * cloud.weatherMap.z, lod);
    w.w *= cloudEdgeFade(xz);
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
    const float3 E = cloud.sun.rgb * (phaseHG(mu, 0.6) * kInvFourPi * 1.6 + 0.12) + cloud.ambientTop.rgb * 0.6;
    return cloudAerial(E * a, 1.0 - a * 0.85, tc, dir);
}

// Puts `back` behind `front` (both premultiplied radiance with transmittance).
float4 cloudOver(float4 front, float4 back) { return float4(front.rgb + front.a * back.rgb, front.a * back.a); }

#endif

// ---------------------------------------------------------------- the volumetric clouds

#if defined(QUENCH_CLOUD_VOLUME)

// The cloud density (0..1) at a world xz and an altitude: the weather map's coverage under the type's profile, the
// anvil of cumulonimbus at the top, the Perlin-Worley shape and, with `detail`, the Worley erosion of the edges.
// `body` is the density before the erosion: how deep in its cloud the point lies, for the light.
float cloudDensity(float2 xz, float alt, bool detail, float detailFade, out float ht, out float thickness,
                   out float rain, out float body) {
    const float base = cloud.layer.x;
    ht = 0.0;
    thickness = 0.0;
    rain = 0.0;
    body = 0.0;
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
    body = c;
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
    float ht, thickness, rain, body;
    return cloudDensity(xz, alt, detail, detailFade, ht, thickness, rain, body);
}

// Optical depth toward the sun from a point in the clouds: steps growing to about 3 km (six start at 60 m; fewer
// are longer and reach as far: the sky cube's three would else end 400 m away, in the gaps of a deck's ragged
// base, and put the sun's glow through the deck into every reflection), each read at `jitter` of its length (the
// march's own, so the frames average what lies along the way: read at fixed distances, the edge of a cloud toward
// the sun passes from one step to the next in lines across the clouds seen against the light). Where
// the last step is still in a cloud, the cloud is taken to go on as it is there up to its top: under a deck and a
// low sun the way out is tens of kilometres long, and the steps alone would let the sun's colour through to the
// bases.
float cloudSunDepth(float2 xz, float alt, float3 sun, int steps, float jitter) {
    float od = 0.0;
    float dist = 0.0;
    float step = 2762.0 / (pow(1.9, float(steps)) - 1.0);
    float mid = 0.0;
    float d = 0.0;
    float ht = 0.0;
    float thickness = 0.0;
    for (int i = 0; i < steps; ++i) {
        float rain, body;
        mid = dist + step * jitter;
        d = cloudDensity(xz + sun.xz * mid, alt + sun.y * mid, i < 2, 1.0, ht, thickness, rain, body);
        od += d * step;
        dist += step;
        step *= 1.9;
    }
    const float above = (1.0 - saturate(ht)) * thickness - sun.y * (dist - mid);
    return od + d * max(above, 0.0) / max(sun.y, 0.05);
}

// The share of the light falling on one side of a cloud that is left, as diffuse light, at an optical depth tau
// into it: droplets absorb nothing and scatter mostly forward, so light is lost only by turning back, slowly (the
// two-stream solution of a layer that scatters without loss, 1 / (1 + (1 - g) tau 3/4) with g near 0.85, here a
// little steeper: a stratus deck of optical depth 8 passes a third, as the ground's light under a closed cover
// assumes, a cumulonimbus a few percent).
float cloudDiffuse(float tau) { return 1.0 / (1.0 + QUENCH_CLOUD_DIFFUSE * tau); }

// The light a cloud sample scatters toward the camera (per unit of extinction), in the scene's units (what a white
// surface would send back). The sun through the octaves of multiple scattering with the powder term: the first
// orders, which shape the lit sides. Then the diffuse light deep in the cloud, by the sample's height in its own
// cloud (ht of `thickness`) and an estimate of the cloud over and under it from the density of the cloud's body
// there (`body`: before the erosion, whose fine grain would show in the light as lines): the sky's light and the
// sun's from above (the sun's growing in over the first optical depths: at the lit surface the octaves have it),
// what the ground sends up from below; the bases of raining clouds darker. A cloud counts as at most
// QUENCH_CLOUD_DEEP m thick: light also comes in through the sides of a tall one, and the ground's light under a
// cover (Clouds.cpp) takes the same limit. A thick cloud's base is as bright as the light it lets through to the
// ground, and its creases, where the cloud is dense, darker than its edges.
float3 cloudScatter(float2 xz, float alt, float density, float body, float ht, float thickness, float rain, float mu,
                    int lightSteps, float jitter) {
    const float column = min(thickness, QUENCH_CLOUD_DEEP) * body * cloud.shape.w * QUENCH_CLOUD_COLUMN;
    const float over = (1.0 - saturate(ht)) * column;
    const float down = cloudDiffuse(over);
    const float up = cloudDiffuse(saturate(ht) * column);
    float3 S = cloud.ambientTop.rgb * down + cloud.sunDeep.rgb * (down * (1.0 - exp(-0.5 * over))) +
               cloud.ambientBottom.rgb * (0.5 * up);
    S *= 1.0 - 0.5 * rain * (1.0 - saturate(ht * 2.0));
    if (cloud.sun.w > 0.0) {
        const float3 sun = quenchFrame.sunDir.xyz;
        const float od = cloudSunDepth(xz, alt, sun, lightSteps, jitter) * cloud.shape.w;
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
        S += cloud.sun.rgb * ms * lerp(powder, 1.0, saturate(mu * 0.5 + 0.5)) * QUENCH_CLOUD_SUN;
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
    // through far clouds, which are smaller on the screen. The jitter moves the whole row of steps along the ray, not
    // each sample inside a cell that stays where it is: with fixed cells the light of the first sample inside a cloud
    // depends on where the cloud's surface lies in its cell, which no number of frames averages out (fine lines
    // like a map's contours over the clouds, each the shell of one step).
    const float t0 = max(seg.x, 100.0);
    const float logGrowth = log(max(seg.y, t0 + 1.0) / t0) / float(steps);
    const float width = exp(0.5 * logGrowth) - exp(-0.5 * logGrowth);
    const float mu = dot(dir, quenchFrame.sunDir.xyz);
    const float sigma = cloud.shape.w;
    const float3 cam = quenchFrame.cameraPos.xyz;
    float T = 1.0;
    float3 L = float3(0.0, 0.0, 0.0);
    float depthSum = 0.0;
    float weight = 0.0;
    float3 last = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < steps; ++i) {
        // Nothing shows through the last hundredth; the cloud behind it is taken to go on as it is here (left open,
        // that hundredth of the sun's disc shone through a closed deck).
        if (T < 0.01) {
            L += T * last;
            T = 0.0;
            break;
        }
        const float t = t0 * exp(logGrowth * (float(i) + jitter));
        const float dt = t * width;
        const float2 off = dir.xz * t;
        const float alt = cloudAltitude(dot(off, off), cam.y + dir.y * t);
        const float2 xz = cam.xz + off;
        // Detail fades out far away, where it would only alias.
        // What is drawn thins out toward the map's edge and ends there (nothing is read past it).
        const float drawn = cloudEdgeFade(xz);
        float ht = 0.0;
        float thickness = 0.0;
        float rain = 0.0;
        float body = 0.0;
        float d = 0.0;
        [branch] if (drawn > 0.0) {
            d = drawn *
                cloudDensity(xz, alt, detail, 1.0 - smoothstep(20000.0, 45000.0, t), ht, thickness, rain, body);
        }
        if (d > 0.002) {
            const float Tr = exp(-d * sigma * dt);
            // The light's steps at a place of their own in every step of the march.
            const float3 S = cloudScatter(xz, alt, d, body, ht, thickness, rain, mu, lightSteps,
                                          frac(jitter + 0.618034 * float(i + 1)));
            const float absorbed = T * (1.0 - Tr);
            last = S;
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
    float4 w = cloudWeather(xz, 0.0);
    w.x *= cloudEdgeFade(xz);
    const float d = layerDensity(xz, w);
    if (d <= 0.002) return cirrus;
    const float thickness = byType(w.y, 300.0, 1200.0, 2500.0, 4000.0);
    const float od = d * thickness * cloud.shape.w * 0.25;
    const float T = exp(-od);
    const float mu = dot(dir, quenchFrame.sunDir.xyz);
    // The base: the light the layer lets through from above (as the volume's clouds do) and the ground's.
    const float deep = 1.0 / (1.0 + QUENCH_CLOUD_DIFFUSE * od * 2.4);
    float3 S = (cloud.ambientTop.rgb + cloud.sunDeep.rgb * (1.0 - exp(-1.2 * od))) * deep +
               cloud.ambientBottom.rgb * 0.5;
    if (over) {
        // The tops: the clear sky's light and the sun's, diffused by the cloud and a little toward it.
        S = cloud.ambientTop.rgb;
        if (cloud.sun.w > 0.0)
            S += cloud.sun.rgb * (saturate(quenchFrame.sunDir.y) * 0.25 + 0.03 + cloudPhase(mu, 1.0) * 0.6);
    } else if (cloud.sun.w > 0.0) {
        const float lightOd = od * lerp(1.6, 0.6, saturate(quenchFrame.sunDir.y * 2.0));
        S += cloud.sun.rgb * (cloudPhase(mu, 1.0) * exp(-lightOd * 0.5) + 0.06 * exp(-lightOd * 0.08)) *
             QUENCH_CLOUD_SUN;
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
    // A different start in every step for every pixel and frame: the temporal accumulation averages them. The noise is
    // read at a place of its own in every frame (cloud.temporal.zw, whole pixels), not shifted by a constant step: on
    // a turning camera the history of a pixel comes from its neighbours, and noise that only steps on would repeat
    // along that path in stripes that move with the view.
    const float jitter = ign(vin.position.xy + cloud.temporal.zw);
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

// The history at a uv, Catmull-Rom filtered from nine bilinear reads, so that it stays sharp when the view moves:
// a bilinear read blurs the history a little in every frame it moves by a fraction of a texel, and the blur builds up
// over the frames the history lasts (the clouds smear while the camera turns).
float4 cloudHistoryAt(float2 uv) {
    const float2 size = cloud.target.xy;
    const float2 centre = floor(uv * size - 0.5) + 0.5;
    const float2 f = uv * size - centre;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (centre - 1.0) * cloud.target.zw;
    const float2 t12 = (centre + w2 / w12) * cloud.target.zw;
    const float2 t3 = (centre + 2.0) * cloud.target.zw;
    float4 sum = historyTex.SampleLevel(historySampler, float2(t0.x, t0.y), 0.0) * (w0.x * w0.y);
    sum += historyTex.SampleLevel(historySampler, float2(t12.x, t0.y), 0.0) * (w12.x * w0.y);
    sum += historyTex.SampleLevel(historySampler, float2(t3.x, t0.y), 0.0) * (w3.x * w0.y);
    sum += historyTex.SampleLevel(historySampler, float2(t0.x, t12.y), 0.0) * (w0.x * w12.y);
    sum += historyTex.SampleLevel(historySampler, float2(t12.x, t12.y), 0.0) * (w12.x * w12.y);
    sum += historyTex.SampleLevel(historySampler, float2(t3.x, t12.y), 0.0) * (w3.x * w12.y);
    sum += historyTex.SampleLevel(historySampler, float2(t0.x, t3.y), 0.0) * (w0.x * w3.y);
    sum += historyTex.SampleLevel(historySampler, float2(t12.x, t3.y), 0.0) * (w12.x * w3.y);
    sum += historyTex.SampleLevel(historySampler, float2(t3.x, t3.y), 0.0) * (w3.x * w3.y);
    return max(sum, 0.0);
}

// The march reprojected over the frames: the history where the clouds were last frame (by their distance), kept within
// the range of this frame's neighbourhood, blended with this frame. This frame enters through a 3x3 tent: its noise
// (the march's jitter) is mostly of the finest scale, which the tent takes out, while the clouds' shapes are wider;
// what the accumulation leaves of it would otherwise show as a fine hatching that TAA's clamp brings out when the
// view moves. Where there is no history (the edge a turning view reveals, the first frame) the tent alone is used.
float4 cloud_temporal_fs(FullscreenOut vin) : SV_Target0 {
    const int2 p = int2(vin.position.xy);
    float4 lo = float4(1e9, 1e9, 1e9, 1e9);
    float4 hi = float4(-1e9, -1e9, -1e9, -1e9);
    float4 current = float4(0.0, 0.0, 0.0, 0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            const float4 n = currentTex.Load(int3(clamp(p + int2(x, y), int2(0, 0), int2(cloud.target.xy) - 1), 0));
            lo = min(lo, n);
            hi = max(hi, n);
            current += n * ((x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0) / 16.0);
        }
    if (cloud.temporal.y <= 0.0) return current;
    const float depth = depthTex.Load(int3(p, 0)).r;
    const float3 world = quenchFrame.cameraPos.xyz + cloudViewRay(vin.uv) * depth;
    const float4 clip = mul(cloud.prevViewProjStable, float4(world, 1.0));
    const float2 ndc = clip.xy / max(clip.w, 1e-6);
    const float2 prevUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (clip.w <= 0.0 || any(prevUv < 0.0) || any(prevUv > 1.0)) return current;
    const float4 history = clamp(cloudHistoryAt(prevUv), lo, hi);
    return lerp(history, current, cloud.temporal.x);
}
#endif

// The sky behind the clouds, as sky_fs draws it (quench/Sky.hlsli), with the clouds over it and the horizon haze
// after them, so that distant clouds meet the fog. Within a few degrees of the horizon, beyond the clouds of the
// weather map, the sky turns toward the cloud bases' grey as the cover closes (the clouds go on past the map): all of
// it below about 3 degrees, where the map's clouds end, or a low sun's glow would show in the gap under a closed deck
// and, through the sky cube's blur, colour every wet surface.
float3 cloudySkyColor(float3 dir, float4 clouds, bool sunDisc) {
    float3 col = skyRadiance(dir);
    if (sunDisc) col = quenchSunDisc(col, dir);
    col = lerp(col, quenchFrame.overcast.rgb, quenchFrame.overcast.w * saturate(1.5 - abs(dir.y) * 10.0));
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
