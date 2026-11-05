#pragma once

// The tone curves of Renderer's tone mapping pass in C++ (the shaders use quench/Tonemap.hlsli; the two are kept
// equal and tested against each other): what a scene value becomes on an SDR or an HDR display.
//
// SDR: the curve maps exposed scene values to display values 0..1 (linear here; the pass encodes them for the
// target). HDR (extended linear output): below the knee the result is the SDR curve's, so shadows and mid tones look
// the same on both; above it an exponential shoulder continues to the display's headroom instead of rolling off to 1.
// Headroom 1 gives the SDR curve.

#include <glm/vec3.hpp>

#include <cstdint>

namespace quench::render {

enum class ToneCurve : std::uint8_t { Aces, Agx };

// Linear display value where the HDR shoulder leaves the SDR curve (QUENCH_HDR_KNEE in quench/Tonemap.hlsli).
inline constexpr float kHdrKnee = 0.6f;

// Linear display values 0..1 of the SDR curve for an exposed scene color (exposure applied, linear HDR).
glm::vec3 toneMapSdr(ToneCurve curve, const glm::vec3& color);

// The curve for a grey scene value: the largest channel of toneMapSdr(curve, {value, value, value}).
float toneCurveScalar(ToneCurve curve, float value);

// The scene value (largest channel) where the SDR curve reaches kHdrKnee, and its slope there (display per scene
// unit): the HDR shoulder starts at that point with that slope.
struct HdrKnee {
    float scene = 0.0f;
    float slope = 0.0f;
};
HdrKnee hdrKnee(ToneCurve curve);

// Linear display values for a display with `headroom` (multiples of paper white, >= 1), paper white at 1: equal to
// toneMapSdr while the largest channel is below the knee, at most `headroom`.
glm::vec3 toneMapHdr(ToneCurve curve, const glm::vec3& color, float headroom);

} // namespace quench::render
