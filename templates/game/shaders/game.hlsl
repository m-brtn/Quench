// One directional light and a constant ambient term; the ground draws a 1 m grid. Lit in linear space, encoded to sRGB
// when the target is UNORM.

#include "quench/Bindings.hlsli"
#include "GameUniforms.h"

cbuffer FrameBlock : QUENCH_UNIFORM(0) {
    GameFrame u_frame;
};
cbuffer DrawBlock : QUENCH_UNIFORM(1) {
    GameDraw u_draw;
};

struct VsOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

#ifdef QUENCH_ENTRY_game_vs
VsOut game_vs(float3 position : TEXCOORD0, float3 normal : TEXCOORD1) {
    VsOut o;
    const float4 world = mul(u_draw.model, float4(position, 1.0));
    o.pos = mul(u_frame.viewProj, world);
    o.world = world.xyz;
    o.normal = mul(u_draw.model, float4(normal, 0.0)).xyz;
    return o;
}
#endif

#ifdef QUENCH_ENTRY_game_fs
float3 linearToSrgb(float3 c) {
    return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308));
}

float4 game_fs(VsOut i) : SV_Target0 {
    float3 albedo = u_draw.color.rgb;
    if (u_draw.color.w > 0.5) {
        const float2 cell = abs(frac(i.world.xz) - 0.5);
        const float edge = 0.5 - max(cell.x, cell.y); // distance to the nearest grid line (m)
        albedo *= lerp(0.55, 1.0, smoothstep(0.01, 0.03, edge));
    }
    const float ndl = saturate(dot(normalize(i.normal), u_frame.lightDir.xyz));
    float3 color = albedo * (u_frame.ambient.rgb + u_frame.lightColor.rgb * ndl);
    color = 1.0 - exp(-color);
    if (u_frame.options.x > 0.5) color = linearToSrgb(color);
    return float4(color, 1.0);
}
#endif
