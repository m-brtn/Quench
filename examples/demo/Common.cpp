#include "Demo.h"

#include <quench/core/Parse.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace demo {

using namespace quench;

glm::vec3 OrbitCamera::eye() const {
    const float c = std::cos(pitch);
    return target + distance * glm::vec3(std::sin(yaw) * c, std::sin(pitch), std::cos(yaw) * c);
}

glm::mat4 OrbitCamera::view() const { return glm::lookAt(eye(), target, glm::vec3(0.0f, 1.0f, 0.0f)); }

float OrbitCamera::fovFor(float aspect) const {
    return std::max(fovY, 2.0f * std::atan(std::tan(glm::radians(32.0f)) / aspect));
}

void OrbitCamera::ray(glm::vec2 point, glm::vec2 window, glm::vec3& origin, glm::vec3& direction) const {
    const float aspect = window.x / std::max(window.y, 1.0f);
    const float t = std::tan(fovFor(aspect) * 0.5f);
    const glm::vec2 ndc(point.x / window.x * 2.0f - 1.0f, 1.0f - point.y / window.y * 2.0f);
    const glm::mat4 invView = glm::inverse(view());
    origin = glm::vec3(invView[3]);
    direction = glm::normalize(glm::vec3(invView * glm::vec4(ndc.x * t * aspect, ndc.y * t, -1.0f, 0.0f)));
}

std::optional<glm::vec2> OrbitCamera::project(const glm::vec3& world, glm::vec2 window) const {
    const float aspect = window.x / std::max(window.y, 1.0f);
    const float t = std::tan(fovFor(aspect) * 0.5f);
    const glm::vec4 v = view() * glm::vec4(world, 1.0f);
    if (v.z > -1e-4f) return std::nullopt;
    const glm::vec2 ndc(v.x / (-v.z * t * aspect), v.y / (-v.z * t));
    return glm::vec2((ndc.x + 1.0f) * 0.5f * window.x, (1.0f - ndc.y) * 0.5f * window.y);
}

Result<render::MeshId> addMesh(render::Scene& scene, const render::MeshData& mesh) {
    return scene.addMesh(mesh.desc());
}

glm::mat4 placed(const glm::vec3& position, const glm::vec3& scale, float yaw) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    if (yaw != 0.0f) m = glm::rotate(m, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(m, scale);
}

render::MaterialId addCheckerMaterial(Context& c, render::Scene& scene, const glm::vec3& a, const glm::vec3& b,
                                      float cell, float roughness) {
    render::MaterialDesc desc{
        .surface = c.checker, .baseColor = glm::vec4((a + b) * 0.5f, 1.0f), .roughness = roughness, .name = "checker"};
    desc.params[0] = glm::vec4(a, roughness);
    desc.params[1] = glm::vec4(b, roughness * 0.9f);
    desc.params[2] = glm::vec4(cell, 0.0f, 0.0f, 0.0f);
    return scene.addMaterial(desc);
}

const char* antiAliasingName(render::AntiAliasing mode) {
    switch (mode) {
    case render::AntiAliasing::None: return "none";
    case render::AntiAliasing::Fxaa: return "FXAA";
    case render::AntiAliasing::Taa: return "TAA";
    }
    return "?";
}

void effectsPanel(Context& c, const render::RenderSettings& settings) {
    if (!ImGui::CollapsingHeader("Frame and fallbacks")) return;
    const render::ActiveEffects& e = c.renderer.effects();
    const render::RenderStats& s = c.renderer.stats();
    const gpu::Caps& caps = c.app.device().caps();
    ImGui::TextWrapped("%u x %u (scale %.2f), %u draws, %u instances", s.renderWidth, s.renderHeight,
                       double(settings.renderScale), s.draws, s.instances);
    ImGui::TextWrapped("Culled %u, local lights %u, shadow draws %u", s.culled, s.lights, s.shadowDraws);
    ImGui::TextWrapped("Shadows: %d of %d cascades%s", e.shadowCascades, settings.shadowCascades,
                       e.shadowCascades > 0 ? (e.shadowDepthClamp ? ", depth clamp" : ", no depth clamp (fallback)")
                                            : "");
    ImGui::TextWrapped("Anti-aliasing: %s%s", antiAliasingName(e.antiAliasing),
                       e.antiAliasing != settings.antiAliasing ? " (fallback)" : "");
    ImGui::TextWrapped("Bloom: %s, sky: %s", e.bloom ? "on" : "off", settings.sky ? "on" : "off");
    ImGui::TextWrapped("Formats: HDR %s, depth %s", gpu::formatInfo(c.renderer.hdrFormat()).name,
                       gpu::formatInfo(c.renderer.depthFormat()).name);
    ImGui::TextWrapped("Caps: depth clamp %s, GPU timing %s", caps.depthClamp ? "yes" : "no",
                       caps.gpuTiming ? "yes" : "no");
}

std::optional<bool> parseBool(std::string_view value) {
    if (value == "1" || value == "on" || value == "true") return true;
    if (value == "0" || value == "off" || value == "false") return false;
    return std::nullopt;
}

std::optional<float> parseFloat(std::string_view value) {
    const std::optional<double> v = parseDouble(value);
    if (!v) return std::nullopt;
    return float(*v);
}

std::uint32_t Random::next() {
    // xorshift32
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float Random::uniform(float lo, float hi) { return lo + (hi - lo) * float(next() >> 8) / float(1u << 24); }

} // namespace demo
