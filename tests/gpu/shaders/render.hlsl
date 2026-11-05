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
