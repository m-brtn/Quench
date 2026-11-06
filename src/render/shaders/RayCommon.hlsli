#ifndef QUENCH_RAY_COMMON_HLSLI
#define QUENCH_RAY_COMMON_HLSLI

// Helpers of the renderer's ray-traced and denoising passes: positions and normals from the prepass depth, and
// random numbers per pixel and frame.

#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"

// World position of a pixel center at a depth (reversed: near 1, far 0).
float3 rtWorldAt(float2 pixel, float depth) {
    const float2 uv = pixel * quenchFrame.screen.zw;
    const float4 w = mul(quenchFrame.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return w.xyz / w.w;
}

// The normal of the surface at pixel p from the depth: the smaller difference on each axis keeps edges from bending
// it; turned toward the camera.
float3 rtDepthNormal(Texture2D<float> depthTex, int2 p, float3 P) {
    const int2 last = int2(quenchFrame.screen.xy) - 1;
    const int2 pr = min(p + int2(1, 0), last), pl = max(p - int2(1, 0), int2(0, 0));
    const int2 pd = min(p + int2(0, 1), last), pu = max(p - int2(0, 1), int2(0, 0));
    const float3 R = rtWorldAt(float2(pr) + 0.5, depthTex.Load(int3(pr, 0))) - P;
    const float3 L = P - rtWorldAt(float2(pl) + 0.5, depthTex.Load(int3(pl, 0)));
    const float3 D = rtWorldAt(float2(pd) + 0.5, depthTex.Load(int3(pd, 0))) - P;
    const float3 U = P - rtWorldAt(float2(pu) + 0.5, depthTex.Load(int3(pu, 0)));
    const float3 dx = dot(R, R) < dot(L, L) ? R : L;
    const float3 dy = dot(D, D) < dot(U, U) ? D : U;
    float3 N = normalize(cross(dy, dx));
    if (dot(N, quenchFrame.cameraPos.xyz - P) < 0.0) N = -N;
    return N;
}

// View distance of a reversed depth (the projection is infinite: near / depth).
float rtViewDepth(float depth) { return quenchFrame.cameraFwd.w / max(depth, 1e-8); }

// A random number in [0, 1) per seed (PCG hash).
uint rtHash(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rtRandom(inout uint seed) {
    seed = rtHash(seed);
    return float(seed >> 8) * (1.0 / 16777216.0);
}
uint rtSeed(uint2 pixel, uint frame, uint salt) { return rtHash(pixel.x + rtHash(pixel.y + rtHash(frame ^ salt))); }

// A unit vector perpendicular to n.
float3 rtPerpendicular(float3 n) {
    return abs(n.y) < 0.99 ? normalize(cross(n, float3(0.0, 1.0, 0.0))) : normalize(cross(n, float3(1.0, 0.0, 0.0)));
}

// A cosine-weighted direction around N.
float3 rtCosineDirection(float3 N, float2 u) {
    const float3 T = rtPerpendicular(N);
    const float3 B = cross(N, T);
    const float r = sqrt(u.x);
    const float a = 6.2831853 * u.y;
    return normalize(T * (r * cos(a)) + B * (r * sin(a)) + N * sqrt(max(1.0 - u.x, 0.0)));
}

// A GGX microfacet normal seen from V (visible normals, Heitz 2018) around N for roughness alpha = roughness^2.
float3 rtGgxVisibleNormal(float3 N, float3 V, float alpha, float2 u) {
    const float3 T = rtPerpendicular(N);
    const float3 B = cross(N, T);
    const float3 v = normalize(float3(dot(V, T) * alpha, dot(V, B) * alpha, max(dot(V, N), 1e-4)));
    const float lensq = v.x * v.x + v.y * v.y;
    const float3 t1 = lensq > 0.0 ? float3(-v.y, v.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(v, t1);
    const float r = sqrt(u.x);
    const float phi = 6.2831853 * u.y;
    const float p1 = r * cos(phi);
    const float s = 0.5 * (1.0 + v.z);
    const float p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * r * sin(phi);
    const float3 h = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * v;
    const float3 m = normalize(float3(alpha * h.x, alpha * h.y, max(h.z, 1e-6)));
    return normalize(T * m.x + B * m.y + N * m.z);
}

#endif
