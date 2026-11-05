#ifndef QUENCH_IMGUI_UNIFORMS_H
#define QUENCH_IMGUI_UNIFORMS_H

#include "quench/Shared.h"

QUENCH_SHARED_BEGIN

struct QUENCH_ALIGN16 ImGuiUniforms {
    float4 transform; // xy: scale, zw: offset from ImGui display coordinates to clip space
    float4 options;   // x: 1 when the target takes linear values (sRGB, HDR); y: white (vertex colors are scaled by it)
};

QUENCH_SHARED_END

#endif
