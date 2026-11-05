#include <quench/render/ToneMap.h>

#include <glm/common.hpp>
#include <glm/exponential.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <cmath>

namespace quench::render {
namespace {

constexpr float kAcesPreExposure = 0.6f; // QUENCH_ACES_PRE_EXPOSURE

float maxChannel(const glm::vec3& c) { return std::max(c.r, std::max(c.g, c.b)); }

float luminance(const glm::vec3& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

glm::vec3 aces(const glm::vec3& x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return glm::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

glm::vec3 agxContrast(const glm::vec3& x) {
    const glm::vec3 x2 = x * x, x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

// Encoded display values (about gamma 2.2), as agx() in quench/Tonemap.hlsli. HLSL's mul(v, M) with the float3x3
// written row by row is glm's M * v with the same numbers (glm's constructor takes columns).
glm::vec3 agx(const glm::vec3& c) {
    const glm::mat3 inset(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f, 0.0784335999999992f,
                          0.878468636469772f, 0.0784336f, 0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
    const glm::mat3 outset(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f, -0.0980208811401368f,
                           1.15190312990417f, -0.0980434501171241f, -0.0990297440797205f, -0.0989611768448433f,
                           1.15107367264116f);
    const float minEv = -12.47393f, maxEv = 4.026069f;
    glm::vec3 v = inset * glm::max(c, glm::vec3(1e-10f));
    v = (glm::clamp(glm::log2(v), minEv, maxEv) - minEv) / (maxEv - minEv);
    v = glm::clamp(agxContrast(v), 0.0f, 1.0f);
    v = glm::pow(v, glm::vec3(1.35f));
    const float l = luminance(v);
    v = l + 1.4f * (v - l);
    return glm::clamp(outset * v, 0.0f, 1.0f);
}

HdrKnee findKnee(ToneCurve curve) {
    // The scalar curves rise monotonically: bisection in log space.
    float lo = 1e-4f, hi = 1e4f;
    for (int i = 0; i < 80; ++i) {
        const float mid = std::sqrt(lo * hi);
        (toneCurveScalar(curve, mid) < kHdrKnee ? lo : hi) = mid;
    }
    const float scene = std::sqrt(lo * hi);
    const float h = scene * 1e-3f;
    const float slope = (toneCurveScalar(curve, scene + h) - toneCurveScalar(curve, scene - h)) / (2.0f * h);
    return {scene, slope};
}

} // namespace

glm::vec3 toneMapSdr(ToneCurve curve, const glm::vec3& color) {
    if (curve == ToneCurve::Agx) return glm::pow(agx(color), glm::vec3(2.2f));
    return aces(color * kAcesPreExposure);
}

float toneCurveScalar(ToneCurve curve, float value) { return maxChannel(toneMapSdr(curve, glm::vec3(value))); }

HdrKnee hdrKnee(ToneCurve curve) {
    static const HdrKnee aces = findKnee(ToneCurve::Aces);
    static const HdrKnee agxKnee = findKnee(ToneCurve::Agx);
    return curve == ToneCurve::Agx ? agxKnee : aces;
}

glm::vec3 toneMapHdr(ToneCurve curve, const glm::vec3& color, float headroom) {
    const glm::vec3 sdr = toneMapSdr(curve, color);
    const float s = maxChannel(color);
    const HdrKnee knee = hdrKnee(curve);
    if (headroom <= 1.0f || s <= knee.scene) return sdr;
    const float room = headroom - kHdrKnee;
    const float target = kHdrKnee + room * (1.0f - std::exp(-knee.slope * (s - knee.scene) / room));
    return glm::min(sdr * (target / std::max(toneCurveScalar(curve, s), 1e-4f)), glm::vec3(headroom));
}

} // namespace quench::render
