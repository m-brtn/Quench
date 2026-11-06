#pragma once

// The engine demo (examples/demo): one app with a section per engine feature (sections/), each with its own scene,
// camera and render settings behind the Section interface below. DemoGame.h is the app around them: the layouts, the
// camera controls (Camera.cpp), the modes (Modes.cpp: tour, checks, benchmark), the settings and the scenario hooks.

#include <quench/app/App.h>
#include <quench/app/Input.h>
#include <quench/core/Result.h>
#include <quench/render/MeshBuilder.h>
#include <quench/render/Renderer.h>
#include <quench/render/Scene.h>

#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

// Orbits a target point: drags turn it, pans move the target, the wheel, pinches and the triggers zoom (Camera.cpp).
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
    // Moves the target in the ground plane by a drag of `points` (screen right and up), scaled with the distance.
    void pan(glm::vec2 points, float windowHeight);
};

// Engine objects every section uses.
struct Context {
    quench::app::App& app;
    quench::render::Renderer& renderer;
    quench::render::SurfaceId checker; // the demo's checker surface shader (shaders/checker.hlsl)
    // The keyboard and the gamepad are the scene's this frame, not the windows' (Tab or the D-pad hand them to the
    // windows): sections read their actions only then.
    bool sceneInput = true;
};

// One section of the demo. Sections are made at the start (cheap: the camera and the settings) and initialized when
// first shown; they stay loaded until the app ends or the section is reset (Reset section: shutdown, then a new
// object of the same kind). A section's panel shows controls(), stats() and the renderer's frame and fallbacks, in
// that order; about() is the caption under the scene (in the compact layout, the top of the controls sheet).
class Section {
public:
    virtual ~Section() = default;
    virtual std::string_view id() const = 0;    // for --section and the scenario command `section`
    virtual std::string_view title() const = 0; // menu entry and panel title
    virtual std::string_view about() const = 0; // what to look at

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
    // The section's controls and its numbers in its panel.
    virtual void controls(Context& c) = 0;
    virtual void stats(Context& c) { (void)c; }
    // Drawn over the scene while the windows are shown (labels at world points, fingers): ImGui's foreground list.
    virtual void overlay(Context& c) { (void)c; }
    // A click or tap on the scene (not on a window) without a drag, in window points.
    virtual void click(Context& c, glm::vec2 point) {
        (void)c;
        (void)point;
    }
    virtual quench::render::Scene& scene() = 0;
    // The display output the renderer maps to (the frame's, or the section's variant of it).
    virtual quench::gpu::DisplayOutput output(const quench::gpu::DisplayOutput& display) const { return display; }
    // false: fingers and sticks are the section's own (the input section's virtual stick and player), the camera
    // only zooms.
    virtual bool freeCamera() const { return true; }
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
    // Releases what init made and puts back what the section changed outside itself (bus volumes, ...).
    virtual void shutdown(Context& c) { (void)c; }

    OrbitCamera camera;
    quench::render::RenderSettings settings;
};

using SectionFactory = std::unique_ptr<Section> (*)();
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
// The daylight palette (render::daylightFor) for a sun at the given angles: the scene's environment and the sun light.
// `exposure` scales the palette's exposure. Returns the environment that was set.
quench::render::Environment setDaylight(quench::render::Scene& scene, quench::render::LightId sun,
                                        float elevationDegrees, float azimuthDegrees, float exposure = 1.0f);
// Where a ray through a window point meets the ground plane y = 0; nullopt when it points at or above the horizon.
std::optional<glm::vec3> groundPoint(const Context& c, const OrbitCamera& camera, glm::vec2 point);
glm::vec2 windowSize(const Context& c); // points
// An input action, made on the first call and found by its name afterwards (a section made again after a reset).
quench::app::ActionId action(quench::app::Input& input, std::string name,
                             std::initializer_list<quench::app::Binding> bindings);
// The renderer's frame and its fallbacks with their reasons, in a collapsing header.
void effectsPanel(Context& c, const quench::render::RenderSettings& settings, const quench::render::Scene& scene);
// Why ray tracing is not available (gpu::Caps::rayTracing false), one sentence.
const char* rayTracingWhyNot(const quench::gpu::Caps& caps);
// Width for a slider or combo of the panels.
float itemWidth();
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

// The demo's animated mannequin (tools/assets/make_demo_character.py), shared by the animation and ray tracing
// sections.
namespace mannequin {
constexpr const char* kModel = "characters/mannequin.qmodel";
constexpr const char* kSkeleton = "characters/mannequin.qskel";
constexpr const char* kClips = "characters/mannequin.qanim";
constexpr const char* kGraph = "characters/mannequin.graph.json";
} // namespace mannequin

} // namespace demo
