// Shader set of the shader hot reload case (gpu_device.shader_hot_reload): the case edits copies of this file and of
// the file it includes, and reads the colour after each reload.

#include "reload_color.hlsli"

struct ReloadOut {
    float4 pos : SV_Position;
};

// One triangle that covers the target, without vertex buffers.
#ifdef QUENCH_ENTRY_reload_vs
ReloadOut reload_vs(uint id : SV_VertexID) {
    ReloadOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
#endif

#ifdef QUENCH_ENTRY_reload_fs
float4 reload_fs(ReloadOut i) : SV_Target0 {
    return float4(RELOAD_COLOR, 1.0);
}
#endif
