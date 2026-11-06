#ifndef QUENCH_SURFACE_HLSLI
#define QUENCH_SURFACE_HLSLI

// Surface shaders: how a material looks, written by the engine (the standard surface) or by a game, and lit by the
// engine (sun with cascaded shadows, local lights, sky ambient and reflections, fog). A surface shader is one HLSL
// file built with quench_add_shaders(... ENTRIES surface_vs:vertex surface_fs:fragment) and registered with
// quench::render::Renderer::addSurface (docs/render.md):
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
#include "quench/Shadows.hlsli"
#include "quench/Sky.hlsli"

// The engine's vertex (quench::render::Vertex): 48 bytes.
struct QuenchVertexIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;
};

struct QuenchVaryings {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : TEXCOORD3;  // vertex color times InstanceDesc::color
    float4 custom : TEXCOORD4; // InstanceDesc::custom
};

// What a surface function gets for a pixel.
struct QuenchSurfaceInput {
    float3 world;  // position (m)
    float3 normal; // unit geometric normal (interpolated), turned toward the viewer on back faces
    float3 view;   // unit direction toward the camera
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

// True when the material has texture n (MaterialDesc::textures); unset textures sample white.
bool quenchHasTexture(int n) { return (uint(quenchMaterial.surface.w) & (1u << uint(n))) != 0u; }

// The standard surface: base color times texture 0 (sRGB) and the vertex color, roughness and metallic times the
// green and blue channels of texture 1 (glTF's metallic-roughness layout), emissive.
QuenchSurface quenchStandardSurface(QuenchSurfaceInput i) {
    QuenchSurface s;
    const float4 base = quenchMaterial.baseColor * quenchTexture0.Sample(quenchSampler0, i.uv) * i.color;
    const float4 mr = quenchTexture1.Sample(quenchSampler1, i.uv);
    s.albedo = base.rgb;
    s.opacity = base.a;
    s.roughness = saturate(quenchMaterial.surface.x * mr.g);
    s.metallic = saturate(quenchMaterial.surface.y * mr.b);
    s.normal = i.normal;
    s.emissive = quenchMaterial.emissive.rgb;
    s.occlusion = 1.0;
    return s;
}

// The sun's light a translucent caster lets through: 1 - opacity, tinted toward the surface's hue (its albedo over
// its brightest channel) as far as it is opaque. A grey surface of opacity a passes 1 - a in every channel.
float3 quenchTransmittance(QuenchSurface s) {
    const float a = saturate(s.opacity);
    const float peak = max(s.albedo.r, max(s.albedo.g, s.albedo.b));
    const float3 hue = peak > 1e-4 ? saturate(s.albedo / peak) : float3(1.0, 1.0, 1.0);
    return (1.0 - a) * lerp(float3(1.0, 1.0, 1.0), hue, a);
}

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
    if (quenchFrame.sunDir.w > 0.0) {
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
        }
        if (attenuation > 0.0) color += quenchDirectLight(s, V, L, light.color.rgb * attenuation);
    }
    const float3 F0 = lerp(((float3)(0.04)), s.albedo, s.metallic);
    const float3 ambient = quenchAmbient(N);
    color += ambient * s.albedo * (1.0 - s.metallic) * s.occlusion;
    // Sun-shadowed areas see less of the bright sky in their reflection.
    const float3 R = reflect(-V, N);
    const float3 env = lerp(skyRadiance(R), ambient, saturate(s.roughness * 1.2)) * lerp(0.35, 1.0, shadow);
    color += env * envBRDFApprox(F0, s.roughness, NdotV) * s.occlusion;
    return color + s.emissive;
}
#endif

#endif
