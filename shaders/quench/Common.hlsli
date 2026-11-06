#ifndef QUENCH_COMMON_HLSLI
#define QUENCH_COMMON_HLSLI

// Small helpers shared by the engine's shaders and the games'. luminance, hash12 and valueNoise were identical in
// TrailForge and Ashfall; hash22, ign and phaseHG are TrailForge's versions (docs/migration.md).

static const float QUENCH_PI = 3.14159265;

// Rec. 709 luminance of a linear color.
float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Hash without sine (Dave Hoskins): 0..1 from a 2D position.
float hash12(float2 p) {
    float3 p3 = frac(((float3)(p.xyx)) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 hash22(float2 p) {
    float3 p3 = frac(((float3)(p.xyx)) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

// Smooth value noise, 0..1.
float valueNoise(float2 p) {
    const float2 i = floor(p);
    const float2 u = frac(p);
    const float2 w = u * u * (3.0 - 2.0 * u);
    return lerp(lerp(hash12(i), hash12(i + float2(1, 0)), w.x), lerp(hash12(i + float2(0, 1)), hash12(i + float2(1, 1)), w.x), w.y);
}

// Interleaved gradient noise (Jimenez) of a pixel position.
float ign(float2 pixel) { return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))); }

// Ordered dither threshold of a pixel position: the 4 x 4 Bayer matrix, (n + 0.5) / 16 for n = 0..15.
float bayer4(float2 pixel) {
    const uint2 p = uint2(pixel) & 3u;
    const uint d = p.x ^ p.y;
    const uint n = ((d & 1u) << 3) | ((p.y & 1u) << 2) | (d & 2u) | ((p.y & 2u) >> 1);
    return (float(n) + 0.5) / 16.0;
}

// Henyey-Greenstein phase function times 4 pi (1 = isotropic).
float phaseHG(float mu, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5);
}

// The sun's light a translucent surface lets through: 1 - opacity, tinted toward its hue (its albedo over its
// brightest channel) as far as it is opaque. A grey surface of opacity a passes 1 - a in every channel.
float3 quenchTransmittanceOf(float3 albedo, float opacity) {
    const float a = saturate(opacity);
    const float peak = max(albedo.r, max(albedo.g, albedo.b));
    const float3 hue = peak > 1e-4 ? saturate(albedo / peak) : float3(1.0, 1.0, 1.0);
    return (1.0 - a) * lerp(float3(1.0, 1.0, 1.0), hue, a);
}

// The sRGB transfer function and its inverse.
float3 linearToSrgb(float3 c) {
    c = max(c, 0.0);
    return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308));
}

float3 srgbToLinear(float3 c) {
    c = max(c, 0.0);
    return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));
}

#endif
