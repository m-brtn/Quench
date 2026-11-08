#ifndef QUENCH_CLOUDS_HLSLI
#define QUENCH_CLOUDS_HLSLI

// What surfaces and a game's passes see of the scene's clouds (docs/weather.md, "Light under the clouds"): the clouds'
// shadow on the sun's light and the sky under them. Without a weather or with clouds off both are the plain ones
// (FrameUniforms::cloudShadow.w and ::overcast.w are 0).

#include "quench/Frame.hlsli"
#include "quench/Sky.hlsli"

// The share of the sun's light the clouds let through to a world point (1 without cloud shadows). The cloud shadow
// map (the first region of the weather maps, surface sampler 14) holds the transmittance along the sun's direction of
// the clouds above a plane (render::RenderSettings::cloudShadowHeight); a point reads it where its line toward the
// sun meets that plane (here: y = 0, the map's corner in FrameUniforms::cloudShadow moved to match). Over the outer
// twentieth of the map's width it fades to `outside`: past the map there are only its edge texels drawn out, which
// show as stripes along its axes in the distance (the shafts of rain and snow reach farther than the map).
float quenchCloudShadow(Texture2D<float4> maps, SamplerState s, float3 P, float outside) {
    if (quenchFrame.cloudShadow.w <= 0.0) return 1.0;
    const float3 sun = quenchFrame.sunDir.xyz;
    const float2 ground = P.xz - sun.xz * (P.y / max(sun.y, 0.05));
    const float2 uv = (ground - quenchFrame.cloudShadow.xy) * quenchFrame.cloudShadow.z;
    const float2 off = abs(uv - 0.5) * 2.0;
    const float beyond = smoothstep(0.9, 1.0, max(off.x, off.y));
    [branch] if (beyond >= 1.0) return outside;
    // Clamped to the region's edge texels, as a texture of its own would be.
    const float edge = 0.5 / float(QUENCH_RAIN_MAP_ROW);
    const float v = clamp(uv.y, edge, 1.0 - edge) * (float(QUENCH_RAIN_MAP_ROW) / float(QUENCH_WEATHER_MAPS_HEIGHT));
    return lerp(maps.SampleLevel(s, float2(uv.x, v), 0.0).r, outside, beyond);
}

// The same, fading to the share of the sun's light that the cover around the camera lets through.
float quenchCloudShadow(Texture2D<float4> maps, SamplerState s, float3 P) {
    return quenchCloudShadow(maps, s, P, 1.0 - quenchFrame.overcast.w);
}

// The sky with the scene's clouds as one overcast amount: the plain sky blended toward the radiance of the cloud
// bases by how overcast it is around the camera. What rays that miss the scene see (ray-traced reflections, the
// probe grid, the path tracer); surfaces read the sky cube with the clouds themselves where there is one.
float3 quenchCloudySky(float3 dir) {
    const float3 sky = skyRadiance(dir);
    if (quenchFrame.overcast.w <= 0.0) return sky;
    return lerp(sky, quenchFrame.overcast.rgb, quenchFrame.overcast.w * saturate(dir.y * 4.0 + 0.6));
}

// The share of the sun's light the traced effects' hits get under the clouds: what an even cover lets through on
// average (the hits do not read the cloud shadow map, which covers the ground near the camera only).
float quenchOvercastSun() { return 1.0 - quenchFrame.overcast.w; }

// The sky a surface reflects along R: the sky cube (surface sampler 15: the sky with its clouds, without the sun's
// disc) where it is drawn, blurred by its mips with the roughness; else quenchCloudySky.
float3 quenchSkyReflection(TextureCube<float4> cube, SamplerState s, float3 R, float roughness) {
    [branch] if (quenchFrame.skyCube.x > 0.0) {
        return cube.SampleLevel(s, R, saturate(roughness) * quenchFrame.skyCube.y).rgb;
    }
    return quenchCloudySky(R);
}

#endif
