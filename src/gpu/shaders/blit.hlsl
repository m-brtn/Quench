// CommandBuffer::blit on the native Metal backend (src/gpu/metal/MetalCommands.mm): a triangle over the destination
// region sampling the source region, the same arithmetic as SDL GPU's blit shaders (SDL's Metal_Blit.metal).

#include "quench/Bindings.hlsli"
#include "quench/Fullscreen.hlsli"
#include "BlitUniforms.h"

#if defined(QUENCH_ENTRY_blit_2d_fs) || defined(QUENCH_ENTRY_blit_2d_array_fs)
cbuffer BlitBlock : QUENCH_UNIFORM(0) {
    BlitUniforms u_blit;
};
SamplerState s_source : QUENCH_SAMPLER(0);
#endif

#ifdef QUENCH_ENTRY_blit_2d_fs
Texture2D<float4> t_source : QUENCH_TEXTURE(0);

float4 blit_2d_fs(FullscreenOut vin) : SV_Target0 {
    const float2 uv = u_blit.region.xy + u_blit.region.zw * vin.uv;
    return t_source.SampleLevel(s_source, uv, u_blit.source.x);
}
#endif

#ifdef QUENCH_ENTRY_blit_2d_array_fs
Texture2DArray<float4> t_source : QUENCH_TEXTURE(0);

float4 blit_2d_array_fs(FullscreenOut vin) : SV_Target0 {
    const float2 uv = u_blit.region.xy + u_blit.region.zw * vin.uv;
    return t_source.SampleLevel(s_source, float3(uv, u_blit.source.y), u_blit.source.x);
}
#endif
