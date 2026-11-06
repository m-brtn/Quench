// The passes of quench::render::Renderer around the surface shaders: shadow casters, sky, motion vectors, temporal
// anti-aliasing, bloom, tone mapping and FXAA. Moved from TrailForge (shaders/scene.hlsl at b97883e) and generalized:
// the game's terms (clouds, stars, moon, mountains, lens flare, grading) stay in the game.
//
// Bindings: vertex and fragment uniform slot 0 hold FrameUniforms (quench/Frame.hlsli); the fullscreen passes take
// PostUniforms in fragment slot 1 and their inputs as textures from slot 0.

#include "quench/Bindings.hlsli"
#include "quench/Common.hlsli"
#include "quench/Frame.hlsli"
#include "quench/Fullscreen.hlsli"
#include "quench/SceneUniforms.h"
#include "quench/Sky.hlsli"
#include "quench/Tonemap.hlsli"

#include "PostUniforms.h"

float2 clipToUv(float4 c) {
    const float2 n = c.xy / c.w;
    return float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
}

// ---------------------------------------------------------------- shadow casters

#if defined(QUENCH_ENTRY_shadow_vs) || defined(QUENCH_ENTRY_velocity_obj_vs) || defined(QUENCH_ENTRY_velocity_skin_vs)
cbuffer QuenchDrawBlock : QUENCH_UNIFORM(1) {
    DrawUniforms draw;
};
#endif

#if defined(QUENCH_ENTRY_shadow_vs)
StructuredBuffer<InstanceData> instances : QUENCH_STORAGE(0);

float4 shadow_vs(float3 position : TEXCOORD0, uint instance : SV_InstanceID) : SV_Position {
    const InstanceData inst = instances[uint(draw.info.x) + instance];
    return mul(quenchFrame.shadowViewProj[int(draw.info.y)], mul(inst.model, float4(position, 1.0)));
}
#endif

#if defined(QUENCH_ENTRY_shadow_fs)
void shadow_fs() {}
#endif

// ---------------------------------------------------------------- sky

#if defined(QUENCH_ENTRY_sky_fs)
// Behind everything: drawn with a GreaterOrEqual test at depth 0 after the opaque surfaces.
float4 sky_fs(FullscreenOut vin) : SV_Target0 {
    const float3 dir = quenchViewRay(vin.uv);
    float3 col = quenchSunDisc(skyRadiance(dir), dir);
    // Horizon haze matches the fog color.
    if (quenchFrame.fogParams.x > 0.0) {
        const float haze = pow(1.0 - saturate(abs(dir.y)), 6.0) * 0.6 * quenchFrame.fogColor.w;
        col = lerp(col, hazeColor(dir), haze);
    }
    return float4(col, 1.0);
}
#endif

// ---------------------------------------------------------------- fullscreen passes

#if defined(QUENCH_ENTRY_velocity_fs) || defined(QUENCH_ENTRY_taa_fs) || defined(QUENCH_ENTRY_bloom_down_fs) || \
    defined(QUENCH_ENTRY_bloom_up_fs) || defined(QUENCH_ENTRY_tonemap_fs) || defined(QUENCH_ENTRY_fxaa_fs)
cbuffer PostBlock : QUENCH_UNIFORM(1) {
    PostUniforms post;
};
#endif

#if defined(QUENCH_ENTRY_velocity_fs)
Texture2D<float> depthTex : QUENCH_TEXTURE(0);
SamplerState depthSampler : QUENCH_SAMPLER(0);

// Camera motion: screen offset since the previous frame of the point seen at this pixel (uv units). Points at
// infinity (the sky) stay homogeneous, so only the camera's rotation moves them.
float4 velocity_fs(FullscreenOut vin) : SV_Target0 {
    const float d = depthTex.Load(int3(vin.position.xy, 0));
    const float2 ndc = float2(vin.uv.x * 2.0 - 1.0, 1.0 - vin.uv.y * 2.0);
    const float4 w = mul(quenchFrame.invViewProj, float4(ndc, d, 1.0));
    const float2 v = clipToUv(mul(quenchFrame.viewProjStable, w)) - clipToUv(mul(quenchFrame.prevViewProj, w));
    return float4(v, 0.0, 1.0);
}
#endif

struct VelocityOut {
    float4 position : SV_Position;
    float4 cur : TEXCOORD0;
    float4 prev : TEXCOORD1;
};

#if defined(QUENCH_ENTRY_velocity_obj_vs)
StructuredBuffer<VelocityInstance> moved : QUENCH_STORAGE(0);

// Instances that moved since the previous frame, over the camera velocity, with their previous transform.
VelocityOut velocity_obj_vs(float3 position : TEXCOORD0, uint instance : SV_InstanceID) {
    const VelocityInstance inst = moved[uint(draw.info.x) + instance];
    VelocityOut o;
    const float4 world = mul(inst.model, float4(position, 1.0));
    o.position = mul(quenchFrame.viewProj, world); // jittered, as the depth it is tested against
    o.cur = mul(quenchFrame.viewProjStable, world);
    o.prev = mul(quenchFrame.prevViewProj, mul(inst.prevModel, float4(position, 1.0)));
    return o;
}
#endif

#if defined(QUENCH_ENTRY_velocity_skin_vs)
StructuredBuffer<VelocityInstance> moved : QUENCH_STORAGE(0);

// Skinned instances: the previous frame's skinned position comes from a second vertex buffer.
VelocityOut velocity_skin_vs(float3 position : TEXCOORD0, float3 prevPosition : TEXCOORD1,
                             uint instance : SV_InstanceID) {
    const VelocityInstance inst = moved[uint(draw.info.x) + instance];
    VelocityOut o;
    const float4 world = mul(inst.model, float4(position, 1.0));
    o.position = mul(quenchFrame.viewProj, world);
    o.cur = mul(quenchFrame.viewProjStable, world);
    o.prev = mul(quenchFrame.prevViewProj, mul(inst.prevModel, float4(prevPosition, 1.0)));
    return o;
}
#endif

#if defined(QUENCH_ENTRY_velocity_obj_fs)
float4 velocity_obj_fs(VelocityOut vin) : SV_Target0 { return float4(clipToUv(vin.cur) - clipToUv(vin.prev), 0.0, 1.0); }
#endif

#if defined(QUENCH_ENTRY_taa_fs)
Texture2D<float4> current : QUENCH_TEXTURE(0);
SamplerState currentSampler : QUENCH_SAMPLER(0);
Texture2D<float4> history : QUENCH_TEXTURE(1);
SamplerState historySampler : QUENCH_SAMPLER(1);
Texture2D<float4> velocityTex : QUENCH_TEXTURE(2);
SamplerState velocitySampler : QUENCH_SAMPLER(2);
Texture2D<float> depthTex : QUENCH_TEXTURE(3);
SamplerState depthSampler : QUENCH_SAMPLER(3);

float3 rgbToYCoCg(float3 c) {
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}
float3 yCoCgToRgb(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Catmull-Rom history fetch with 5 bilinear taps.
float3 historyCatmullRom(float2 uv, float2 size) {
    const float2 sp = uv * size;
    const float2 tc1 = floor(sp - 0.5) + 0.5;
    const float2 fr = sp - tc1;
    const float2 w0 = fr * (-0.5 + fr * (1.0 - 0.5 * fr));
    const float2 w1 = 1.0 + fr * fr * (-2.5 + 1.5 * fr);
    const float2 w2 = fr * (0.5 + fr * (2.0 - 1.5 * fr));
    const float2 w3 = fr * fr * (-0.5 + 0.5 * fr);
    const float2 w12 = w1 + w2;
    const float2 tc0 = (tc1 - 1.0) / size, tc3 = (tc1 + 2.0) / size, tc12 = (tc1 + w2 / w12) / size;
    const float a = w12.x * w0.y, b = w0.x * w12.y, c = w12.x * w12.y, d = w3.x * w12.y, e = w12.x * w3.y;
    const float3 r = history.SampleLevel(historySampler, float2(tc12.x, tc0.y), 0).rgb * a +
                     history.SampleLevel(historySampler, float2(tc0.x, tc12.y), 0).rgb * b +
                     history.SampleLevel(historySampler, tc12, 0).rgb * c +
                     history.SampleLevel(historySampler, float2(tc3.x, tc12.y), 0).rgb * d +
                     history.SampleLevel(historySampler, float2(tc12.x, tc3.y), 0).rgb * e;
    return max(r / (a + b + c + d + e), 0.0);
}

// The jittered current frame blended into the reprojected history, clipped to the variance box of the 3 x 3
// neighbourhood (YCoCg, tone-mapped domain). post.params x: 1 = reset the history, y: weight of the current frame.
float4 taa_fs(FullscreenOut vin) : SV_Target0 {
    const float2 uv = vin.uv;
    const float2 t = post.texel.xy;
    const int2 pixel = int2(vin.position.xy);
    const int2 last = int2(post.texel.zw) - 1;
    // Velocity of the nearest surface in a cross (keeps the edges of moving objects from trailing); reversed depth:
    // the nearest has the largest value.
    int2 best = pixel;
    float nearest = depthTex.Load(int3(pixel, 0));
    const int2 offs[4] = {int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1)};
    for (int i = 0; i < 4; ++i) {
        const int2 o = clamp(pixel + offs[i], int2(0, 0), last);
        const float d = depthTex.Load(int3(o, 0));
        if (d > nearest) {
            nearest = d;
            best = o;
        }
    }
    const float2 vel = velocityTex.Load(int3(best, 0)).xy;
    float3 m1 = ((float3)(0.0)), m2 = ((float3)(0.0));
    float3 center = ((float3)(0.0));
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float3 c = rgbToYCoCg(tmap(current.SampleLevel(currentSampler, uv + float2(x, y) * t, 0).rgb));
            if (x == 0 && y == 0) center = c;
            m1 += c;
            m2 += c * c;
        }
    }
    const float2 huv = uv - vel;
    if (post.params.x > 0.5 || any(huv < 0.0) || any(huv > 1.0)) return float4(itmap(yCoCgToRgb(center)), 1.0);
    const float3 mu = m1 / 9.0;
    const float3 sigma = sqrt(abs(m2 / 9.0 - mu * mu));
    const float3 bmin = mu - sigma * 1.1, bmax = mu + sigma * 1.1;
    float3 h = rgbToYCoCg(tmap(historyCatmullRom(huv, 1.0 / t)));
    // Clip toward the box centre (not a per-channel clamp: keeps the hue).
    const float3 hc = 0.5 * (bmax + bmin), he = 0.5 * (bmax - bmin) + 1e-5;
    const float3 dv = (h - hc) / he;
    const float ma = max(abs(dv.x), max(abs(dv.y), abs(dv.z)));
    if (ma > 1.0) h = hc + (h - hc) / ma;
    const float speed = length(vel / t);
    const float a = lerp(post.params.y, 0.3, saturate(speed / 40.0));
    return float4(max(itmap(yCoCgToRgb(lerp(h, center, a))), 0.0), 1.0);
}
#endif

#if defined(QUENCH_ENTRY_bloom_down_fs) || defined(QUENCH_ENTRY_bloom_up_fs) || defined(QUENCH_ENTRY_tonemap_fs) || \
    defined(QUENCH_ENTRY_fxaa_fs)
Texture2D<float4> src : QUENCH_TEXTURE(0);
SamplerState srcSampler : QUENCH_SAMPLER(0);
#endif

#if defined(QUENCH_ENTRY_bloom_down_fs)
float3 karisWeight(float3 c) { return c / (1.0 + luminance(c)); }

// 13-tap downsample (Jimenez 2014). post.params x: threshold, w: 1 on the first pass (Karis average against
// fireflies, then a soft threshold).
float4 bloom_down_fs(FullscreenOut vin) : SV_Target0 {
    const float2 uv = vin.uv;
    const float2 t = post.texel.xy;
    const float3 a = src.Sample(srcSampler, uv + t * float2(-2, -2)).rgb;
    const float3 b = src.Sample(srcSampler, uv + t * float2(0, -2)).rgb;
    const float3 c = src.Sample(srcSampler, uv + t * float2(2, -2)).rgb;
    const float3 d = src.Sample(srcSampler, uv + t * float2(-1, -1)).rgb;
    const float3 e = src.Sample(srcSampler, uv + t * float2(1, -1)).rgb;
    const float3 f0 = src.Sample(srcSampler, uv + t * float2(-2, 0)).rgb;
    const float3 g = src.Sample(srcSampler, uv).rgb;
    const float3 h = src.Sample(srcSampler, uv + t * float2(2, 0)).rgb;
    const float3 i = src.Sample(srcSampler, uv + t * float2(-1, 1)).rgb;
    const float3 j = src.Sample(srcSampler, uv + t * float2(1, 1)).rgb;
    const float3 k = src.Sample(srcSampler, uv + t * float2(-2, 2)).rgb;
    const float3 l = src.Sample(srcSampler, uv + t * float2(0, 2)).rgb;
    const float3 m = src.Sample(srcSampler, uv + t * float2(2, 2)).rgb;
    float3 col;
    if (post.params.w > 0.5) {
        col = karisWeight((d + e + i + j) * 0.25) * 0.5 + karisWeight((a + b + f0 + g) * 0.25) * 0.125 +
              karisWeight((b + c + g + h) * 0.25) * 0.125 + karisWeight((f0 + g + k + l) * 0.25) * 0.125 +
              karisWeight((g + h + l + m) * 0.25) * 0.125;
        col = col / max(1.0 - luminance(col), 1e-3);
        const float br = max(col.r, max(col.g, col.b));
        const float knee = post.params.x * 0.5;
        float soft = clamp(br - post.params.x + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-4);
        col *= max(soft, br - post.params.x) / max(br, 1e-4);
    } else {
        col = (d + e + i + j) * 0.125 + (a + c + k + m) * 0.03125 + (b + f0 + h + l) * 0.0625 + g * 0.125;
    }
    return float4(col, 1.0);
}
#endif

#if defined(QUENCH_ENTRY_bloom_up_fs)
// 3 x 3 tent upsample, added onto the larger level by blending.
float4 bloom_up_fs(FullscreenOut vin) : SV_Target0 {
    const float2 t = post.texel.xy;
    const float2 uv = vin.uv;
    float3 c = src.Sample(srcSampler, uv).rgb * 4.0;
    c += (src.Sample(srcSampler, uv + float2(-t.x, 0)).rgb + src.Sample(srcSampler, uv + float2(t.x, 0)).rgb +
          src.Sample(srcSampler, uv + float2(0, -t.y)).rgb + src.Sample(srcSampler, uv + float2(0, t.y)).rgb) *
         2.0;
    c += src.Sample(srcSampler, uv + float2(-t.x, -t.y)).rgb + src.Sample(srcSampler, uv + float2(t.x, -t.y)).rgb +
         src.Sample(srcSampler, uv + float2(-t.x, t.y)).rgb + src.Sample(srcSampler, uv + float2(t.x, t.y)).rgb;
    return float4(c / 16.0, 1.0);
}
#endif

// Display values to what the target stores. mode 0: UNORM targets, display values (gamma 2.2) unchanged; 1: sRGB
// targets, linear (the hardware encodes them again); 2: HDR extended linear, the values are linear already.
float3 quenchToTarget(float3 display, float mode) {
    if (mode > 1.5) return display;
    return mode > 0.5 ? pow(saturate(display), ((float3)(2.2))) : display;
}

#if defined(QUENCH_ENTRY_tonemap_fs)
Texture2D<float4> bloomTex : QUENCH_TEXTURE(1);
SamplerState bloomSampler : QUENCH_SAMPLER(1);

// post.params x: sharpening, y: bloom strength, z: exposure, w: curve (0 ACES, 1 AgX); post.params2 x: vignette,
// y: 1 = write luma to alpha (FXAA follows), z: target mode (quenchToTarget), w: dither; post.params3 (HDR output,
// x > 0): x headroom, y paper white, z and w the knee of the HDR shoulder (scene value, slope).
float4 tonemap_fs(FullscreenOut vin) : SV_Target0 {
    const float2 uv = vin.uv;
    float3 c = src.Sample(srcSampler, uv).rgb;
    if (post.params.x > 0.0) {
        // Sharpening after TAA (unsharp mask on the 4 neighbours, in the tone-mapped domain).
        const float2 t = post.texel.xy;
        const float3 n = tmap(src.Sample(srcSampler, uv + float2(0, -t.y)).rgb) +
                         tmap(src.Sample(srcSampler, uv + float2(0, t.y)).rgb) +
                         tmap(src.Sample(srcSampler, uv + float2(-t.x, 0)).rgb) +
                         tmap(src.Sample(srcSampler, uv + float2(t.x, 0)).rgb);
        const float3 tc = tmap(c);
        c = itmap(clamp(tc + (tc - n * 0.25) * post.params.x, 0.0, 0.999));
    }
    if (post.params.y > 0.0) c += bloomTex.Sample(bloomSampler, uv).rgb * post.params.y;
    // The vignette darkens the scene before the curve, as a lens does: after it, the white point would fall toward the
    // edges and a clipped sun near the top would show darker than the less darkened sky below it.
    const float2 q = uv - 0.5;
    c *= post.params.z * saturate(1.0 - post.params2.x * dot(q, q) * 1.6);
    if (post.params3.x > 0.0) {
        // HDR: linear values, paper white at post.params3.y. FXAA finds edges on display values clipped at paper white.
        const float3 hdr =
            quenchToneMapHdr(c, post.params.w, post.params3.x, post.params3.z, post.params3.w) * post.params3.y;
        const float luma = luminance(pow(saturate(hdr / post.params3.y), ((float3)(1.0 / 2.2))));
        return float4(hdr, post.params2.y > 0.5 ? luma : 1.0);
    }
    float3 display;
    if (post.params.w > 0.5)
        display = agx(c);
    else
        display = pow(aces(c * QUENCH_ACES_PRE_EXPOSURE), ((float3)(1.0 / 2.2)));
    // Dither against banding in the sky.
    display += (hash12(vin.position.xy) - 0.5) / 255.0 * post.params2.w;
    if (post.params2.y > 0.5) return float4(saturate(display), luminance(saturate(display)));
    return float4(quenchToTarget(display, post.params2.z), 1.0);
}
#endif

#if defined(QUENCH_ENTRY_fxaa_fs)
// FXAA (simplified 3.11 "quality" variant with 5 edge steps) on display values with luma in alpha (HDR: linear
// values, luma of the values clipped at paper white). post.params2 z: target mode (quenchToTarget).
float4 fxaa_fs(FullscreenOut vin) : SV_Target0 {
    const float2 uv = vin.uv;
    const float2 t = post.texel.xy;
    const float4 center = src.Sample(srcSampler, uv);
    const float lM = center.a;
    const float lN = src.Sample(srcSampler, uv + float2(0, -t.y)).a, lS = src.Sample(srcSampler, uv + float2(0, t.y)).a;
    const float lE = src.Sample(srcSampler, uv + float2(t.x, 0)).a, lW = src.Sample(srcSampler, uv + float2(-t.x, 0)).a;
    const float lMax = max(lM, max(max(lN, lS), max(lE, lW))), lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    const float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) return float4(quenchToTarget(center.rgb, post.params2.z), 1.0);
    const float lNW = src.Sample(srcSampler, uv + float2(-t.x, -t.y)).a;
    const float lNE = src.Sample(srcSampler, uv + float2(t.x, -t.y)).a;
    const float lSW = src.Sample(srcSampler, uv + float2(-t.x, t.y)).a;
    const float lSE = src.Sample(srcSampler, uv + float2(t.x, t.y)).a;
    const float edgeH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    const float edgeV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    const bool horizontal = edgeH >= edgeV;
    const float l1 = horizontal ? lN : lW, l2 = horizontal ? lS : lE;
    const float g1 = abs(l1 - lM), g2 = abs(l2 - lM);
    const bool steep1 = g1 >= g2;
    float stepLen = horizontal ? t.y : t.x;
    float lLocal;
    if (steep1) {
        stepLen = -stepLen;
        lLocal = 0.5 * (l1 + lM);
    } else {
        lLocal = 0.5 * (l2 + lM);
    }
    const float gScaled = 0.25 * max(g1, g2);
    float2 cuv = uv;
    if (horizontal)
        cuv.y += stepLen * 0.5;
    else
        cuv.x += stepLen * 0.5;
    const float2 off = horizontal ? float2(t.x, 0) : float2(0, t.y);
    float2 uv1 = cuv - off, uv2 = cuv + off;
    float e1 = src.Sample(srcSampler, uv1).a - lLocal, e2 = src.Sample(srcSampler, uv2).a - lLocal;
    bool d1 = abs(e1) >= gScaled, d2 = abs(e2) >= gScaled;
    const float steps[5] = {1.5, 2.0, 2.0, 4.0, 8.0};
    for (int i = 0; i < 5 && !(d1 && d2); ++i) {
        if (!d1) {
            uv1 -= off * steps[i];
            e1 = src.Sample(srcSampler, uv1).a - lLocal;
            d1 = abs(e1) >= gScaled;
        }
        if (!d2) {
            uv2 += off * steps[i];
            e2 = src.Sample(srcSampler, uv2).a - lLocal;
            d2 = abs(e2) >= gScaled;
        }
    }
    const float dist1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
    const float dist2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
    const bool closer1 = dist1 < dist2;
    const float dmin = min(dist1, dist2);
    const float edgeLen = dist1 + dist2;
    const bool correct = ((closer1 ? e1 : e2) < 0.0) != (lM < lLocal);
    float pixelOffset = correct ? -dmin / edgeLen + 0.5 : 0.0;
    // Sub-pixel aliasing.
    const float lAvg = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;
    const float sub = saturate(abs(lAvg - lM) / range);
    const float sub2 = (-2.0 * sub + 3.0) * sub * sub;
    pixelOffset = max(pixelOffset, sub2 * sub2 * 0.75);
    float2 fuv = uv;
    if (horizontal)
        fuv.y += pixelOffset * stepLen;
    else
        fuv.x += pixelOffset * stepLen;
    return float4(quenchToTarget(src.Sample(srcSampler, fuv).rgb, post.params2.z), 1.0);
}
#endif
