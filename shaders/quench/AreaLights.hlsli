#ifndef QUENCH_AREA_LIGHTS_HLSLI
#define QUENCH_AREA_LIGHTS_HLSLI

// Area lights (render::LightType::Rect, Disk, Tube; LocalLight types 3, 4, 5) shaded with linearly transformed
// cosines: a 3 x 3 transform fitted per roughness and view angle turns the GGX lobe into a clamped cosine, whose
// integral over a polygon, an ellipse or a line has a closed form. Ported from selfshadow/ltc_code (commit 31e5e96,
// BSD-2-Clause, Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt; THIRD_PARTY_LICENSES):
// Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading with Linearly Transformed Cosines" (SIGGRAPH
// 2016); Heitz and Hill, "Real-Time Line- and Disk-Light Shading with Linearly Transformed Cosines" (SIGGRAPH 2017
// course); the horizon-clipped sphere of Hill and Heitz, "Real-Time Area Lighting: a Journey from Research to
// Production" (SIGGRAPH 2016 course), here as its analytic fit in place of ltc_code's table.
//
// The table (src/render/LtcTable.inc, written by tools/make_ltc_table.py from ltc_code's fit) is a 64 x 64 x 2
// Texture2DArray<float4>, x the perceptual roughness and y sqrt(1 - N.V): layer 0 the inverse transform, layer 1 the
// lobe's magnitude and Fresnel part. It is read with Load and filtered here (32-bit floats: half floats lose the
// transforms of smooth surfaces at grazing angles, and not every device filters 32-bit floats).

#include "quench/Brdf.hlsli"
#include "quench/Common.hlsli"
#include "quench/SceneUniforms.h"

// The area lights' terms of one surface point.
struct QuenchLtc {
    float3x3 diffuse;  // rows: a frame around the shading normal
    float3x3 specular; // rows: T1 toward the viewer, T2 and the normal of the specular lobe
    float4 minv;       // the inverse transform M^-1 = [[x 0 z] [0 1 0] [y 0 w]] in the specular frame
    float3 kd;         // the diffuse albedo times its weight
    float3 ks;         // the specular color: F0 times the lobe's magnitude plus (1 - F0) times its Fresnel part
};

QuenchLtc quenchNoLtc() {
    QuenchLtc l;
    l.diffuse = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
    l.specular = l.diffuse;
    l.minv = float4(1.0, 0.0, 0.0, 1.0);
    l.kd = float3(0.0, 0.0, 0.0);
    l.ks = float3(0.0, 0.0, 0.0);
    return l;
}

float3x3 quenchLtcFrame(float3 N, float3 V) {
    const float3 t = V - N * dot(V, N);
    const float t2 = dot(t, t);
    float3 T1;
    if (t2 > 1e-8) {
        T1 = t * rsqrt(t2);
    } else {
        const float3 a = abs(N.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        T1 = normalize(cross(a, N));
    }
    return float3x3(T1, cross(N, T1), N);
}

float4 quenchLtcFetch(Texture2DArray<float4> table, float2 uv, int layer) {
    const float2 p = saturate(uv) * 63.0;
    const int2 i = min(int2(p), int2(62, 62));
    const float2 f = p - float2(i);
    const float4 a = table.Load(int4(i, layer, 0));
    const float4 b = table.Load(int4(i + int2(1, 0), layer, 0));
    const float4 c = table.Load(int4(i + int2(0, 1), layer, 0));
    const float4 d = table.Load(int4(i + int2(1, 1), layer, 0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// The terms of a surface: N its shading normal, Ns the normal of its specular lobe (N, or the bent normal of an
// anisotropic surface), roughness the lobe's perceptual roughness.
QuenchLtc quenchLtcAt(Texture2DArray<float4> table, float3 N, float3 Ns, float3 V, float roughness, float3 albedo,
                      float metallic) {
    QuenchLtc l;
    l.diffuse = quenchLtcFrame(N, V);
    l.specular = quenchLtcFrame(Ns, V);
    const float2 uv = float2(roughness, sqrt(1.0 - saturate(dot(Ns, V))));
    l.minv = quenchLtcFetch(table, uv, 0);
    const float2 amplitude = quenchLtcFetch(table, uv, 1).xy;
    const float3 F0 = lerp(((float3)(0.04)), albedo, metallic);
    l.kd = (1.0 - fresnelSchlick(F0, saturate(dot(N, V)))) * (1.0 - metallic) * albedo;
    l.ks = F0 * amplitude.x + (1.0 - F0) * amplitude.y;
    return l;
}

float3 quenchLtcApply(float4 m, float3 p) { return float3(m.x * p.x + m.z * p.z, p.y, m.y * p.x + m.w * p.z); }

// The integral of a clamped cosine over the spherical polygon edge v1 -> v2 (unit vectors) as a vector: cross(v1, v2)
// times theta / sin(theta) / (2 pi), with ltc_code's rational fit of theta / sin(theta).
float3 quenchLtcEdge(float3 v1, float3 v2) {
    const float x = dot(v1, v2);
    const float y = abs(x);
    const float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    const float b = 3.4175940 + (4.1616724 + y) * y;
    const float v = a / b;
    const float k = x > 0.0 ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * k;
}

// The form factor of a sphere cap with the vector form factor f, clipped by the horizon (Hill and Heitz's fit).
float quenchLtcSphere(float3 f, bool behind) {
    const float l = length(f);
    const float z = behind ? -f.z : f.z;
    return max((l * l + z) / (l + 1.0), 0.0);
}

// A quad seen from the origin (corners in the cosine's space), without clipping: its form factor.
float quenchLtcQuad(float3 p0, float3 p1, float3 p2, float3 p3, bool behind) {
    p0 = normalize(p0);
    p1 = normalize(p1);
    p2 = normalize(p2);
    p3 = normalize(p3);
    const float3 f = quenchLtcEdge(p0, p1) + quenchLtcEdge(p1, p2) + quenchLtcEdge(p2, p3) + quenchLtcEdge(p3, p0);
    return quenchLtcSphere(f, behind);
}

// The roots of c.w x^3 + c.z x^2 + c.y x + c.x (three real ones), in the order ltc_code's disk needs: Blinn's
// method as extended by Peters, "How to solve a cubic equation, revisited".
float3 quenchLtcSolveCubic(float4 c) {
    c.xyz /= c.w;
    c.yz /= 3.0;
    const float A = c.w;
    const float B = c.z;
    const float C = c.y;
    const float D = c.x;
    const float3 delta = float3(-c.z * c.z + c.y, -c.y * c.z + c.x, dot(float2(c.z, -c.y), c.xy));
    const float discriminant = max(dot(float2(4.0 * delta.x, -delta.y), delta.zy), 0.0);
    float2 xlc, xsc;
    {
        const float Ca = delta.x;
        const float Da = -2.0 * B * delta.x + delta.y;
        const float theta = atan2(sqrt(discriminant), -Da) / 3.0;
        const float x1a = 2.0 * sqrt(max(-Ca, 0.0)) * cos(theta);
        const float x3a = 2.0 * sqrt(max(-Ca, 0.0)) * cos(theta + (2.0 / 3.0) * QUENCH_PI);
        const float xl = x1a + x3a > 2.0 * B ? x1a : x3a;
        xlc = float2(xl - B, A);
    }
    {
        const float Cd = delta.z;
        const float Dd = -D * delta.y + 2.0 * C * delta.z;
        const float theta = atan2(D * sqrt(discriminant), -Dd) / 3.0;
        const float x1d = 2.0 * sqrt(max(-Cd, 0.0)) * cos(theta);
        const float x3d = 2.0 * sqrt(max(-Cd, 0.0)) * cos(theta + (2.0 / 3.0) * QUENCH_PI);
        const float xs = x1d + x3d < 2.0 * C ? x1d : x3d;
        xsc = float2(-D, xs + C);
    }
    const float E = xlc.y * xsc.y;
    const float F = -xlc.x * xsc.y - xlc.y * xsc.x;
    const float G = xlc.x * xsc.x;
    const float2 xmc = float2(C * F - B * G, -B * F + C * E);
    float3 root = float3(xsc.x / xsc.y, xmc.x / xmc.y, xlc.x / xlc.y);
    if (root.x < root.y && root.x < root.z) root = root.yxz;
    else if (root.z < root.x && root.z < root.y) root = root.xzy;
    return root;
}

// An ellipse with center C and semi-axes V1, V2 seen from the origin (in the cosine's space): its form factor.
float quenchLtcDisk(float3 C, float3 V1, float3 V2) {
    float a, b;
    const float d11 = dot(V1, V1), d22 = dot(V2, V2), d12 = dot(V1, V2);
    if (abs(d12) * rsqrt(max(d11 * d22, 1e-20)) > 1e-4) {
        const float tr = d11 + d22;
        const float det = sqrt(max(d11 * d22 - d12 * d12, 0.0));
        const float u = 0.5 * sqrt(max(tr - 2.0 * det, 0.0));
        const float v = 0.5 * sqrt(tr + 2.0 * det);
        const float eMax = (u + v) * (u + v);
        const float eMin = max((u - v) * (u - v), 1e-12);
        float3 W1, W2;
        if (d11 > d22) {
            W1 = d12 * V1 + (eMax - d11) * V2;
            W2 = d12 * V1 + (eMin - d11) * V2;
        } else {
            W1 = d12 * V2 + (eMax - d22) * V1;
            W2 = d12 * V2 + (eMin - d22) * V1;
        }
        a = 1.0 / eMax;
        b = 1.0 / eMin;
        V1 = normalize(W1);
        V2 = normalize(W2);
    } else {
        a = 1.0 / max(d11, 1e-12);
        b = 1.0 / max(d22, 1e-12);
        V1 *= sqrt(a);
        V2 *= sqrt(b);
    }
    float3 V3 = cross(V1, V2);
    if (dot(C, V3) < 0.0) V3 = -V3;
    const float L = dot(V3, C);
    if (L < 1e-6) return 0.0; // seen edge-on
    const float x0 = dot(V1, C) / L, y0 = dot(V2, C) / L;
    a *= L * L;
    b *= L * L;
    const float c0 = a * b;
    const float c1 = a * b * (1.0 + x0 * x0 + y0 * y0) - a - b;
    const float c2 = 1.0 - a * (1.0 + x0 * x0) - b * (1.0 + y0 * y0);
    const float3 roots = quenchLtcSolveCubic(float4(c0, c1, c2, 1.0));
    const float e1 = roots.x, e2 = roots.y, e3 = roots.z;
    float3 dir = float3(a * x0 / (a - e2), b * y0 / (b - e2), 1.0);
    dir = normalize(V1 * dir.x + V2 * dir.y + V3 * dir.z);
    const float L1 = sqrt(max(-e2 / e3, 0.0));
    const float L2 = sqrt(max(-e2 / e1, 0.0));
    const float formFactor = L1 * L2 * rsqrt((1.0 + L1 * L1) * (1.0 + L2 * L2));
    return quenchLtcSphere(dir * formFactor, false);
}

float quenchLtcFpo(float d, float l) { return l / (d * (d * d + l * l)) + atan(l / d) / (d * d); }
float quenchLtcFwt(float d, float l) { return l * l / (d * (d * d + l * l)); }

// The clamped cosine integrated along the segment p1 -> p2 (seen from the origin), per unit of half width (a tube of
// radius R: R times this).
float quenchLtcLineDiffuse(float3 p1, float3 p2) {
    if (p1.z <= 0.0 && p2.z <= 0.0) return 0.0;
    const float3 wt = normalize(p2 - p1);
    if (p1.z < 0.0) p1 = (p1 * p2.z - p2 * p1.z) / (p2.z - p1.z);
    if (p2.z < 0.0) p2 = (-p1 * p2.z + p2 * p1.z) / (-p2.z + p1.z);
    const float l1 = dot(p1, wt), l2 = dot(p2, wt);
    const float3 po = p1 - l1 * wt;
    const float d = max(length(po), 1e-4);
    return ((quenchLtcFpo(d, l2) - quenchLtcFpo(d, l1)) * po.z + (quenchLtcFwt(d, l2) - quenchLtcFwt(d, l1)) * wt.z) /
           QUENCH_PI;
}

// The same through the inverse transform m, with the width the transform gives the line.
float quenchLtcLine(float3 p1, float3 p2, float4 m) {
    const float3 o = cross(p1, p2);
    const float o2 = dot(o, o);
    if (o2 < 1e-12) return 0.0; // the origin lies on the line
    const float3 ortho = o * rsqrt(o2);
    const float det = m.x * m.w - m.y * m.z;
    // inverse(transpose(M^-1)) * ortho
    const float3 w = float3((m.w * ortho.x - m.y * ortho.z) / det, ortho.y, (m.x * ortho.z - m.z * ortho.x) / det);
    return quenchLtcLineDiffuse(quenchLtcApply(m, p1), quenchLtcApply(m, p2)) / max(length(w), 1e-6);
}

// The range window of an area light at P: 1 near its shape, falling smoothly to 0 at LocalLight::positionRange.w
// from it.
float quenchAreaWindow(LocalLight light, float3 P) {
    const int type = int(light.color.w);
    const float3 q = P - light.positionRange.xyz;
    const float3 t = light.shape.xyz;
    float3 d;
    if (type == QUENCH_LIGHT_TUBE) {
        const float3 s = q - t * clamp(dot(q, t), -light.cone.x, light.cone.x);
        const float len = length(s);
        d = s * (max(len - light.cone.z, 0.0) / max(len, 1e-6));
    } else {
        const float3 b = cross(light.direction.xyz, t);
        float2 p = float2(dot(q, t), dot(q, b));
        if (type == QUENCH_LIGHT_RECT) {
            p = clamp(p, -light.cone.xz, light.cone.xz);
        } else {
            const float r = length(p);
            if (r > light.cone.x) p *= light.cone.x / r;
        }
        d = q - t * p.x - b * p.y;
    }
    const float r = dot(d, d) / max(light.positionRange.w * light.positionRange.w, 1e-6);
    const float window = saturate(1.0 - r * r);
    return window * window;
}

// The light an area light sends from the surface at P toward the viewer, before its window and shadow.
float3 quenchAreaLight(LocalLight light, float3 P, QuenchLtc ltc) {
    const int type = int(light.color.w);
    const float3 c = light.positionRange.xyz - P;
    const float3 t = light.shape.xyz;
    float diffuse = 0.0, specular = 0.0;
    if (type == QUENCH_LIGHT_TUBE) {
        const float3 h = t * light.cone.x;
        const float R = light.cone.z;
        diffuse = R * quenchLtcLineDiffuse(mul(ltc.diffuse, c - h), mul(ltc.diffuse, c + h));
        specular = R * quenchLtcLine(mul(ltc.specular, c - h), mul(ltc.specular, c + h), ltc.minv);
        diffuse = min(diffuse, 1.0);
        specular = min(specular, 1.0);
    } else {
        const float3 n = light.direction.xyz;
        const bool behind = dot(c, n) > 0.0; // P behind the emitting face
        if (behind && light.direction.w < 0.5) return float3(0.0, 0.0, 0.0);
        const float3 b = cross(n, t);
        if (type == QUENCH_LIGHT_RECT) {
            const float3 x = t * light.cone.x, y = b * light.cone.z;
            const float3 p0 = c - x - y, p1 = c - x + y, p2 = c + x + y, p3 = c + x - y;
            diffuse = quenchLtcQuad(mul(ltc.diffuse, p0), mul(ltc.diffuse, p1), mul(ltc.diffuse, p2),
                                    mul(ltc.diffuse, p3), behind);
            specular = quenchLtcQuad(
                quenchLtcApply(ltc.minv, mul(ltc.specular, p0)), quenchLtcApply(ltc.minv, mul(ltc.specular, p1)),
                quenchLtcApply(ltc.minv, mul(ltc.specular, p2)), quenchLtcApply(ltc.minv, mul(ltc.specular, p3)),
                behind);
        } else {
            const float3 x = t * light.cone.x, y = b * light.cone.x;
            diffuse = quenchLtcDisk(mul(ltc.diffuse, c), mul(ltc.diffuse, x), mul(ltc.diffuse, y));
            specular = quenchLtcDisk(quenchLtcApply(ltc.minv, mul(ltc.specular, c)),
                                     quenchLtcApply(ltc.minv, mul(ltc.specular, x)),
                                     quenchLtcApply(ltc.minv, mul(ltc.specular, y)));
        }
    }
    return (ltc.kd * diffuse + ltc.ks * specular) * light.color.rgb;
}

#endif
