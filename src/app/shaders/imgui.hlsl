// Dear ImGui draw lists (src/app/ImGuiRenderer.cpp): vertices are ImDrawVert (pos float2, uv float2, col RGBA8),
// colors are blended with straight alpha onto the frame's color target.

#include "quench/Bindings.hlsli"
#include "ImGuiUniforms.h"

struct ImGuiVsOut {
    float4 pos : SV_Position;
    float4 color : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

#ifdef QUENCH_ENTRY_imgui_vs
cbuffer ImGuiBlock : QUENCH_UNIFORM(0) {
    ImGuiUniforms u_imgui;
};

float3 srgbToLinear(float3 c) {
    return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045));
}

ImGuiVsOut imgui_vs(float2 pos : TEXCOORD0, float2 uv : TEXCOORD1, float4 color : TEXCOORD2) {
    ImGuiVsOut o;
    o.pos = float4(pos * u_imgui.transform.xy + u_imgui.transform.zw, 0.0, 1.0);
    o.color = u_imgui.options.x > 0.5 ? float4(srgbToLinear(color.rgb), color.a) : color;
    o.uv = uv;
    return o;
}
#endif

#ifdef QUENCH_ENTRY_imgui_fs
Texture2D<float4> t_texture : QUENCH_TEXTURE(0);
SamplerState s_texture : QUENCH_SAMPLER(0);

float4 imgui_fs(ImGuiVsOut i) : SV_Target0 {
    return i.color * t_texture.Sample(s_texture, i.uv);
}
#endif
