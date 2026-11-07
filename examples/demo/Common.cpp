#include "Demo.h"

#include <quench/core/Parse.h>
#include <quench/render/Daylight.h>

#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>

namespace demo {

using namespace quench;

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

render::Environment setDaylight(render::Scene& scene, render::LightId sun, float elevationDegrees, float azimuthDegrees,
                                float exposure) {
    const glm::vec3 towardSun = render::skyDirection(elevationDegrees, azimuthDegrees);
    render::Daylight day = render::daylightFor(towardSun);
    day.environment.exposure *= exposure;
    scene.setEnvironment(day.environment);
    scene.setLight(sun, {.type = render::LightType::Directional, .color = day.sunColor, .direction = -towardSun});
    return day.environment;
}

glm::vec2 windowSize(const Context& c) { return {float(c.app.windowWidth()), float(c.app.windowHeight())}; }

std::optional<glm::vec3> groundPoint(const Context& c, const OrbitCamera& camera, glm::vec2 point) {
    glm::vec3 origin, direction;
    camera.ray(point, windowSize(c), origin, direction);
    if (direction.y >= -1e-3f) return std::nullopt;
    return origin + direction * (-origin.y / direction.y);
}

app::ActionId action(app::Input& input, std::string name, std::initializer_list<app::Binding> bindings) {
    if (const std::optional<app::ActionId> found = input.findAction(name)) return *found;
    return input.addAction(std::move(name), bindings);
}

float itemWidth() { return ImGui::GetFontSize() * 8.0f; }

const char* antiAliasingName(render::AntiAliasing mode) {
    switch (mode) {
    case render::AntiAliasing::None: return "none";
    case render::AntiAliasing::Fxaa: return "FXAA";
    case render::AntiAliasing::Taa: return "TAA";
    }
    return "?";
}

const char* upscalingName(render::Upscaling mode) {
    switch (mode) {
    case render::Upscaling::Bilinear: return "bilinear";
    case render::Upscaling::Spatial: return "MetalFX spatial";
    case render::Upscaling::Temporal: return "MetalFX temporal";
    case render::Upscaling::TemporalDenoised: return "MetalFX denoising";
    }
    return "?";
}

const char* rayTracingWhyNot(const gpu::Caps& caps) {
    if (caps.backend == gpu::Backend::SdlGpu)
        return caps.driver == "vulkan" ? "SDL GPU (here Vulkan through MoltenVK) has no acceleration structures or "
                                         "ray queries; --gpu-backend metal has them on Apple GPUs."
                                       : "SDL GPU has no acceleration structures or ray queries; --gpu-backend metal "
                                         "has them on Apple GPUs.";
    return "This GPU (or the iOS Simulator) has no ray queries in render passes.";
}

void effectsPanel(Context& c, const render::RenderSettings& settings, const render::Scene& scene) {
    if (!ImGui::CollapsingHeader("Frame and fallbacks")) return;
    const render::ActiveEffects& e = c.renderer.effects();
    const render::RenderStats& s = c.renderer.stats();
    const gpu::Caps& caps = c.app.device().caps();
    ImGui::TextWrapped("Scene: %u meshes, %u textures, %u materials, %u instances, %u lights", scene.meshCount(),
                       scene.textureCount(), scene.materialCount(), scene.instanceCount(), scene.lightCount());
    ImGui::TextWrapped("%u x %u (scale %.2f), %u draws, %u instances", s.renderWidth, s.renderHeight,
                       double(settings.renderScale), s.draws, s.instances);
    ImGui::TextWrapped("Culled %u, local lights %u, shadow draws %u", s.culled, s.lights, s.shadowDraws);
    if (e.sunShadows == render::SunShadows::RayTraced)
        ImGui::TextWrapped("Shadows: ray traced, %u instances in the top level", s.rayInstances);
    else
        ImGui::TextWrapped("Shadows: %d of %d cascades%s", e.shadowCascades, settings.shadowCascades,
                           e.shadowCascades > 0 ? (e.shadowDepthClamp ? ", depth clamp" : ", no depth clamp (fallback)")
                                                : "");
    if (settings.ambientOcclusion != render::AmbientOcclusion::None ||
        e.ambientOcclusion != render::AmbientOcclusion::None)
        ImGui::TextWrapped("Ambient occlusion: %s",
                           e.ambientOcclusion == render::AmbientOcclusion::RayTraced     ? "ray traced"
                           : e.ambientOcclusion == render::AmbientOcclusion::ScreenSpace ? "screen space (GTAO)"
                                                                                         : "none");
    ImGui::TextWrapped("Translucent shadows: %s, %u transmittance draws",
                       e.translucentShadows == render::TranslucentShadows::Map ? "transmittance map" : "dithered",
                       s.transmittanceDraws);
    if (e.localLightShadows > 0 || e.reflections != render::Reflections::Sky ||
        e.indirectLight != render::IndirectLight::Sky)
        ImGui::TextWrapped("Traced lighting: %d local light shadows, reflections %s, indirect light %s (%u probes); "
                           "%s denoiser",
                           e.localLightShadows,
                           e.reflections == render::Reflections::RayTraced     ? "traced"
                           : e.reflections == render::Reflections::ScreenSpace ? "screen space (SSR)"
                                                                               : "sky",
                           e.indirectLight == render::IndirectLight::RayTraced ? "traced"
                           : e.indirectLight == render::IndirectLight::Probes  ? "probes"
                                                                               : "sky",
                           s.probes, e.rayDenoiser == render::RayDenoiser::Temporal ? "temporal" : "spatial");
    if (e.pathTracer) ImGui::TextWrapped("Path tracer: %u samples", s.pathSamples);
    ImGui::TextWrapped("Anti-aliasing: %s", antiAliasingName(e.antiAliasing));
    if (settings.upscaling != render::Upscaling::Bilinear || e.upscaling != render::Upscaling::Bilinear)
        ImGui::TextWrapped("Upscaling: %s", upscalingName(e.upscaling));
    ImGui::TextWrapped("Bloom: %s, sky: %s", e.bloom ? "on" : "off", settings.sky ? "on" : "off");
    ImGui::TextWrapped("Formats: HDR %s, depth %s", gpu::formatInfo(c.renderer.hdrFormat()).name,
                       gpu::formatInfo(c.renderer.depthFormat()).name);
    // What the settings asked for and did not get, with the renderer's reasons.
    const std::span<const std::string> fallbacks = c.renderer.fallbacks();
    if (fallbacks.empty()) {
        ImGui::TextDisabled("No fallbacks: every effect runs as set.");
    } else {
        ImGui::TextUnformatted("Fallbacks:");
        for (const std::string& line : fallbacks) ImGui::BulletText("%s", line.c_str());
    }
    ImGui::TextWrapped("GPU %s backend (%s, %s); caps: depth clamp %s, GPU timing %s, ray tracing %s, MetalFX %s",
                       gpu::backendName(caps.backend), caps.driver.c_str(), caps.deviceName.c_str(),
                       caps.depthClamp ? "yes" : "no", caps.gpuTiming ? "yes" : "no", caps.rayTracing ? "yes" : "no",
                       caps.temporalUpscaling ? "yes" : "no");
    if (!caps.rayTracing) ImGui::TextWrapped("No ray tracing: %s", rayTracingWhyNot(caps));
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
