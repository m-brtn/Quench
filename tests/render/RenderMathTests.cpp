// The render module's CPU parts: handles, effect fallbacks, cascades, projection, jitter, procedural meshes and the
// daylight palette. No GPU device.

#include "Test.h"
#include "render/Effects.h"
#include "render/SlotMap.h"

#include <quench/render/Cascades.h>
#include <quench/render/Daylight.h>
#include <quench/render/MeshBuilder.h>
#include <quench/render/Projection.h>
#include <quench/render/ToneMap.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

using namespace quench;
using namespace quench::render;

QUENCH_TEST(slot_map_detects_stale_handles) {
    quench::render::detail::SlotMap<MeshId, int> map;
    const MeshId a = map.add(1);
    const MeshId b = map.add(2);
    CHECK(map.size() == 2, "size %u", map.size());
    CHECK(map.get(a) && *map.get(a) == 1, "a");
    CHECK(map.remove(a), "remove a");
    CHECK(!map.get(a), "a is stale after removal");
    CHECK(!map.remove(a), "a removed twice");
    const MeshId c = map.add(3);
    CHECK(c.index == a.index && c.generation != a.generation, "slot reused with a new generation (%u, %u)", c.index,
          c.generation);
    CHECK(!map.get(a) && map.get(c) && *map.get(c) == 3, "the old handle does not reach the new value");
    CHECK(map.get(b) && *map.get(b) == 2, "b unchanged");
    CHECK(!map.get(MeshId{}), "null handle");
    CHECK(!MeshId{} && bool(c), "null converts to false");
}

QUENCH_TEST(effects_fall_back_by_device_support) {
    RenderSettings s;
    s.shadowCascades = 3;
    s.antiAliasing = AntiAliasing::Taa;
    quench::render::detail::DeviceSupport full{.shadowArray = true,
                                               .depthClamp = true,
                                               .sampledDepth = true,
                                               .velocityTarget = true,
                                               .transmittanceArray = true};
    ActiveEffects e = quench::render::detail::selectEffects(s, full);
    CHECK(e.shadowCascades == 3 && e.shadowDepthClamp && e.antiAliasing == AntiAliasing::Taa && e.bloom &&
              e.translucentShadows == TranslucentShadows::Map,
          "full");

    quench::render::detail::DeviceSupport noTransmittance = full;
    noTransmittance.transmittanceArray = false;
    CHECK(quench::render::detail::selectEffects(s, noTransmittance).translucentShadows == TranslucentShadows::Dithered,
          "translucent shadows are dithered without an Rgba16Float texture array target");
    s.translucentShadows = TranslucentShadows::Dithered;
    CHECK(quench::render::detail::selectEffects(s, full).translucentShadows == TranslucentShadows::Dithered,
          "dithered when asked");
    s.translucentShadows = TranslucentShadows::Map;

    quench::render::detail::DeviceSupport noDepthSampling = full;
    noDepthSampling.sampledDepth = false;
    e = quench::render::detail::selectEffects(s, noDepthSampling);
    CHECK(e.antiAliasing == AntiAliasing::Fxaa, "TAA falls back to FXAA without a sampled depth buffer");

    quench::render::detail::DeviceSupport noVelocity = full;
    noVelocity.velocityTarget = false;
    CHECK(quench::render::detail::selectEffects(s, noVelocity).antiAliasing == AntiAliasing::Fxaa,
          "TAA needs Rg16Float");

    quench::render::detail::DeviceSupport noArray = full;
    noArray.shadowArray = false;
    e = quench::render::detail::selectEffects(s, noArray);
    CHECK(e.shadowCascades == 0 && !e.shadowDepthClamp, "no shadows without a depth texture array");

    quench::render::detail::DeviceSupport noClamp = full;
    noClamp.depthClamp = false; // the iOS Simulator
    e = quench::render::detail::selectEffects(s, noClamp);
    CHECK(e.shadowCascades == 3 && !e.shadowDepthClamp, "shadows without depth clamp");

    s.shadowCascades = 9;
    CHECK(quench::render::detail::selectEffects(s, full).shadowCascades == kMaxCascades, "cascades clamped to %d",
          kMaxCascades);
    s.bloom = false;
    s.antiAliasing = AntiAliasing::None;
    e = quench::render::detail::selectEffects(s, full);
    CHECK(!e.bloom && e.antiAliasing == AntiAliasing::None, "off stays off");
}

QUENCH_TEST(cascades_cover_the_view) {
    const glm::mat4 view = glm::lookAt(glm::vec3(3, 2, 8), glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
    const glm::vec3 sun = glm::normalize(glm::vec3(0.4f, 0.8f, 0.3f));
    const float fovY = 1.0f, aspect = 16.0f / 9.0f, nearPlane = 0.1f;
    const CascadeSettings settings{.count = 3, .distance = 60.0f, .mapSize = 1024};
    const Cascades c = computeCascades(view, fovY, aspect, nearPlane, sun, settings);
    REQUIRE(c.count == 3, "count %d", c.count);
    CHECK(c.splits[0] < c.splits[1] && c.splits[1] < c.splits[2], "splits increase: %f %f %f", double(c.splits[0]),
          double(c.splits[1]), double(c.splits[2]));
    CHECK(std::abs(c.splits[2] - 60.0f) < 1e-3f, "last split %f", double(c.splits[2]));
    // Every point of a slice lies inside its cascade (xy in -1..1, depth 0..1).
    const glm::mat4 invView = glm::inverse(view);
    const float tanY = std::tan(fovY * 0.5f), tanX = tanY * aspect;
    float prev = nearPlane;
    for (int i = 0; i < c.count; ++i) {
        for (const float d : {prev, c.splits[std::size_t(i)]})
            for (const float sx : {-1.0f, 1.0f})
                for (const float sy : {-1.0f, 1.0f}) {
                    const glm::vec4 world = invView * glm::vec4(sx * tanX * d, sy * tanY * d, -d, 1.0f);
                    const glm::vec4 p = c.viewProj[std::size_t(i)] * world;
                    // Snapping moves the cascade by up to half a texel.
                    const float edge = 1.0f + 1.0f / 1024.0f;
                    CHECK(std::abs(p.x) <= edge && std::abs(p.y) <= edge && p.z >= 0.0f && p.z <= 1.0f,
                          "cascade %d corner at %f m: %f %f %f", i, double(d), double(p.x), double(p.y), double(p.z));
                }
        CHECK(c.texel[std::size_t(i)] > 0.0f, "texel %d", i);
        prev = c.splits[std::size_t(i)];
    }
    // Texel snapping: moving the camera by a fraction of a texel keeps the world origin on whole texels.
    const glm::vec4 origin = c.viewProj[0] * glm::vec4(0, 0, 0, 1) * 512.0f;
    CHECK(std::abs(origin.x - std::round(origin.x)) < 1e-2f && std::abs(origin.y - std::round(origin.y)) < 1e-2f,
          "origin on the texel grid: %f %f", double(origin.x), double(origin.y));
    CHECK(computeCascades(view, fovY, aspect, nearPlane, sun, {.count = 0}).count == 0, "no cascades");
}

QUENCH_TEST(reversed_perspective_depth) {
    const glm::mat4 p = reversedPerspective(1.0f, 1.5f, 0.1f);
    const auto depth = [&p](float distance) {
        const glm::vec4 c = p * glm::vec4(0.0f, 0.0f, -distance, 1.0f);
        return c.z / c.w;
    };
    CHECK(std::abs(depth(0.1f) - 1.0f) < 1e-6f, "near -> 1: %f", double(depth(0.1f)));
    CHECK(std::abs(depth(0.2f) - 0.5f) < 1e-6f, "2 x near -> 0.5: %f", double(depth(0.2f)));
    CHECK(depth(1e6f) > 0.0f && depth(1e6f) < 1e-6f, "far -> 0: %g", double(depth(1e6f)));
    const glm::mat4 inv = glm::inverse(p);
    CHECK(std::isfinite(inv[0][0]) && std::isfinite(inv[3][2]), "invertible");
}

QUENCH_TEST(subpixel_jitter_cycles) {
    for (std::uint64_t i = 0; i < 16; ++i) {
        const glm::vec2 j = subpixelJitter(i);
        CHECK(std::abs(j.x) <= 0.5f && std::abs(j.y) <= 0.5f, "frame %llu: %f %f", static_cast<unsigned long long>(i),
              double(j.x), double(j.y));
        CHECK(j == subpixelJitter(i + 8), "period 8 at frame %llu", static_cast<unsigned long long>(i));
    }
    CHECK(subpixelJitter(0) != subpixelJitter(1), "frames differ");
}

namespace {

// Every triangle is counter-clockwise seen from outside: its face normal points along the vertex normals.
void checkWinding(const MeshData& mesh, const char* name) {
    REQUIRE(!mesh.vertices.empty() && mesh.indices.size() % 3 == 0, "%s: %zu vertices, %zu indices", name,
            mesh.vertices.size(), mesh.indices.size());
    int wrong = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const Vertex& a = mesh.vertices[mesh.indices[i]];
        const Vertex& b = mesh.vertices[mesh.indices[i + 1]];
        const Vertex& c = mesh.vertices[mesh.indices[i + 2]];
        const glm::vec3 face = glm::cross(b.position - a.position, c.position - a.position);
        // Degenerate at the poles; with FMA contraction the cross product of equal edges is not exactly 0.
        if (glm::length(face) < 1e-6f) continue;
        if (glm::dot(face, a.normal + b.normal + c.normal) <= 0.0f) ++wrong;
    }
    CHECK(wrong == 0, "%s: %d of %zu triangles wound the wrong way", name, wrong, mesh.indices.size() / 3);
}

} // namespace

QUENCH_TEST(mesh_builder_winding) {
    checkWinding(boxMesh(glm::vec3(1.0f, 2.0f, 0.5f)), "box");
    checkWinding(sphereMesh(1.0f, 8, 16), "sphere");
    checkWinding(planeMesh(glm::vec2(2.0f, 3.0f), 4.0f), "plane");
    checkWinding(torusMesh(1.0f, 0.3f, 16, 8), "torus");
    const MeshData box = boxMesh(glm::vec3(1.0f));
    CHECK(box.vertices.size() == 24 && box.indices.size() == 36, "box %zu / %zu", box.vertices.size(),
          box.indices.size());
}

QUENCH_TEST(tone_map_hdr_extends_the_sdr_curve) {
    for (const ToneCurve curve : {ToneCurve::Aces, ToneCurve::Agx}) {
        const char* name = curve == ToneCurve::Aces ? "ACES" : "AgX";
        const HdrKnee knee = hdrKnee(curve);
        CHECK(std::abs(toneCurveScalar(curve, knee.scene) - kHdrKnee) < 1e-4f && knee.slope > 0.0f,
              "%s knee at %f, slope %f", name, double(knee.scene), double(knee.slope));
        float worstSdr = 0.0f, worstBelow = 0.0f;
        for (float s = 1e-3f; s < 1e3f; s *= 1.25f) {
            const glm::vec3 c(s, s * 0.5f, s * 0.1f);
            const glm::vec3 sdr = toneMapSdr(curve, c);
            worstSdr = std::max(worstSdr, glm::length(toneMapHdr(curve, c, 1.0f) - sdr));
            if (s < knee.scene) worstBelow = std::max(worstBelow, glm::length(toneMapHdr(curve, c, 8.0f) - sdr));
        }
        CHECK(worstSdr < 1e-6f, "%s: headroom 1 is the SDR curve (largest difference %g)", name, double(worstSdr));
        CHECK(worstBelow < 1e-6f, "%s: below the knee HDR is the SDR curve (largest difference %g)", name,
              double(worstBelow));
        const float mid = toneMapHdr(curve, glm::vec3(0.18f), 4.0f).g;
        CHECK(std::abs(mid - toneMapSdr(curve, glm::vec3(0.18f)).g) < 1e-6f, "%s: mid grey unchanged (%f)", name,
              double(mid));

        for (const float headroom : {2.0f, 4.0f, 16.0f}) {
            float previous = 0.0f, peak = 0.0f;
            bool monotonic = true, bounded = true;
            for (float s = 1e-3f; s < 1e4f; s *= 1.05f) {
                const float v = toneMapHdr(curve, glm::vec3(s), headroom).g;
                monotonic = monotonic && v >= previous - 1e-6f;
                bounded = bounded && v <= headroom + 1e-5f;
                previous = v;
                peak = std::max(peak, v);
            }
            CHECK(monotonic && bounded, "%s, headroom %g: monotonic %d, at most the headroom %d", name,
                  double(headroom), monotonic, bounded);
            CHECK(peak > headroom * 0.98f, "%s, headroom %g: reaches %f", name, double(headroom), double(peak));
            // C1 at the knee: the slopes on both sides agree.
            const float h = knee.scene * 1e-3f;
            const auto at = [&](float s) {
                const glm::vec3 v = toneMapHdr(curve, glm::vec3(s), headroom);
                return std::max({v.r, v.g, v.b});
            };
            const float below = (at(knee.scene) - at(knee.scene - h)) / h;
            const float above = (at(knee.scene + h) - at(knee.scene)) / h;
            CHECK(std::abs(below - above) < 0.02f * below, "%s, headroom %g: slope %f below the knee, %f above", name,
                  double(headroom), double(below), double(above));
            // A highlight keeps its hue: the channel ratios of the SDR curve.
            const glm::vec3 warm(6.0f, 3.0f, 1.0f);
            const glm::vec3 sdr = toneMapSdr(curve, warm), hdr = toneMapHdr(curve, warm, headroom);
            CHECK(std::abs(hdr.g / hdr.r - sdr.g / sdr.r) < 1e-4f && hdr.r > sdr.r, "%s, headroom %g: %f %f %f", name,
                  double(headroom), double(hdr.r), double(hdr.g), double(hdr.b));
        }
    }
}

QUENCH_TEST(daylight_follows_the_sun) {
    const Daylight noon = daylightFor(skyDirection(60.0f, 0.0f));
    const Daylight low = daylightFor(skyDirection(3.0f, 0.0f));
    const Daylight night = daylightFor(skyDirection(-20.0f, 0.0f));
    CHECK(noon.sunColor.r > 4.0f && noon.environment.exposure == 1.0f, "noon sun %f", double(noon.sunColor.r));
    CHECK(low.sunColor.b < low.sunColor.r * 0.5f, "low sun is warm: %f %f", double(low.sunColor.r),
          double(low.sunColor.b));
    CHECK(night.sunColor == glm::vec3(0.0f) && night.environment.exposure > 2.0f, "night: no sun, more exposure");
    const glm::vec3 d = skyDirection(30.0f, 90.0f);
    CHECK(std::abs(d.y - 0.5f) < 1e-5f && d.x > 0.0f && std::abs(d.z) < 1e-5f, "direction %f %f %f", double(d.x),
          double(d.y), double(d.z));
}
