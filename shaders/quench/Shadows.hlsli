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

#include "quench/Common.hlsli"
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

// Area lights (rect, disk, tube) with shadow maps: a cube of maps around the light's center in the light's own frame
// (x its tangent, y its normal, for a tube a direction across its axis, z their cross product) with a point light's
// faces and up vectors in that frame, five for a one-sided rect or disk (none looks behind it: +x, -x, +y, +z, -z).
// LocalLight::cone.w is the first map's layer + 1; the far plane is the range plus the shape's radius, the near plane
// QUENCH_AREA_SHADOW_NEAR of it. The shadow is soft by the shape's size (percentage-closer soft shadows, Fernando
// 2005, with the shape's own points as the kernel): a search over the region where blockers of the shape's light could
// be finds their mean depth d, then taps toward points of the shape (uniform over its area, a tube's half that faces
// P) seen through a blocker at d say which of them P sees. Each point counts with its diffuse light at P (the cosines
// at both ends over the squared distance; the ratio of Heitz et al. 2018), so that the far ends of a long light, which
// add little to P's light, add little to its shadow. Exact for one flat blocker parallel to the shape; the maps see the
// casters from the center only. Too costly per surface fragment: the renderer reads it per 2 x 2 pixels in a
// screen-space pass (docs/render.md, "Local light shadow maps").
#define QUENCH_AREA_SHADOW_SEARCH 8
#define QUENCH_AREA_SHADOW_TAPS 8

struct QuenchAreaMaps {
    int type;
    float first; // the first map's layer
    bool oneSided;
    float tanHalf;
    float a, b;  // a map's depth of a distance z along its axis: a - b / z
    float3 size; // rect: half sizes (x, z); disk: radius (x); tube: half length (x), radius (z)
    float2 n0;   // tube: the direction toward P across the axis (y, z)
    float3 N;    // the receiver's normal in the light's frame
    float D;     // its plane: dot(x, N) = D
    float dist;  // its distance from the light's center
};

// A point of the shape for u in [0, 1)^2, uniform over its area, from its center in the light's frame; a tube's on the
// half that faces P.
float3 quenchAreaShadowPoint(QuenchAreaMaps m, float2 u) {
    if (m.type == QUENCH_LIGHT_RECT) return float3((u.x * 2.0 - 1.0) * m.size.x, 0.0, (u.y * 2.0 - 1.0) * m.size.z);
    float s, c;
    if (m.type == QUENCH_LIGHT_DISK) {
        sincos(6.2831853 * u.y, s, c);
        const float r = sqrt(u.x) * m.size.x;
        return float3(r * c, 0.0, r * s);
    }
    sincos(3.14159265 * (u.y - 0.5), s, c);
    return float3((u.x * 2.0 - 1.0) * m.size.x, (m.n0 * c + float2(-m.n0.y, m.n0.x) * s) * m.size.z);
}

// A point's distance from the shape's plane (rect, disk) or axis (tube), in the light's frame: the depth by which the
// shape's points spread through a blocker (exact for a blocker parallel to the shape).
float quenchAreaShadowDepth(int type, float3 p) { return type == QUENCH_LIGHT_TUBE ? length(p.yz) : abs(p.y); }

// The diffuse light the shape's point p sends to q (normal N) up to a constant: the cosines at both ends over the
// squared distance (a tube's point by the share of its width seen from q); 0 under q's horizon.
float quenchAreaShadowWeight(int type, float3 p, float3 q, float3 N) {
    const float3 l = p - q;
    const float d2 = max(dot(l, l), 1e-8);
    return max(dot(l, N), 0.0) * quenchAreaShadowDepth(type, l) / (d2 * d2);
}

// Where direction v (the light's frame) meets the receiver's plane, in the map v falls in: uv and layer, and the
// map's depth there (P's own distance where v runs along the plane or away from it); z: v's component along the map's
// axis. False behind a one-sided light (no map there). The faces' right and up axes as quenchLocalShadow's.
bool quenchAreaMapAt(QuenchAreaMaps m, float3 v, out float3 at, out float ref, out float z) {
    const float3 b = abs(v);
    float2 st;
    float face;
    if (b.x >= b.y && b.x >= b.z) {
        z = b.x;
        face = v.x >= 0.0 ? 0.0 : 1.0;
        st = float2(v.x >= 0.0 ? v.z : -v.z, v.y);
    } else if (b.y >= b.z) {
        z = b.y;
        face = v.y >= 0.0 ? 2.0 : 3.0;
        st = float2(v.y >= 0.0 ? v.x : -v.x, v.z);
    } else {
        z = b.z;
        face = v.z >= 0.0 ? 4.0 : 5.0;
        st = float2(v.z >= 0.0 ? -v.x : v.x, v.y);
    }
    const float2 ndc = st / (z * m.tanHalf);
    at = float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, m.first + face - (m.oneSided && face > 3.0 ? 1.0 : 0.0));
    float t = m.dist * rsqrt(dot(v, v));
    const float vn = dot(v, m.N);
    if (vn * m.D > 0.0) t = clamp(m.D / vn, 0.5 * t, 2.0 * t);
    ref = min(m.a - m.b / (t * z), 1.0);
    return !(m.oneSided && face == 3.0);
}

// 1 = lit. P and N in world space; pixel turns the taps per pixel (and per frame under TAA).
float quenchAreaShadow(Texture2DArray<float> maps, SamplerState s, LocalLight light, float3 P, float3 N, float2 pixel) {
    if (light.cone.w <= 0.0) return 1.0;
    QuenchAreaMaps m;
    m.type = int(light.color.w);
    const float radius = m.type == QUENCH_LIGHT_DISK ? light.cone.x : length(light.cone.xz);
    const float zFar = light.positionRange.w + radius;
    const float3 toP = P - light.positionRange.xyz;
    if (dot(toP, toP) >= zFar * zFar) return 1.0;
    const float zNear = zFar * QUENCH_AREA_SHADOW_NEAR;
    m.first = light.cone.w - 1.0;
    m.oneSided = m.type != QUENCH_LIGHT_TUBE && light.direction.w < 0.5;
    m.tanHalf = quenchFrame.localShadows.w;
    m.a = zFar / (zFar - zNear);
    m.b = zNear * m.a;
    m.size = light.cone.xyz;
    const float3 X = light.shape.xyz, Y = light.direction.xyz, Z = cross(X, Y);
    float3 q = float3(dot(toP, X), dot(toP, Y), dot(toP, Z));
    if (m.oneSided && q.y <= 0.0) return 1.0; // behind the light: it does not shine there
    m.N = float3(dot(N, X), dot(N, Y), dot(N, Z));
    // The normal offset scales with the world size of a texel at P's depth in its map.
    const float3 aq = abs(q);
    const float texel = 2.0 * m.tanHalf * quenchFrame.localShadows.y; // per unit of depth
    q += m.N * (max(aq.x, max(aq.y, aq.z)) * texel * quenchFrame.localShadows.z);
    m.dist = length(q);
    m.D = dot(q, m.N);
    const float l2 = dot(q.yz, q.yz);
    m.n0 = l2 > 1e-10 ? q.yz * rsqrt(l2) : float2(1.0, 0.0);
    const float frame = quenchFrame.options.z > 0.5 ? float(uint(quenchFrame.options.y) & 63u) : 0.0;
    const float2 shift = float2(ign(pixel + frame * 5.588238), ign(pixel.yx + 7.0 + frame * 3.17));
    const float2 r2 = float2(0.7548777, 0.5698403); // the R2 sequence (Roberts 2018)

    // A tap toward the shape's point p seen through a blocker at depth d (quenchAreaShadowDepth) from P at depth zR
    // runs along q + p * (zR / d - 1). The search: blockers whose shadow of some point of the shape could reach P,
    // from the near plane on, seen within about 37 degrees of P's direction; its taps spread from P's own direction
    // outward (quadratically), so that blockers close to P, whose region is small, are found as well.
    const float zR = quenchAreaShadowDepth(m.type, q);
    const float reach = min(zR / zNear - 1.0, 0.75 * m.dist / max(radius, 1e-4));
    float blockers = 0.0, weights = 0.0, depths = 0.0;
    int taps = 0;
    for (int i = 0; i < QUENCH_AREA_SHADOW_SEARCH; ++i) {
        const float2 u = frac(shift + float(i) * r2);
        const float3 p = quenchAreaShadowPoint(m, u);
        const float k = (float(i) + shift.y) / float(QUENCH_AREA_SHADOW_SEARCH);
        const float3 v = q + p * (reach * k * k);
        float3 at;
        float ref, z;
        if (!quenchAreaMapAt(m, v, at, ref, z)) continue;
        const float4 d = maps.Gather(s, at);
        const float4 hit = 1.0 - step(ref, d);
        const float found = dot(hit, float4(1.0, 1.0, 1.0, 1.0));
        // A texel's depth along the map's axis, then the depth of the blocker it holds in direction v.
        const float4 along = m.b / (m.a - d);
        const float w = quenchAreaShadowWeight(m.type, p, q, m.N) + 1e-12;
        blockers += found;
        taps += 4;
        weights += w * found;
        depths += w * dot(hit, along) * quenchAreaShadowDepth(m.type, v) / z;
    }
    if (blockers == 0.0) return 1.0;
    if (blockers == float(taps)) return 0.0;
    // The taps: the shape's points seen through a blocker at the mean depth, at least a texel apart at P.
    const float least = texel * m.dist / max(radius, 1e-4);
    const float spread = clamp(zR * weights / max(depths, 1e-12) - 1.0, least, max(reach, least));
    float lit = 0.0, total = 0.0;
    for (int j = 0; j < QUENCH_AREA_SHADOW_TAPS; ++j) {
        const float3 p = quenchAreaShadowPoint(m, frac(shift.yx + (float(j) + 0.5) * r2));
        const float w = quenchAreaShadowWeight(m.type, p, q, m.N);
        if (w <= 0.0) continue; // under P's horizon: no light from there
        float3 at;
        float ref, z;
        total += w;
        if (quenchAreaMapAt(m, q + p * spread, at, ref, z))
            lit += w * quenchLocalShadowTap(maps, s, at.xy, at.z, ref);
        else
            lit += w;
    }
    return total > 0.0 ? lit / total : 1.0;
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
