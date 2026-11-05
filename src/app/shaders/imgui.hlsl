// Dear ImGui draw lists (src/app/ImGuiRenderer.cpp): vertices are ImDrawVert (pos float2, uv float2, col RGBA8),
// colors are blended with straight alpha onto the frame's color target.
//
// HDR output (src/app/UiComposite.cpp): ImGui is drawn into a transparent 8-bit layer, which ui_composite_fs puts
// over the frame.

#include "quench/Bindings.hlsli"
#include "quench/Fullscreen.hlsli"
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
    o.color.rgb *= u_imgui.options.y;
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

#ifdef QUENCH_ENTRY_ui_composite_fs
// The UI layer (premultiplied display values, coverage in alpha) over an HDR frame (linear, paper white at
// options.y), blended on display values as on an SDR target, so panels look the same in both. The frame's range
// above paper white is kept, dimmed by the UI's coverage; without UI the frame passes unchanged.
Texture2D<float4> t_frame : QUENCH_TEXTURE(0);
SamplerState s_frame : QUENCH_SAMPLER(0);
Texture2D<float4> t_ui : QUENCH_TEXTURE(1);
SamplerState s_ui : QUENCH_SAMPLER(1);
cbuffer ImGuiBlock : QUENCH_UNIFORM(0) {
    ImGuiUniforms u_imgui;
};

float4 ui_composite_fs(FullscreenOut vin) : SV_Target0 {
    const float3 frame = max(t_frame.Sample(s_frame, vin.uv).rgb, 0.0);
    const float4 ui = t_ui.Sample(s_ui, vin.uv);
    if (ui.a <= 0.0 && all(ui.rgb <= 0.0)) return float4(frame, 1.0);
    const float white = u_imgui.options.y;
    const float3 s = frame / white;
    const float3 display = ui.rgb + (1.0 - ui.a) * pow(saturate(s), ((float3)(1.0 / 2.2)));
    return float4((pow(saturate(display), ((float3)(2.2))) + (1.0 - ui.a) * max(s - 1.0, 0.0)) * white, 1.0);
}
#endif
