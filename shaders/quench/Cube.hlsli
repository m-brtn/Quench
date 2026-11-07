#pragma once

// Cube textures drawn face by face: the direction a texel of a face stands for, as the hardware samples it. Faces in
// layer order +x, -x, +y, -y, +z, -z; uv (0, 0) at the top left of the face (quench/Fullscreen.hlsli's uv). One table
// for every backend: D3D, Metal and Vulkan share the convention (GPU device test cube_faces).

float3 quenchCubeDirection(int face, float2 uv) {
    const float u = uv.x * 2.0 - 1.0;
    const float v = uv.y * 2.0 - 1.0;
    float3 d;
    if (face == 0)
        d = float3(1.0, -v, -u);
    else if (face == 1)
        d = float3(-1.0, -v, u);
    else if (face == 2)
        d = float3(u, 1.0, v);
    else if (face == 3)
        d = float3(u, -1.0, -v);
    else if (face == 4)
        d = float3(u, -v, 1.0);
    else
        d = float3(-u, -v, -1.0);
    return normalize(d);
}
