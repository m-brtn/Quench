// The demo's own render pass (PassStage::Transparent): a glowing disc at every light, depth tested against the scene
// and added to the HDR image, so fog-free bloom and TAA treat it like scene content.

#include "quench/Frame.hlsli"

#include "DemoUniforms.h"

struct MarkerOut {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    float4 color : TEXCOORD1;
};

#if defined(QUENCH_ENTRY_marker_vs)
cbuffer MarkerBlock : QUENCH_UNIFORM(1) { MarkerUniforms markers; }

MarkerOut marker_vs(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
                               float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0)};
    const float2 c = corners[vertex];
    const float4 m = markers.position[instance];
    const float3 world = m.xyz + (markers.right.xyz * c.x + markers.up.xyz * c.y) * m.w;
    MarkerOut o;
    o.position = mul(quenchFrame.viewProj, float4(world, 1.0));
    o.local = c;
    o.color = markers.color[instance];
    return o;
}
#endif

#if defined(QUENCH_ENTRY_marker_fs)
float4 marker_fs(MarkerOut i) : SV_Target0 {
    const float r2 = dot(i.local, i.local);
    if (r2 > 1.0) discard;
    const float glow = (1.0 - r2) * (1.0 - r2);
    return float4(i.color.rgb * glow, 0.0);
}
#endif
