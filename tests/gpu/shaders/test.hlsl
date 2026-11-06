// Shader set of quench_gpu_tests: a vertex, a fragment and a compute entry point, an #include next to this file and
// one from INCLUDE_DIRS.

#include "TestLocal.hlsli"
#include "TestLighting.hlsli"

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

#ifdef QUENCH_ENTRY_test_vs
cbuffer Transform : register(b0, space1) {
    float4x4 u_mvp;
};

VsOut test_vs(float3 pos : TEXCOORD0, float2 uv : TEXCOORD1) {
    VsOut o;
    o.pos = mul(u_mvp, float4(pos, 1.0));
    o.uv = uv;
    return o;
}
#endif

#ifdef QUENCH_ENTRY_test_fs
Texture2D<float4> t_albedo : register(t0, space2);
SamplerState s_albedo : register(s0, space2);
cbuffer Light : register(b0, space3) {
    float4 u_lightDir;
    float4 u_lightColor;
};

float4 test_fs(VsOut i) : SV_Target0 {
    const float4 albedo = t_albedo.Sample(s_albedo, i.uv);
    return float4(testTint(albedo.rgb) * lambert(float3(0.0, 1.0, 0.0), u_lightDir.xyz) * u_lightColor.rgb, albedo.a);
}
#endif

#ifdef QUENCH_ENTRY_test_cs
StructuredBuffer<float4> b_input : register(t0, space0);
RWStructuredBuffer<float4> b_first : register(u0, space1);
RWStructuredBuffer<float4> b_second : register(u1, space1);
cbuffer Params : register(b0, space2) {
    float4 u_scale;
};

[numthreads(8, 4, 2)]
void test_cs(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x + id.y * 8 + id.z * 32;
    b_first[i] = b_input[i] * u_scale;
    b_second[i] = testTint(b_input[i].xyz).xyzz;
}
#endif
