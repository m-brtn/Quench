#pragma once

// The engine demo (examples/demo): one app with a section per engine feature. Each section owns its scene, camera and
// render settings; Demo.cpp has the menu, the shared camera controls, the timings and the scenario hooks.

#include <quench/app/App.h>
#include <quench/app/Input.h>
#include <quench/core/Result.h>
#include <quench/render/MeshBuilder.h>
#include <quench/render/Renderer.h>
#include <quench/render/Scene.h>

#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

// Orbits a target point. Mouse or finger drags turn it, the wheel, pinches and the gamepad triggers zoom.
struct OrbitCamera {
    glm::vec3 target{0.0f};
    float distance = 8.0f;
    float yaw = 0.0f;   // radians; 0: the camera is on +z looking toward -z
    float pitch = 0.4f; // radians above the target
    float minDistance = 1.5f;
    float maxDistance = 60.0f;
    float fovY = glm::radians(50.0f);

    glm::vec3 eye() const;
    glm::mat4 view() const;
    // fovY, widened on portrait screens to keep at least 64 degrees horizontally.
    float fovFor(float aspect) const;
    // A world-space ray through a window point (points, origin top left).
    void ray(glm::vec2 point, glm::vec2 window, glm::vec3& origin, glm::vec3& direction) const;
    // The window point a world point is drawn at; nullopt behind the camera.
    std::optional<glm::vec2> project(const glm::vec3& world, glm::vec2 window) const;
};

// Engine objects every section uses.
struct Context {
    quench::app::App& app;
    quench::render::Renderer& renderer;
    quench::render::SurfaceId checker; // the demo's checker surface shader (shaders/checker.hlsl)
};

class Section {
public:
    virtual ~Section() = default;
    virtual std::string_view id() const = 0;    // for --section and the scenario command `section`
    virtual std::string_view title() const = 0; // menu entry
    virtual std::string_view about() const = 0; // one line under the title

    virtual quench::Result<void> init(Context& c) = 0;
    // Becomes the current section (passes are added here and removed in leave).
    virtual void enter(Context& c) { (void)c; }
    virtual void leave(Context& c) { (void)c; }
    virtual void fixedStep(Context& c, double dt) {
        (void)c;
        (void)dt;
    }
    virtual void update(Context& c, double frameDt) {
        (void)c;
        (void)frameDt;
    }
    // The section's controls inside its window.
    virtual void ui(Context& c) = 0;
    // A click or tap on the scene (not on a window) without a drag, in window points.
    virtual void click(Context& c, glm::vec2 point) {
        (void)c;
        (void)point;
    }
    virtual quench::render::Scene& scene() = 0;
    // The display output the renderer maps to (the frame's, or the section's variant of it).
    virtual quench::gpu::DisplayOutput output(const quench::gpu::DisplayOutput& display) const { return display; }
    // false: drags and the gamepad's right stick are the section's own (the input section's virtual stick).
    virtual bool orbitWithFingers() const { return true; }
    // Scenario values and `set NAME VALUE` commands of the section; false: unknown name or bad value.
    virtual std::optional<double> value(std::string_view name) const {
        (void)name;
        return std::nullopt;
    }
    virtual bool set(Context& c, std::string_view name, std::string_view value) {
        (void)c;
        (void)name;
        (void)value;
        return false;
    }
    // Appended to the perf line of a benchmark region (`key=value` pairs, docs/performance.md); empty: nothing.
    virtual std::string perfDetails() const { return {}; }
    // Named window points for the scenario pointer commands (`click @NAME`), e.g. a projected world point.
    virtual std::optional<glm::vec2> point(const Context& c, std::string_view name) const {
        (void)c;
        (void)name;
        return std::nullopt;
    }
    virtual void shutdown(Context& c) { (void)c; }

    OrbitCamera camera;
    quench::render::RenderSettings settings;
};

std::unique_ptr<Section> makeMaterialsSection();
std::unique_ptr<Section> makeShadowsSection();
std::unique_ptr<Section> makeSkySection();
std::unique_ptr<Section> makePostSection();
std::unique_ptr<Section> makeHdrSection();
std::unique_ptr<Section> makePhysicsSection();
std::unique_ptr<Section> makeInputSection();
std::unique_ptr<Section> makeAssetsSection();
std::unique_ptr<Section> makeAudioSection();
std::unique_ptr<Section> makeAnimationSection();
std::unique_ptr<Section> makeRayTracingSection();

// ---- helpers shared by the sections (Common.cpp)

quench::Result<quench::render::MeshId> addMesh(quench::render::Scene& scene, const quench::render::MeshData& mesh);
glm::mat4 placed(const glm::vec3& position, const glm::vec3& scale = glm::vec3(1.0f), float yaw = 0.0f);
// A ground of the checker surface; falls back to a plain material without the custom surface.
quench::render::MaterialId addCheckerMaterial(Context& c, quench::render::Scene& scene, const glm::vec3& a,
                                              const glm::vec3& b, float cell, float roughness = 0.85f);
// The renderer's settings and effects in a collapsing header: what runs, and what fell back.
void effectsPanel(Context& c, const quench::render::RenderSettings& settings);
// Parses a `set` value: 0 / 1, on / off, true / false; numbers.
std::optional<bool> parseBool(std::string_view value);
std::optional<float> parseFloat(std::string_view value);
const char* antiAliasingName(quench::render::AntiAliasing mode);
// Small deterministic random numbers (the same sequence on every platform).
struct Random {
    std::uint32_t state = 0x9e3779b9u;
    std::uint32_t next();
    float uniform(float lo = 0.0f, float hi = 1.0f);
};

} // namespace demo
