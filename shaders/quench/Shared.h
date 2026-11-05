#ifndef QUENCH_SHARED_H
#define QUENCH_SHARED_H

// Read by C++ and HLSL: uniform and storage structs are defined once, in headers that include this one. Members are
// float4 and float4x4 only, so the HLSL, std140/std430 and MSL layouts agree with C++. Matrices are column-major
// (glm); shaders multiply with mul(M, v).
//
//   QUENCH_SHARED_BEGIN
//   struct QUENCH_ALIGN16 DrawUniforms {
//       float4x4 model;
//       float4 tint;
//   };
//   QUENCH_SHARED_END
//
// C++ sees quench::shader::DrawUniforms with glm types; HLSL sees a plain struct to put into a cbuffer.

#ifdef __cplusplus
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace quench::shader {
using float4 = glm::vec4;
using float4x4 = glm::mat4;
} // namespace quench::shader

#define QUENCH_SHARED_BEGIN namespace quench::shader {
#define QUENCH_SHARED_END }
#define QUENCH_ALIGN16 alignas(16)
#else
#define QUENCH_SHARED_BEGIN
#define QUENCH_SHARED_END
#define QUENCH_ALIGN16
#endif

#endif
