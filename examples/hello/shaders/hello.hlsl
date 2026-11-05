// The hello example: one directional light, a hemisphere ambient term, Blinn-Phong highlights, a checkered ground with
// a blob shadow and distance fog. Lit in linear space, tone mapped, then encoded to sRGB when the target is UNORM.

#include "quench/Bindings.hlsli"
#include "HelloUniforms.h"

cbuffer FrameBlock : QUENCH_UNIFORM(0) {
    HelloFrame u_frame;
};
cbuffer DrawBlock : QUENCH_UNIFORM(1) {
    HelloDraw u_draw;
};

struct VsOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

#ifdef QUENCH_ENTRY_hello_vs
VsOut hello_vs(float3 position : TEXCOORD0, float3 normal : TEXCOORD1) {
    VsOut o;
    const float4 world = mul(u_draw.model, float4(position, 1.0));
    o.pos = mul(u_frame.viewProj, world);
    o.world = world.xyz;
    o.normal = mul(u_draw.model, float4(normal, 0.0)).xyz;
    return o;
}
#endif

#ifdef QUENCH_ENTRY_hello_fs
float3 linearToSrgb(float3 c) {
    return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308));
}

float4 hello_fs(VsOut i) : SV_Target0 {
    const float3 n = normalize(i.normal);
    const bool ground = u_draw.color.w > 0.5;
    float3 albedo = u_draw.color.rgb;
    if (ground) {
        const float2 cell = floor(i.world.xz);
        albedo *= lerp(0.7, 1.0, fmod(abs(cell.x + cell.y), 2.0));
        const float d = length(i.world.xz - u_draw.shadow.xz) / u_draw.shadow.w;
        albedo *= lerp(0.35, 1.0, smoothstep(0.2, 1.0, d));
    }
    const float3 l = u_frame.lightDir.xyz;
    const float3 v = normalize(u_frame.cameraPos.xyz - i.world);
    const float ndl = saturate(dot(n, l));
    const float specular = pow(saturate(dot(n, normalize(l + v))), 64.0) * (ground ? 0.05 : 0.4);
    const float3 ambient = lerp(float3(0.10, 0.09, 0.08), float3(0.30, 0.38, 0.50), n.y * 0.5 + 0.5);
    float3 color = albedo * (ambient + u_frame.lightColor.rgb * ndl) + u_frame.lightColor.rgb * specular * ndl;
    const float fog = 1.0 - exp(-u_frame.fogColor.w * length(u_frame.cameraPos.xyz - i.world));
    color = lerp(color, u_frame.fogColor.rgb, fog);
    color = 1.0 - exp(-color);
    if (u_frame.options.x > 0.5) color = linearToSrgb(color);
    return float4(color, 1.0);
}
#endif
