// GPU skinning of quench::render::Renderer: a compute pass before the frame poses the bind-pose vertices of every
// skinned instance with its joint palette (linear blend skinning, four joints per vertex) into the instance's range
// of a shared vertex buffer, which the shadow, surface and motion vector passes then draw like a rigid mesh.
//
// Buffers hold float4 and uint4 elements only, so their layout is the same in SPIR-V, MSL and DXIL:
//   vertices: four float4 per render::Vertex (position.xyz normal.x | normal.yz uv | color | tangent)
//   skin:     one uint4 per vertex, render::SkinVertex (joints as 2 x 16 bits in x and y, weights in z and w)
//   palette:  three float4 per joint, the rows of its affine matrix

#include "quench/Bindings.hlsli"

#include "PostUniforms.h"

#if defined(QUENCH_ENTRY_skin_cs)
StructuredBuffer<float4> bindVertices : QUENCH_STORAGE(0);
StructuredBuffer<uint4> skin : QUENCH_STORAGE(1);
StructuredBuffer<float4> palette : QUENCH_STORAGE(2);
RWStructuredBuffer<float4> skinned : QUENCH_RW_STORAGE(0);
cbuffer SkinBlock : QUENCH_UNIFORM(0) {
    SkinUniforms params;
};

[numthreads(64, 1, 1)]
void skin_cs(uint3 id : SV_DispatchThreadID) {
    const uint v = id.x;
    if (v >= uint(params.counts.x)) return;
    const float4 a = bindVertices[v * 4 + 0];
    const float4 b = bindVertices[v * 4 + 1];
    const float4 c = bindVertices[v * 4 + 2];
    const float4 t = bindVertices[v * 4 + 3];
    const uint4 s = skin[v];
    const uint joints[4] = {s.x & 0xffffu, s.x >> 16, s.y & 0xffffu, s.y >> 16};
    const float weights[4] = {float(s.z & 0xffffu), float(s.z >> 16), float(s.w & 0xffffu), float(s.w >> 16)};
    const float total = weights[0] + weights[1] + weights[2] + weights[3];

    float4 r0 = float4(1, 0, 0, 0), r1 = float4(0, 1, 0, 0), r2 = float4(0, 0, 1, 0);
    if (total > 0.0) {
        r0 = float4(0, 0, 0, 0);
        r1 = float4(0, 0, 0, 0);
        r2 = float4(0, 0, 0, 0);
        const uint first = uint(params.counts.y);
        [unroll] for (int k = 0; k < 4; ++k) {
            const float w = weights[k] / total;
            const uint j = (first + joints[k]) * 3;
            r0 += palette[j + 0] * w;
            r1 += palette[j + 1] * w;
            r2 += palette[j + 2] * w;
        }
    }
    const float4 p = float4(a.xyz, 1.0);
    const float3 position = float3(dot(r0, p), dot(r1, p), dot(r2, p));
    // The blended matrix's linear part: exact for rotations and uniform scales, an approximation for others.
    const float3 n0 = float3(a.w, b.x, b.y);
    float3 normal = float3(dot(r0.xyz, n0), dot(r1.xyz, n0), dot(r2.xyz, n0));
    const float len = length(normal);
    normal = len > 1e-8 ? normal / len : n0;
    float3 tangent = float3(dot(r0.xyz, t.xyz), dot(r1.xyz, t.xyz), dot(r2.xyz, t.xyz));
    const float tlen = length(tangent);
    tangent = tlen > 1e-8 ? tangent / tlen : t.xyz;

    const uint o = (uint(params.counts.z) + v) * 4;
    skinned[o + 0] = float4(position, normal.x);
    skinned[o + 1] = float4(normal.yz, b.zw);
    skinned[o + 2] = c;
    skinned[o + 3] = float4(tangent, t.w);
}
#endif
