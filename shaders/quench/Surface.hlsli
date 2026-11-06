#ifndef QUENCH_SURFACE_HLSLI
#define QUENCH_SURFACE_HLSLI

// Surface shaders: how a material looks, written by the engine (the standard surface) or by a game, and lit by the
// engine (sun with cascaded or ray-traced shadows, local lights, sky ambient and reflections with ray-traced
// occlusion where it runs, fog). A surface shader is one HLSL file built with quench_add_shaders(... ENTRIES
// surface_vs:vertex surface_fs:fragment) and registered with quench::render::Renderer::addSurface (docs/render.md):
//
//   #include "quench/Surface.hlsli"
//   #if defined(QUENCH_STAGE_FRAGMENT) // the material, the textures and the lighting exist in the fragment stage only
//   QuenchSurface checker(QuenchSurfaceInput i) {
//       QuenchSurface s = quenchStandardSurface(i);
//       s.albedo *= fmod(floor(i.world.x) + floor(i.world.z), 2.0) == 0.0 ? 1.0 : 0.5;
//       return s;
//   }
//   #endif
//   #define QUENCH_SURFACE checker
//   #include "quench/SurfaceEntries.hlsli"
//
// Optional: QUENCH_SURFACE_VERTEX names a function `void f(inout QuenchVertexIn v, InstanceData instance)` that moves
// the vertex in object space before the transform (wind, procedural animation); then also build the entry
// surface_shadow_vs:vertex and pass it as SurfaceDesc::shadowVertexShader, so the shadows move with it.
//
// Shadows of cut-out and translucent materials run the surface function too, in the cascades' passes: build
// surface_shadow_fs:fragment and surface_transmittance_fs:fragment and pass them as SurfaceDesc::shadowFragmentShader
// and transmittanceShader, so that their holes and opacity are the surface's own (without them the standard surface's
// opacity is used). There, QuenchSurfaceInput::view points toward the sun and pixel is the shadow map texel.

#include "quench/Brdf.hlsli"
#include "quench/Common.hlsli"
#include "quench/Fog.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Probes.hlsli"
#include "quench/Shadows.hlsli"
#include "quench/Sky.hlsli"

// The engine's vertex (quench::render::Vertex): 64 bytes.
struct QuenchVertexIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
    float4 tangent : TEXCOORD4; // xyz along +u, w: the bitangent's sign (Vertex::tangent)
};

struct QuenchVaryings {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;   // vertex color times InstanceDesc::color
    float4 custom : TEXCOORD4;  // InstanceDesc::custom
    float4 tangent : TEXCOORD5; // world tangent, w: the bitangent's sign (mirroring transforms included)
};

// What a surface function gets for a pixel.
struct QuenchSurfaceInput {
    float3 world;   // position (m)
    float3 normal;  // unit geometric normal (interpolated), turned toward the viewer on back faces
    float4 tangent; // unit world tangent along +u, w: bitangent = w * cross(normal, tangent) (toward -v)
    float3 view;    // unit direction toward the camera
    float2 uv;
    float4 color;  // vertex color times InstanceDesc::color
    float4 custom; // InstanceDesc::custom
    float2 pixel;  // render target position (pixels)
    bool front;
};

// What a surface function returns; lit by quenchShade.
struct QuenchSurface {
    float3 albedo;   // linear
    float opacity;   // 0..1: alpha blended materials and their shadows; alpha tested with MaterialDesc::alphaCutoff
    float roughness; // perceptual, 0..1
    float metallic;
    float3 normal;   // unit shading normal (world)
    float3 emissive; // linear radiance, added after lighting
    float occlusion; // ambient occlusion of the indirect light, 0..1
};

#if defined(QUENCH_STAGE_FRAGMENT)
cbuffer QuenchLightBlock : QUENCH_UNIFORM(1) {
    LightUniforms quenchLights;
};
cbuffer QuenchMaterialBlock : QUENCH_UNIFORM(2) {
    MaterialUniforms quenchMaterial;
};
Texture2DArray<float> quenchShadowMap : QUENCH_TEXTURE(0);
SamplerState quenchShadowSampler : QUENCH_SAMPLER(0);
Texture2D<float4> quenchTexture0 : QUENCH_TEXTURE(1);
SamplerState quenchSampler0 : QUENCH_SAMPLER(1);
Texture2D<float4> quenchTexture1 : QUENCH_TEXTURE(2);
SamplerState quenchSampler1 : QUENCH_SAMPLER(2);
Texture2D<float4> quenchTexture2 : QUENCH_TEXTURE(3);
SamplerState quenchSampler2 : QUENCH_SAMPLER(3);
Texture2D<float4> quenchTexture3 : QUENCH_TEXTURE(4);
SamplerState quenchSampler3 : QUENCH_SAMPLER(4);
Texture2DArray<float4> quenchTransmittanceMap : QUENCH_TEXTURE(5);
SamplerState quenchTransmittanceSampler : QUENCH_SAMPLER(5);
// Ray-traced sun visibility (rgb) and ambient occlusion (a) at the render size (FrameUniforms::options.w); white where
// nothing was traced (transparent surfaces, game passes that bind it so).
Texture2D<float4> quenchRayMask : QUENCH_TEXTURE(6);
SamplerState quenchRayMaskSampler : QUENCH_SAMPLER(6);
// Ray-traced reflections (rgb radiance, a: weight), indirect light (rgb: irradiance / pi) and the shadows of up to
// four local lights (one channel each) at the render size, and the probe grid's atlases (quench/Probes.hlsli);
// FrameUniforms::options.w says which are read.
Texture2D<float4> quenchRayReflections : QUENCH_TEXTURE(7);
SamplerState quenchRayReflectionsSampler : QUENCH_SAMPLER(7);
Texture2D<float4> quenchRayIndirect : QUENCH_TEXTURE(8);
SamplerState quenchRayIndirectSampler : QUENCH_SAMPLER(8);
Texture2D<float4> quenchRayLocalShadows : QUENCH_TEXTURE(9);
SamplerState quenchRayLocalShadowsSampler : QUENCH_SAMPLER(9);
Texture2D<float4> quenchProbeIrradianceAtlas : QUENCH_TEXTURE(10);
SamplerState quenchProbeSampler : QUENCH_SAMPLER(10);
Texture2D<float4> quenchProbeDistanceAtlas : QUENCH_TEXTURE(11);
SamplerState quenchProbeDistanceSampler : QUENCH_SAMPLER(11);

// True when the material has texture n (MaterialDesc::textures); unset textures sample white.
bool quenchHasTexture(int n) { return (uint(quenchMaterial.surface.w) & (1u << uint(n))) != 0u; }

// The shading normal of a tangent-space normal map texel (glTF's encoding: xyz * 0.5 + 0.5, +y toward -v) in the
// frame of the input's normal and tangent; the geometric normal where the tangent gives no frame.
float3 quenchNormalFromMap(QuenchSurfaceInput i, float3 texel) {
    const float3 n = texel * 2.0 - 1.0;
    const float3 t = i.tangent.xyz - i.normal * dot(i.normal, i.tangent.xyz);
    const float t2 = dot(t, t);
    if (t2 < 1e-8) return i.normal;
    const float3 T = t * rsqrt(t2);
    const float3 B = cross(i.normal, T) * i.tangent.w;
    const float3 N = T * n.x + B * n.y + i.normal * n.z;
    const float n2 = dot(N, N);
    return n2 > 1e-8 ? N * rsqrt(n2) : i.normal;
}

// The standard surface: base color times texture 0 (sRGB) and the vertex color, roughness and metallic times the
// green and blue channels of texture 1 (glTF's metallic-roughness layout), the normal map of texture 2 (tangent
// space), emissive times texture 3 (sRGB).
QuenchSurface quenchStandardSurface(QuenchSurfaceInput i) {
    QuenchSurface s;
    const float4 base = quenchMaterial.baseColor * quenchTexture0.Sample(quenchSampler0, i.uv) * i.color;
    const float4 mr = quenchTexture1.Sample(quenchSampler1, i.uv);
    s.albedo = base.rgb;
    s.opacity = base.a;
    s.roughness = saturate(quenchMaterial.surface.x * mr.g);
    s.metallic = saturate(quenchMaterial.surface.y * mr.b);
    s.normal = i.normal;
    [branch] if (quenchHasTexture(2)) {
        s.normal = quenchNormalFromMap(i, quenchTexture2.Sample(quenchSampler2, i.uv).rgb);
    }
    s.emissive = quenchMaterial.emissive.rgb * quenchTexture3.Sample(quenchSampler3, i.uv).rgb;
    s.occlusion = 1.0;
    return s;
}

// The sun's light a translucent caster lets through (quenchTransmittanceOf).
float3 quenchTransmittance(QuenchSurface s) { return quenchTransmittanceOf(s.albedo, s.opacity); }

// Diffuse irradiance from the sky hemisphere, blended with the ground bounce by the normal.
float3 quenchAmbient(float3 N) {
    return lerp(quenchFrame.groundAmbient.rgb, quenchFrame.skyAmbient.rgb, N.y * 0.5 + 0.5);
}

// Light from one direction L with irradiance E on a surface facing it (GGX specular, Lambert diffuse).
float3 quenchDirectLight(QuenchSurface s, float3 V, float3 L, float3 E) {
    const float3 N = s.normal;
    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return float3(0.0, 0.0, 0.0);
    const float3 H = normalize(L + V);
    const float NdotV = max(dot(N, V), 1e-3);
    const float NdotH = saturate(dot(N, H));
    const float3 F0 = lerp(((float3)(0.04)), s.albedo, s.metallic);
    const float3 F = fresnelSchlick(F0, saturate(dot(V, H)));
    const float3 kd = (1.0 - F) * (1.0 - s.metallic);
    return (kd * s.albedo / QUENCH_PI + F * ggxSpec(NdotH, NdotV, NdotL, s.roughness)) * E * NdotL;
}

// The engine's lighting of a surface: the sun with its shadows, the local lights, the sky as ambient and as a
// reflection blurred toward the ambient with roughness (TrailForge's shadeSplitEnv without its game terms).
float3 quenchShade(QuenchSurface s, QuenchSurfaceInput i) {
    const float3 V = i.view;
    const float3 N = s.normal;
    const float NdotV = max(dot(N, V), 1e-3);
    float shadow = 1.0;
    float3 color = float3(0.0, 0.0, 0.0);
    const uint rayTraced = uint(quenchFrame.options.w);
    float4 rayMask = float4(1.0, 1.0, 1.0, 1.0);
    [branch] if (rayTraced != 0u) {
        rayMask = quenchRayMask.SampleLevel(quenchRayMaskSampler, i.pixel * quenchFrame.screen.zw, 0.0);
    }
    [branch] if (quenchFrame.sunDir.w > 0.0 && (rayTraced & 1u) != 0u) {
        color += quenchDirectLight(s, V, quenchFrame.sunDir.xyz, quenchFrame.sunColor.rgb * rayMask.rgb);
        shadow = luminance(rayMask.rgb);
    } else if (quenchFrame.sunDir.w > 0.0) {
        shadow = shadowFactor(quenchShadowMap, quenchShadowSampler, i.world, i.normal);
        // Two calls, not one with a tinted radiance: merged, the compiler orders the products differently and frames
        // without translucent casters change by a few levels.
        [branch] if (shadow > 0.0 && quenchFrame.shadowTransmittance.x > 0.0) {
            const float3 t = shadowTransmittance(quenchTransmittanceMap, quenchTransmittanceSampler, i.world, i.normal);
            color += quenchDirectLight(s, V, quenchFrame.sunDir.xyz, quenchFrame.sunColor.rgb * shadow * t);
            shadow *= luminance(t);
        } else {
            color += quenchDirectLight(s, V, quenchFrame.sunDir.xyz, quenchFrame.sunColor.rgb * shadow);
        }
    }
    const int lights = int(quenchLights.count.x);
    const bool localRays = (rayTraced & 4u) != 0u;
    float4 localShadows = float4(1.0, 1.0, 1.0, 1.0);
    [branch] if (localRays) {
        localShadows = quenchRayLocalShadows.SampleLevel(quenchRayLocalShadowsSampler, i.pixel * quenchFrame.screen.zw,
                                                         0.0);
    }
    for (int k = 0; k < lights; ++k) {
        const LocalLight light = quenchLights.lights[k];
        const int type = int(light.color.w);
        float3 L = -light.direction.xyz;
        float attenuation = 1.0;
        if (type != 0) {
            const float3 d = light.positionRange.xyz - i.world;
            const float d2 = max(dot(d, d), 1e-4);
            L = d * rsqrt(d2);
            // Inverse square with a smooth window to zero at the range (Karis 2013).
            const float r = d2 / max(light.positionRange.w * light.positionRange.w, 1e-6);
            const float window = saturate(1.0 - r * r);
            attenuation = window * window / d2;
            if (type == 2) {
                const float t = saturate((dot(-L, light.direction.xyz) - light.direction.w) * light.cone.x);
                attenuation *= t * t;
            }
            // A light with ray-traced shadows: its channel in sampler 9 (LocalLight::cone.y = channel + 1).
            if (localRays && light.cone.y > 0.0) {
                const int c = int(light.cone.y) - 1;
                attenuation *= dot(localShadows, float4(c == 0, c == 1, c == 2, c == 3));
            }
        }
        if (attenuation > 0.0) color += quenchDirectLight(s, V, L, light.color.rgb * attenuation);
    }
    const float3 F0 = lerp(((float3)(0.04)), s.albedo, s.metallic);
    const float2 screenUv = i.pixel * quenchFrame.screen.zw;
    // The indirect light: traced per pixel (it has its own occlusion), the probe grid, or the sky's ambient.
    const bool tracedIndirect = (rayTraced & 16u) != 0u;
    float3 ambient = quenchAmbient(N);
    [branch] if (tracedIndirect) {
        ambient = quenchRayIndirect.SampleLevel(quenchRayIndirectSampler, screenUv, 0.0).rgb;
    } else if ((rayTraced & 32u) != 0u) {
        ambient = quenchProbeIrradiance(quenchProbeIrradianceAtlas, quenchProbeDistanceAtlas, quenchProbeSampler,
                                        i.world, N, V, ambient);
    }
    const float occlusion = (rayTraced & 2u) != 0u && !tracedIndirect ? s.occlusion * rayMask.a : s.occlusion;
    color += ambient * s.albedo * (1.0 - s.metallic) * occlusion;
    // Sun-shadowed areas see less of the bright sky in their reflection.
    const float3 R = reflect(-V, N);
    float3 env = lerp(skyRadiance(R), ambient, saturate(s.roughness * 1.2)) * lerp(0.35, 1.0, shadow);
    float envOcclusion = occlusion;
    // Traced reflections up to the roughness cutoff, faded into the sky's over its last quarter.
    [branch] if ((rayTraced & 8u) != 0u) {
        const float4 traced = quenchRayReflections.SampleLevel(quenchRayReflectionsSampler, screenUv, 0.0);
        const float cutoff = quenchFrame.rayLighting.x;
        const float w = traced.a > 0.0 ? saturate((cutoff - s.roughness) / max(cutoff * 0.25, 1e-3)) : 0.0;
        env = lerp(env, traced.rgb, w);
        envOcclusion = lerp(occlusion, 1.0, w);
    }
    color += env * envBRDFApprox(F0, s.roughness, NdotV) * envOcclusion;
    return color + s.emissive;
}
#endif

#endif
