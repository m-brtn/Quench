#ifndef QUENCH_SHADOWS_HLSLI
#define QUENCH_SHADOWS_HLSLI

// The sun's cascaded shadow maps: one layer of a Texture2DArray per cascade, standard depth (0 near the light). From
// TrailForge's shadowFactor, without its far terrain layer. Each tap gathers the 2x2 footprint and compares in the
// shader with bilinear weights (the result of a LessOrEqual comparison sampler), so the sampler is a plain one:
// MoltenVK cannot bind comparison samplers through SDL GPU.
//
// Translucent casters (render::ShadowMode::Translucent) have a transmittance map per cascade beside the depth map
// (half its size, Rgba16Float): rgb is the product of the light the casters let through, a is 1 - the depth of the
// caster nearest to the light, so that a receiver in front of the casters stays untinted.

#include "quench/Frame.hlsli"

// 1 = lit: z <= the depth in the map, filtered over the four nearest texels.
float quenchShadowTap(Texture2DArray<float> sm, SamplerState s, float2 uv, float layer, float z) {
    const float size = 1.0 / quenchFrame.shadowParams.y;
    const float2 t = uv * size - 0.5;
    const float2 f = frac(t);
    // The shared corner of the footprint, so that Gather picks the same four texels as the weights.
    const float4 d = sm.Gather(s, float3((floor(t) + 1.0) / size, layer));
    const float4 lit = step(z, d); // x (0,1), y (1,1), z (1,0), w (0,0)
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// Where P lies in the cascades. P and N in world space.
struct QuenchShadowCoord {
    float2 uv;
    float z;     // depth in the cascade (0 at the light)
    float layer; // the cascade
    float fade;  // 0..1: the shadow fades to lit over the last 15 % of the last cascade
};

// False where P is outside every cascade (lit).
bool quenchShadowCoord(float3 P, float3 N, out QuenchShadowCoord sc) {
    sc.uv = float2(0.0, 0.0);
    sc.z = 0.0;
    sc.layer = 0.0;
    sc.fade = 1.0;
    const int cascades = int(quenchFrame.shadowParams.x);
    if (cascades <= 0) return false;
    const float viewDepth = quenchViewDepth(P);
    int c = 0;
    for (; c < cascades; ++c)
        if (viewDepth < quenchFrame.cascadeSplits[c]) break;
    if (c >= cascades) return false;
    // Normal offset scales with the world size of one shadow texel in this cascade.
    const float3 Pn = P + N * quenchFrame.cascadeTexel[c] * quenchFrame.shadowParams.w;
    const float4 p = mul(quenchFrame.shadowViewProj[c], float4(Pn, 1.0));
    sc.uv = p.xy * 0.5 + 0.5;
    sc.uv.y = 1.0 - sc.uv.y;
    sc.z = p.z;
    if (any(sc.uv < 0.0) || any(sc.uv > 1.0) || sc.z > 1.0) return false;
    sc.layer = float(c);
    const float end = quenchFrame.cascadeSplits[cascades - 1];
    sc.fade = saturate((viewDepth - end * 0.85) / (end * 0.15));
    return true;
}

// 1 = lit. P and N in world space; maxFilter caps FrameUniforms::shadowParams.z (0: one tap, 1: four, 2: twelve).
float shadowFactor(Texture2DArray<float> sm, SamplerState s, float3 P, float3 N, int maxFilter = 2) {
    QuenchShadowCoord sc;
    if (!quenchShadowCoord(P, N, sc)) return 1.0;
    const float2 uv = sc.uv;
    const float z = sc.z;
    const float layer = sc.layer;
    const float texel = quenchFrame.shadowParams.y;
    const int filterMode = min(int(quenchFrame.shadowParams.z), maxFilter);
    float lit = 0.0;
    if (filterMode == 0) {
        lit = quenchShadowTap(sm, s, uv, layer, z);
    } else if (filterMode == 1) {
        const float2 o = ((float2)(texel)) * 1.2;
        lit = quenchShadowTap(sm, s, uv + float2(-o.x, -o.y), layer, z) +
              quenchShadowTap(sm, s, uv + float2(o.x, -o.y), layer, z) +
              quenchShadowTap(sm, s, uv + float2(-o.x, o.y), layer, z) +
              quenchShadowTap(sm, s, uv + float2(o.x, o.y), layer, z);
        lit *= 0.25;
    } else {
        const float2 taps[12] = {float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457),
                                 float2(-0.203, 0.621),  float2(0.962, -0.195),  float2(0.473, -0.480),
                                 float2(0.519, 0.767),   float2(0.185, -0.893),  float2(0.507, 0.064),
                                 float2(0.896, 0.412),   float2(-0.322, -0.933), float2(-0.792, -0.598)};
        for (int i = 0; i < 12; ++i) lit += quenchShadowTap(sm, s, uv + taps[i] * texel * 2.2, layer, z);
        lit /= 12.0;
    }
    return lerp(lit, 1.0, sc.fade);
}

// The local lights' shadow maps (render::RenderSettings::localShadowMapSize, where their shadows are not ray traced):
// one layer of a Texture2DArray per map, standard depth of a perspective projection from the light's position (0 at
// the near plane LocalLight::shape.z, 1 at its range). A spot light has one map along its direction, a point light
// six, one per face of a cube around it (+x, -x, +y, -y, +z, -z). The renderer projects with the same up vectors.
float quenchLocalShadowTap(Texture2DArray<float> maps, SamplerState s, float2 uv, float layer, float z) {
    const float size = quenchFrame.localShadows.x;
    const float2 t = uv * size - 0.5;
    const float2 f = frac(t);
    const float4 d = maps.Gather(s, float3((floor(t) + 1.0) / size, layer));
    const float4 lit = step(z, d); // x (0,1), y (1,1), z (1,0), w (0,0)
    return lerp(lerp(lit.w, lit.z, f.x), lerp(lit.x, lit.y, f.x), f.y);
}

// 1 = lit: four bilinear taps two texels apart (4 x 4 texels). P and N in world space.
float quenchLocalShadow(Texture2DArray<float> maps, SamplerState s, LocalLight light, float3 P, float3 N) {
    const bool cube = light.shape.y <= 0.0;
    const float tanHalf = cube ? quenchFrame.localShadows.w : light.shape.y;
    // The normal offset scales with the world size of a texel at P's depth in its map.
    const float3 toP = P - light.positionRange.xyz;
    if (dot(toP, toP) >= light.positionRange.w * light.positionRange.w) return 1.0;
    const float3 a = abs(toP);
    const float depth = cube ? max(a.x, max(a.y, a.z)) : dot(toP, light.direction.xyz);
    const float3 v =
        toP + N * (2.0 * max(depth, 0.0) * tanHalf * quenchFrame.localShadows.y * quenchFrame.localShadows.z);
    float3 forward = light.direction.xyz;
    float3 up = abs(forward.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float layer = light.shape.x - 1.0;
    if (cube) {
        const float3 b = abs(v);
        if (b.x >= b.y && b.x >= b.z) {
            forward = float3(v.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
            up = float3(0.0, 1.0, 0.0);
            layer += v.x >= 0.0 ? 0.0 : 1.0;
        } else if (b.y >= b.z) {
            forward = float3(0.0, v.y >= 0.0 ? 1.0 : -1.0, 0.0);
            up = float3(0.0, 0.0, 1.0);
            layer += v.y >= 0.0 ? 2.0 : 3.0;
        } else {
            forward = float3(0.0, 0.0, v.z >= 0.0 ? 1.0 : -1.0);
            up = float3(0.0, 1.0, 0.0);
            layer += v.z >= 0.0 ? 4.0 : 5.0;
        }
    }
    const float3 right = normalize(cross(forward, up));
    const float3 upward = cross(right, forward);
    const float z = dot(v, forward);
    const float zNear = light.shape.z;
    const float zFar = light.positionRange.w;
    if (z <= zNear) return 1.0;
    const float2 ndc = float2(dot(v, right), dot(v, upward)) / (z * tanHalf);
    if (any(abs(ndc) >= 1.0)) return 1.0;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const float ref = zFar * (z - zNear) / (z * (zFar - zNear));
    const float o = quenchFrame.localShadows.y;
    return 0.25 * (quenchLocalShadowTap(maps, s, uv + float2(-o, -o), layer, ref) +
                   quenchLocalShadowTap(maps, s, uv + float2(o, -o), layer, ref) +
                   quenchLocalShadowTap(maps, s, uv + float2(-o, o), layer, ref) +
                   quenchLocalShadowTap(maps, s, uv + float2(o, o), layer, ref));
}

// rgb: the part of the sun's light that reaches P through translucent casters (1: none in front of P). The map is
// read bilinearly (four taps with the 4 and 12 tap filters); P is behind the casters when its depth is more than
// FrameUniforms::transmittanceBias beyond the nearest caster of the four texels around it. A receiver between two
// translucent casters gets the light of both (the map keeps one depth per texel).
float3 shadowTransmittance(Texture2DArray<float4> tm, SamplerState s, float3 P, float3 N) {
    QuenchShadowCoord sc;
    if (!quenchShadowCoord(P, N, sc)) return float3(1.0, 1.0, 1.0);
    const uint cascade = uint(sc.layer);
    if ((uint(quenchFrame.shadowTransmittance.x) & (1u << cascade)) == 0u) return float3(1.0, 1.0, 1.0);
    const float3 at = float3(sc.uv, sc.layer);
    const float4 w = tm.GatherAlpha(s, at);
    const float nearest = 1.0 - max(max(w.x, w.y), max(w.z, w.w));
    const float bias = quenchFrame.transmittanceBias[cascade];
    const float behind = saturate((sc.z - nearest) / bias - 1.0);
    float3 t;
    if (int(quenchFrame.shadowParams.z) == 0) {
        t = tm.SampleLevel(s, at, 0.0).rgb;
    } else {
        const float o = quenchFrame.shadowTransmittance.y * 0.5;
        t = (tm.SampleLevel(s, at + float3(-o, -o, 0.0), 0.0).rgb + tm.SampleLevel(s, at + float3(o, -o, 0.0), 0.0).rgb +
             tm.SampleLevel(s, at + float3(-o, o, 0.0), 0.0).rgb + tm.SampleLevel(s, at + float3(o, o, 0.0), 0.0).rgb) *
            0.25;
    }
    return lerp(float3(1.0, 1.0, 1.0), t, behind * (1.0 - sc.fade));
}

#endif
