// Shader set of quench_gpu_device_tests: small entry points whose output the tests read back.

struct ColorVsOut {
    float4 pos : SV_Position;
    float4 color : TEXCOORD0;
};

struct FullscreenOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0; // (0, 0) at the top left of the target
};

#ifdef QUENCH_ENTRY_color_vs
ColorVsOut color_vs(float3 pos : TEXCOORD0, float4 color : TEXCOORD1) {
    ColorVsOut o;
    o.pos = float4(pos, 1.0);
    o.color = color;
    return o;
}
#endif

#ifdef QUENCH_ENTRY_color_fs
float4 color_fs(ColorVsOut i) : SV_Target0 {
    return i.color;
}
#endif

// One triangle that covers the target, without vertex buffers.
#ifdef QUENCH_ENTRY_fullscreen_vs
FullscreenOut fullscreen_vs(uint id : SV_VertexID) {
    FullscreenOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
#endif

#ifdef QUENCH_ENTRY_uniform_fs
cbuffer Tint : register(b0, space3) {
    float4 u_color;
};

float4 uniform_fs(FullscreenOut i) : SV_Target0 {
    return u_color;
}
#endif

#ifdef QUENCH_ENTRY_texture_fs
Texture2D<float4> t_image : register(t0, space2);
SamplerState s_image : register(s0, space2);

float4 texture_fs(FullscreenOut i) : SV_Target0 {
    return t_image.Sample(s_image, i.uv);
}
#endif

// One rectangle per instance (a 4-vertex triangle strip) from a storage buffer, moved by a vertex uniform.
#ifdef QUENCH_ENTRY_rects_vs
struct RectData {
    float4 bounds; // min xy, max xy in clip space
    float4 color;
};
StructuredBuffer<RectData> b_rects : register(t0, space0);
cbuffer Offset : register(b0, space1) {
    float4 u_offset;
};

ColorVsOut rects_vs(uint id : SV_VertexID, uint instance : SV_InstanceID) {
    const RectData r = b_rects[instance];
    const float2 corner = float2(id & 1, (id >> 1) & 1);
    ColorVsOut o;
    o.pos = float4(lerp(r.bounds.xy, r.bounds.zw, corner) + u_offset.xy, 0.5, 1.0);
    o.color = r.color;
    return o;
}
#endif

// out[i] = a[i] * scale + b[i] + i for i < count; two read-only inputs, one read-write output.
#ifdef QUENCH_ENTRY_scale_cs
StructuredBuffer<float4> b_a : register(t0, space0);
StructuredBuffer<float4> b_b : register(t1, space0);
RWStructuredBuffer<float4> b_out : register(u0, space1);
cbuffer Params : register(b0, space2) {
    float4 u_params; // scale, count
};

[numthreads(32, 1, 1)]
void scale_cs(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(u_params.y)) return;
    b_out[id.x] = b_a[id.x] * u_params.x + b_b[id.x] + float(id.x);
}
#endif

// Rays down the -z axis from z = 10 through the target (x, y in -1..1): r 1 on a hit, g the instance id / 255, b the
// distance / 20 (gpu::Caps::rayTracing; compiled by DXC).
#ifdef QUENCH_ENTRY_ray_fs
RaytracingAccelerationStructure t_scene : register(t0, space2);

float4 ray_fs(FullscreenOut i) : SV_Target0 {
    RayDesc ray;
    ray.Origin = float3(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, 10.0);
    ray.Direction = float3(0.0, 0.0, -1.0);
    ray.TMin = 0.0;
    ray.TMax = 100.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(t_scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return float4(0.0, 0.0, 0.0, 1.0);
    return float4(1.0, float(q.CommittedInstanceID()) / 255.0, q.CommittedRayT() / 20.0, 1.0);
}
#endif

// The rays of ray_fs: r 1 on a hit, g 1 when it hit a front face, b the instance id / 255.
#ifdef QUENCH_ENTRY_ray_face_fs
RaytracingAccelerationStructure t_scene : register(t0, space2);

float4 ray_face_fs(FullscreenOut i) : SV_Target0 {
    RayDesc ray;
    ray.Origin = float3(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, 10.0);
    ray.Direction = float3(0.0, 0.0, -1.0);
    ray.TMin = 0.0;
    ray.TMax = 100.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(t_scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return float4(0.0, 0.0, 0.0, 1.0);
    return float4(1.0, q.CommittedTriangleFrontFace() ? 1.0 : 0.0, float(q.CommittedInstanceID()) / 255.0, 1.0);
}
#endif
