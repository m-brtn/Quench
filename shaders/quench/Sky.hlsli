#ifndef QUENCH_SKY_HLSLI
#define QUENCH_SKY_HLSLI

// Sky radiance for a view direction: a gradient between the horizon and the zenith colors with a glow around the
// sun, mixed with single scattering in a flat exponential atmosphere. From TrailForge (skyRadiance, paletteSky,
// atmosphere, hazeColor), with its time-of-day palette replaced by the scene's Environment (FrameUniforms).

#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"

// Single scattering in a flat exponential atmosphere (Hoffman and Preetham 2002): Rayleigh (per wavelength, scale
// height 8 km) and Mie (1.2 km) with Kasten-Young air mass along the view and toward the sun, so the sky turns blue
// overhead, pale at the horizon and red where sunlight crosses more air. Unit sun irradiance.
float quenchAirMass(float y) {
    const float zenithDeg = acos(clamp(y, 0.0, 1.0)) * 57.29578;
    return 1.0 / (max(y, 0.0) + 0.50572 * pow(max(96.07995 - zenithDeg, 1e-3), -1.6364));
}

float3 quenchAtmosphere(float3 dir, float3 sun) {
    const float3 bR = float3(5.8e-3, 13.5e-3, 33.1e-3); // 1/km
    const float3 ozone = float3(0.0098, 0.028, 0.0013); // vertical optical depth of the ozone layer
    const float bM = 3.0e-3, HR = 8.0, HM = 1.2;
    const float mv = quenchAirMass(dir.y), ms = quenchAirMass(sun.y);
    const float3 tauV = bR * HR * mv + bM * 1.1 * HM * mv;
    // Most scattering happens well above the ground, where sunlight has crossed only part of the air column (0.35 of
    // the ground-level air mass); ozone removes the green that the plain model leaves at low sun.
    const float3 sunT = exp(-((bR * HR + bM * 1.1 * HM) * 0.35 + ozone) * ms - ozone * 0.5 * mv);
    const float mu = dot(dir, sun);
    const float pR = 0.0596831 * (1.0 + mu * mu);
    const float g = 0.76;
    const float pM = 0.0795775 * (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5);
    return sunT * (bR * pR + bM * pM) / (bR + bM) * (1.0 - exp(-tauV));
}

// The gradient sky: toward the sun the warm horizon band reaches higher, away from it the sky stays cooler.
float3 quenchGradientSky(float3 dir) {
    const float3 sun = quenchFrame.sunDir.xyz;
    const float hasSun = quenchFrame.sunDir.w;
    const float up = saturate(dir.y);
    const float mu = dot(dir, sun);
    const float toward = saturate(mu * 0.5 + 0.5) * hasSun;
    const float band = lerp(0.55, 0.28, toward * toward);
    float3 col = lerp(quenchFrame.skyHorizon.rgb, quenchFrame.skyZenith.rgb, pow(up, band));
    const float g = 0.76;
    const float hg = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5) / (4.0 * QUENCH_PI);
    col += quenchFrame.sunGlow.rgb * hasSun * (hg * 0.10 + 0.05 * pow(saturate(mu), 6.0) * (1.0 - up));
    return col;
}

// Sky radiance without the sun disc: the sky pass and the environment light of surfaces.
float3 skyRadiance(float3 dir) {
    const float3 sun = quenchFrame.sunDir.xyz;
    const float3 d = float3(dir.x, max(dir.y, 0.0), dir.z);
    const float3 gradient = quenchGradientSky(d);
    float3 col = gradient;
    const float atmosphere = quenchFrame.skyGround.w * quenchFrame.sunDir.w;
    if (atmosphere > 0.0) {
        // The scattering model scaled to the gradient's brightness at the zenith; the gradient carries twilight.
        const float3 zen = quenchAtmosphere(float3(0, 1, 0), sun);
        const float scale = luminance(quenchFrame.skyZenith.rgb) / max(luminance(zen), 1e-6);
        const float day = smoothstep(-0.06, 0.05, sun.y);
        col = lerp(gradient, quenchAtmosphere(d, sun) * scale * 0.85 + gradient * 0.25, day * atmosphere);
    }
    // Below the horizon: the ground color, through haze near the horizon.
    const float3 below = lerp(quenchFrame.fogColor.rgb * 0.85, quenchFrame.skyGround.rgb, saturate(-dir.y * 4.0));
    return dir.y < 0.0 ? lerp(col, below, saturate(-dir.y * 12.0)) : col;
}

// Haze color along a view ray: the fog color, warmed toward the sun; with the scattering sky also the sky color just
// above the horizon in that direction (aerial perspective: distant land fades into the sky behind it).
float3 hazeColor(float3 rd) {
    const float hasSun = quenchFrame.sunDir.w;
    const float sunAmt = pow(saturate(dot(rd, quenchFrame.sunDir.xyz)), 8.0) * hasSun;
    const float3 base = quenchFrame.fogColor.rgb + quenchFrame.sunGlow.rgb * 0.22 * sunAmt * quenchFrame.fogParams.w;
    const float atmosphere = quenchFrame.skyGround.w * hasSun;
    if (atmosphere <= 0.0) return base;
    const float3 sky = skyRadiance(normalize(float3(rd.x, 0.06, rd.z)));
    return lerp(base, sky * (luminance(base) / max(luminance(sky), 1e-5)) * 0.5 + sky * 0.5, 0.6 * atmosphere);
}

// The disc's radiance is at least this many times the sky radiance at the sun's centre: with a low sun the scattering
// sky's Mie peak is brighter than the palette's disc, which would otherwise show as a darker spot in the glow.
static const float QUENCH_SUN_OVER_SKY = 4.0;

// The sun disc added to the sky radiance (the sky is light scattered in front of the sun): a slightly brighter core, a
// soft edge, faded out with the palette's disc radiance.
float3 quenchSunDisc(float3 col, float3 dir) {
    if (quenchFrame.sunDir.w <= 0.0) return col;
    const float3 sun = quenchFrame.sunDir.xyz;
    const float mu = dot(dir, sun);
    const float cosR = quenchFrame.sunDisc.w;
    const float edge = (1.0 - cosR) * 0.04;
    const float disc = smoothstep(cosR - edge, cosR + edge, mu);
    const float3 base = quenchFrame.sunDisc.rgb;
    const float l = luminance(base);
    if (disc <= 0.0 || l <= 0.0) return col;
    const float3 radiance = base * max(1.0, QUENCH_SUN_OVER_SKY * luminance(skyRadiance(sun)) / l);
    const float core = saturate((mu - cosR) / max(1.0 - cosR, 1e-6));
    return col + radiance * (0.85 + 0.35 * sqrt(core)) * disc * saturate(l * 4.0);
}

#endif
