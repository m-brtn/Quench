#ifndef QUENCH_SURFACE_ENTRIES_HLSLI
#define QUENCH_SURFACE_ENTRIES_HLSLI

// The entry points of a surface shader (quench/Surface.hlsli): surface_vs, surface_fs and surface_shadow_vs. Include
// it after the surface function; QUENCH_SURFACE names it (default: quenchStandardSurface).

#include "quench/Surface.hlsli"

#ifndef QUENCH_SURFACE
#define QUENCH_SURFACE quenchStandardSurface
#endif

#if defined(QUENCH_STAGE_VERTEX)
StructuredBuffer<InstanceData> quenchInstances : QUENCH_STORAGE(0);
cbuffer QuenchDrawBlock : QUENCH_UNIFORM(1) {
    DrawUniforms quenchDraw;
};

InstanceData quenchInstance(uint instance) { return quenchInstances[uint(quenchDraw.info.x) + instance]; }
#endif

#if defined(QUENCH_ENTRY_surface_vs)
QuenchVaryings surface_vs(QuenchVertexIn v, uint instance : SV_InstanceID) {
    const InstanceData inst = quenchInstance(instance);
#ifdef QUENCH_SURFACE_VERTEX
    QUENCH_SURFACE_VERTEX(v, inst);
#endif
    QuenchVaryings o;
    const float4 world = mul(inst.model, float4(v.position, 1.0));
    o.position = mul(quenchFrame.viewProj, world);
    o.world = world.xyz;
    o.normal = mul(inst.normalMatrix, float4(v.normal, 0.0)).xyz;
    o.uv = v.uv;
    o.color = v.color * inst.color;
    o.custom = inst.custom;
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

#if defined(QUENCH_ENTRY_surface_fs)
float4 surface_fs(QuenchVaryings v, bool front : SV_IsFrontFace) : SV_Target0 {
    QuenchSurfaceInput i;
    i.world = v.world;
    i.normal = normalize(v.normal) * (front ? 1.0 : -1.0);
    i.view = normalize(quenchFrame.cameraPos.xyz - v.world);
    i.uv = v.uv;
    i.color = v.color;
    i.custom = v.custom;
    i.pixel = v.position.xy;
    i.front = front;
    const QuenchSurface s = QUENCH_SURFACE(i);
    if (quenchMaterial.surface.z > 0.0 && s.opacity < quenchMaterial.surface.z) discard;
    const float3 lit = quenchShade(s, i);
    const float4 fog = fogAt(v.world, quenchFrame.cameraPos.xyz);
    return float4(lerp(lit, fog.rgb, fog.a), saturate(s.opacity));
}
#endif

#endif
