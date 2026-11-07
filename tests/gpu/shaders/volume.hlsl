// Shader set of quench_gpu_device_tests' 3D texture and cube cases (VolumeTests.cpp): slices and faces drawn as
// color targets, then sampled.

#include "quench/Cube.hlsli"

struct FullscreenOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0; // (0, 0) at the top left of the target
};

#ifdef QUENCH_ENTRY_fullscreen_vs
FullscreenOut fullscreen_vs(uint id : SV_VertexID) {
    FullscreenOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
#endif

// A flat color (a slice of a 3D texture).
#ifdef QUENCH_ENTRY_flat_fs
cbuffer Flat : register(b0, space3) {
    float4 u_color;
};

float4 flat_fs(FullscreenOut i) : SV_Target0 {
    return u_color;
}
#endif

// The slice at depth u_depth.x (0..1) of a 3D texture.
#ifdef QUENCH_ENTRY_volume_fs
Texture3D<float4> t_volume : register(t0, space2);
SamplerState s_volume : register(s0, space2);
cbuffer Depth : register(b0, space3) {
    float4 u_depth;
};

float4 volume_fs(FullscreenOut i) : SV_Target0 {
    return t_volume.SampleLevel(s_volume, float3(i.uv, u_depth.x), 0.0);
}
#endif

// A cube face (u_face.x): the direction of each texel as a color (quenchCubeDirection).
#ifdef QUENCH_ENTRY_cube_face_fs
cbuffer Face : register(b0, space3) {
    float4 u_face;
};

float4 cube_face_fs(FullscreenOut i) : SV_Target0 {
    return float4(quenchCubeDirection(int(u_face.x), i.uv) * 0.5 + 0.5, 1.0);
}
#endif

// A cube sampled along directions, one per pixel of a 6 x 3 target: column = face, row 0 its center, row 1 halfway
// toward its first tangent, row 2 halfway toward its second; at mip level u_level.x.
#ifdef QUENCH_ENTRY_cube_sample_fs
TextureCube<float4> t_cube : register(t0, space2);
SamplerState s_cube : register(s0, space2);
cbuffer Level : register(b0, space3) {
    float4 u_level;
};

float3 cubeTestDirection(int face, int row) {
    static const float3 axes[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
                                   float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
    static const float3 firsts[6] = {float3(0, 1, 0), float3(0, 0, 1), float3(1, 0, 0),
                                     float3(0, 0, 1), float3(1, 0, 0), float3(0, 1, 0)};
    static const float3 seconds[6] = {float3(0, 0, 1), float3(0, 1, 0), float3(0, 0, 1),
                                      float3(1, 0, 0), float3(0, 1, 0), float3(1, 0, 0)};
    float3 d = axes[face];
    if (row == 1) d += firsts[face] * 0.5;
    if (row == 2) d -= seconds[face] * 0.5;
    return d;
}

float4 cube_sample_fs(FullscreenOut i) : SV_Target0 {
    const int face = int(i.uv.x * 6.0);
    const int row = int(i.uv.y * 3.0);
    return t_cube.SampleLevel(s_cube, cubeTestDirection(face, row), u_level.x);
}
#endif
