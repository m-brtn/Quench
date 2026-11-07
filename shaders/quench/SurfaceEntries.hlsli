#ifndef QUENCH_SURFACE_ENTRIES_HLSLI
#define QUENCH_SURFACE_ENTRIES_HLSLI

// The entry points of a surface shader (quench/Surface.hlsli): surface_vs, surface_fs, surface_basic_fs,
// surface_local_shadows_fs, surface_shadow_vs, surface_shadow_fs, surface_transmittance_fs and surface_gbuffer_fs.
// Include it after the surface function; QUENCH_SURFACE names it (default: quenchStandardSurface). In a ray hit
// (QUENCH_SURFACE_HIT) it adds nothing: the trace shader calls the function itself.

#include "quench/Surface.hlsli"

#ifndef QUENCH_SURFACE
#define QUENCH_SURFACE quenchStandardSurface
#endif

#if !QUENCH_SURFACE_HIT

#if defined(QUENCH_STAGE_VERTEX)
StructuredBuffer<InstanceData> quenchInstances : QUENCH_STORAGE(0);
cbuffer QuenchDrawBlock : QUENCH_UNIFORM(1) {
    DrawUniforms quenchDraw;
};

InstanceData quenchInstance(uint instance) { return quenchInstances[uint(quenchDraw.info.x) + instance]; }
#endif

#if defined(QUENCH_ENTRY_surface_vs)
// In the cascades' passes (cut-out and translucent casters) the position is the shadow map's.
QuenchVaryings surface_vs(QuenchVertexIn v, uint instance : SV_InstanceID) {
    const InstanceData inst = quenchInstance(instance);
#ifdef QUENCH_SURFACE_VERTEX
    QUENCH_SURFACE_VERTEX(v, inst);
#endif
    QuenchVaryings o;
    const float4 world = mul(inst.model, float4(v.position, 1.0));
    o.position = quenchDraw.info.z > 0.5 ? mul(quenchFrame.shadowViewProj[int(quenchDraw.info.y)], world)
                                         : mul(quenchFrame.viewProj, world);
    o.world = world.xyz;
    o.normal = mul(inst.normalMatrix, float4(v.normal, 0.0)).xyz;
    o.uv = v.uv;
    o.color = v.color * inst.color;
    o.custom = inst.custom;
    // A mirroring transform (negative determinant) turns the bitangent around.
    const float3x3 basis = (float3x3)inst.model;
    const float mirrored = dot(cross(basis[0], basis[1]), basis[2]) < 0.0 ? -1.0 : 1.0;
    o.tangent = float4(mul(basis, v.tangent.xyz), (v.tangent.w < 0.0 ? -1.0 : 1.0) * mirrored);
    return o;
}
#endif

#if defined(QUENCH_ENTRY_surface_shadow_vs)
float4 surface_shadow_vs(QuenchVertexIn v, uint instance : SV_InstanceID) : SV_Position {
    const InstanceData inst = quenchInstance(instance);
#ifdef QUENCH_SURFACE_VERTEX
    QUENCH_SURFACE_VERTEX(v, inst);
#endif
    return mul(quenchFrame.shadowViewProj[int(quenchDraw.info.y)], mul(inst.model, float4(v.position, 1.0)));
}
#endif

#if defined(QUENCH_STAGE_FRAGMENT)
QuenchSurfaceInput quenchSurfaceInput(QuenchVaryings v, bool front, float3 view) {
    QuenchSurfaceInput i;
    i.world = v.world;
    i.normal = normalize(v.normal) * (front ? 1.0 : -1.0);
    const float t2 = dot(v.tangent.xyz, v.tangent.xyz);
    i.tangent = float4(t2 > 1e-12 ? v.tangent.xyz * rsqrt(t2) : float3(0.0, 0.0, 0.0), v.tangent.w < 0.0 ? -1.0 : 1.0);
    i.view = view;
    i.uv = v.uv;
    i.color = v.color;
    i.custom = v.custom;
    i.pixel = v.position.xy;
    i.front = front;
    return i;
}
#endif

#if defined(QUENCH_ENTRY_surface_fs) || defined(QUENCH_ENTRY_surface_basic_fs) ||                                     \
    defined(QUENCH_ENTRY_surface_local_shadows_fs)
#if defined(QUENCH_ENTRY_surface_fs)
#define QUENCH_SURFACE_FS surface_fs
#elif defined(QUENCH_ENTRY_surface_basic_fs)
#define QUENCH_SURFACE_FS surface_basic_fs
#else
#define QUENCH_SURFACE_FS surface_local_shadows_fs
#endif
float4 QUENCH_SURFACE_FS(QuenchVaryings v, bool front : SV_IsFrontFace) : SV_Target0 {
    const QuenchSurfaceInput i = quenchSurfaceInput(v, front, normalize(quenchFrame.cameraPos.xyz - v.world));
    QuenchSurface s = QUENCH_SURFACE(i);
    if (quenchMaterial.surface.z > 0.0 && s.opacity < quenchMaterial.surface.z) discard;
#ifndef QUENCH_SURFACE_NO_RAIN
    quenchApplyRain(s, i);
#endif
    const float3 lit = quenchShade(s, i);
    const float4 fog = fogAt(v.world, quenchFrame.cameraPos.xyz);
    return float4(lerp(lit, fog.rgb, fog.a), saturate(s.opacity));
}
#endif

#if defined(QUENCH_ENTRY_surface_gbuffer_fs)
// The ray tracing prepass with a G-buffer (ray-traced reflections and indirect light, the denoising upscaler): the
// surface's shading normal and roughness, its albedo and metallic, cut out as in the HDR pass.
struct QuenchGbuffer {
    float4 normal : SV_Target0; // xyz: unit shading normal (world), w: roughness
    float4 albedo : SV_Target1; // rgb: albedo, a: metallic
};

QuenchGbuffer surface_gbuffer_fs(QuenchVaryings v, bool front : SV_IsFrontFace) {
    const QuenchSurfaceInput i = quenchSurfaceInput(v, front, normalize(quenchFrame.cameraPos.xyz - v.world));
    QuenchSurface s = QUENCH_SURFACE(i);
    if (quenchMaterial.surface.z > 0.0 && s.opacity < quenchMaterial.surface.z) discard;
#ifndef QUENCH_SURFACE_NO_RAIN
    quenchApplyRain(s, i);
#endif
    QuenchGbuffer o;
    o.normal = float4(s.normal, saturate(s.roughness));
    o.albedo = float4(saturate(s.albedo), saturate(s.metallic));
    return o;
}
#endif

#if defined(QUENCH_ENTRY_surface_shadow_fs)
// The depth pass of a cascade for cut-out casters: no depth where the surface is cut out (MaterialDesc::alphaCutoff),
// and for translucent casters dithered into the depth map (MaterialUniforms::emissive.w), where the ordered dither
// is above the opacity.
void surface_shadow_fs(QuenchVaryings v, bool front : SV_IsFrontFace) {
    const QuenchSurface s = QUENCH_SURFACE(quenchSurfaceInput(v, front, quenchFrame.sunDir.xyz));
    if (quenchMaterial.surface.z > 0.0 && s.opacity < quenchMaterial.surface.z) discard;
    if (quenchMaterial.emissive.w > 0.5 && saturate(s.opacity) <= bayer4(v.position.xy)) discard;
}
#endif

#if defined(QUENCH_ENTRY_surface_transmittance_fs)
// The transmittance map of a cascade, blended by multiplying rgb and keeping the largest alpha: the light the
// surface lets through (quenchTransmittance) and 1 - its depth.
float4 surface_transmittance_fs(QuenchVaryings v, bool front : SV_IsFrontFace) : SV_Target0 {
    const QuenchSurface s = QUENCH_SURFACE(quenchSurfaceInput(v, front, quenchFrame.sunDir.xyz));
    if (quenchMaterial.surface.z > 0.0 && s.opacity < quenchMaterial.surface.z) discard;
    return float4(quenchTransmittance(s), saturate(1.0 - v.position.z));
}
#endif

#endif
#endif
