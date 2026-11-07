// The demo's debug geometry (lines and triangles with a color per vertex: the navigation section's navmesh, paths and
// agents) in a game pass (PassStage::Transparent): depth tested against the scene, not written, blended over it. Each
// vertex is tested a little closer to the camera than it is, so that a navmesh polygon over stairs (a flat slope
// through the steps) still shows. The colors are display values: divided by the frame's exposure, they look the same
// under any light.

#include "quench/Frame.hlsli"

struct DebugOut {
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

#if defined(QUENCH_ENTRY_debug_vs)
DebugOut debug_vs(float3 position : TEXCOORD0, float4 color : TEXCOORD1) {
    // Along the ray to the camera: the same place on the screen, nearer in depth.
    const float3 toCamera = quenchFrame.cameraPos.xyz - position;
    const float3 nearer = position + toCamera * min(0.3 / max(length(toCamera), 1e-3), 0.5);
    DebugOut o;
    o.position = mul(quenchFrame.viewProj, float4(nearer, 1.0));
    o.color = color;
    return o;
}
#endif

#if defined(QUENCH_ENTRY_debug_fs)
float4 debug_fs(DebugOut i) : SV_Target0 {
    return float4(i.color.rgb / max(quenchFrame.options.x, 1e-6), i.color.a);
}
#endif
