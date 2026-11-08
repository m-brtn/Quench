#ifndef QUENCH_FRAME_HLSLI
#define QUENCH_FRAME_HLSLI

// The frame uniforms of the render module in uniform slot 0 of the stage being compiled, as `quenchFrame`. Every
// shader drawing in a pass of quench::render::Renderer may read them (the renderer pushes them to slot 0 of both
// stages before its passes and before a game's PassStage callbacks).

#include "quench/Bindings.hlsli"
#include "quench/SceneUniforms.h"

cbuffer QuenchFrameBlock : QUENCH_UNIFORM(0) {
    FrameUniforms quenchFrame;
};

// Unit direction from the camera through a uv of the render target (top left 0, 0). Read at depth 0, the far end of
// the renderer's infinite projection, where invViewProj gives a direction without the camera's translation: a point
// on the near plane minus the camera's position loses the direction's precision away from the world's origin (about
// 2 mrad at 2 km).
float3 quenchViewRay(float2 uv) {
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(mul(quenchFrame.invViewProj, float4(ndc, 0.0, 1.0)).xyz);
}

// View distance of a world position along the camera's forward axis.
float quenchViewDepth(float3 P) { return dot(P - quenchFrame.cameraPos.xyz, quenchFrame.cameraFwd.xyz); }

#endif
