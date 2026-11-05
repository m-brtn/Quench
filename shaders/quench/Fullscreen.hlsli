#ifndef QUENCH_FULLSCREEN_HLSLI
#define QUENCH_FULLSCREEN_HLSLI

// A triangle covering the target, drawn with draw(3) and no vertex buffer. fullscreen_vs was identical in TrailForge
// and Ashfall. uv runs from (0, 0) at the top left to (1, 1) at the bottom right; z = 0 is the far plane of the
// reversed depth, so with a GreaterOrEqual depth test the triangle covers only what nothing else covered (the sky).

struct FullscreenOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

#if defined(QUENCH_ENTRY_fullscreen_vs)
FullscreenOut fullscreen_vs(uint vid : SV_VertexID) {
    FullscreenOut vout;
    const float2 p = float2((vid << 1) & 2, vid & 2);
    vout.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    vout.uv = float2(p.x, 1.0 - p.y);
    return vout;
}
#endif

#endif
