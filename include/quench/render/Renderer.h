#pragma once

// The standard frame: sun shadow cascades, the main HDR pass with reversed depth (surfaces, sky, transparent
// surfaces), motion vectors and temporal anti-aliasing, bloom, tone mapping and FXAA, drawn from a render::Scene into
// a target texture. Games add their own passes at fixed stages (addPass) and their own material shading as surface
// shaders (addSurface, quench/Surface.hlsli). Each effect falls back by device capability (effects()), never by
// platform. docs/render.md describes the frame and the shader interface.

#include <quench/core/Result.h>
#include <quench/gpu/Device.h>
#include <quench/render/Cascades.h> // kMaxCascades
#include <quench/render/Handles.h>
#include <quench/render/Scene.h>
#include <quench/render/ToneMap.h> // ToneCurve

#include <quench/SceneUniforms.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::render {

namespace detail {
struct RendererImpl;
}

enum class AntiAliasing : std::uint8_t { None, Fxaa, Taa };

// How translucent casters (ShadowMode::Translucent) shadow the sun's light.
enum class TranslucentShadows : std::uint8_t {
    // A transmittance map per cascade (Rgba16Float, half the shadow map size, made when a frame first has translucent
    // casters): smooth and tinted by the casters' hue; receivers in front of a caster stay lit.
    Map,
    // The opacity dithered into the depth map and smoothed by the shadow filter: grey and grainy, overlapping
    // translucent casters shadow like the most opaque of them. The fallback where the map cannot be drawn.
    Dithered,
};

// How the sun's shadows are made.
enum class SunShadows : std::uint8_t {
    // Cascaded shadow maps (shadowCascades, shadowMapSize, shadowDistance, shadowFilter, translucentShadows).
    ShadowMaps,
    // Rays from every pixel toward the sun's disc against the scene's casters (gpu::Caps::rayTracing; falls back to
    // ShadowMaps): exact contact shadows, soft by the sun's angular size, cut-outs and translucent tints as the maps
    // have them. docs/render.md, "Ray-traced shadows".
    RayTraced,
};

// Ambient occlusion of the sky and ambient light.
enum class AmbientOcclusion : std::uint8_t {
    None,
    RayTraced, // rays over the hemisphere of each pixel (gpu::Caps::rayTracing; falls back to None)
};

// How a frame rendered below the target's size (renderScale < 1) is scaled up.
enum class Upscaling : std::uint8_t {
    Bilinear, // in tone mapping
    Spatial,  // MetalFX spatial upscaling after anti-aliasing (gpu::Caps::spatialUpscaling; else Bilinear)
    // MetalFX temporal upscaling from the jittered frame, its depth and motion, in place of TAA's resolve (also at
    // renderScale 1: anti-aliasing only). Needs TAA's inputs and gpu::Caps::temporalUpscaling; else Bilinear.
    Temporal,
    // MetalFX's temporal denoising upscaler: the frame with its traced effects undenoised, its depth, motion and a
    // G-buffer (albedos, normal, roughness), denoised and scaled up in one pass, in place of TAA and the engine's
    // denoiser (gpu::Caps::denoisedUpscaling, ray tracing; else Temporal with the engine's denoiser).
    TemporalDenoised,
};

// How the noise of the traced effects (soft and local light shadows, ambient occlusion, reflections, indirect light)
// is filtered before the surfaces read them.
enum class RayDenoiser : std::uint8_t {
    // A depth-aware blur of each frame (3 x 3 for the sun's penumbrae, 5 x 5 for the occlusion); TAA averages the
    // rest. Reflections and indirect light get the a-trous filter only.
    Spatial,
    // A history per effect, reprojected with the motion vectors and dropped where the depth disagrees, clamped to the
    // spread of the new samples, then a-trous filtering weighted by the surface's plane (docs/render.md,
    // "Denoising"). Needs the motion vector inputs (else Spatial).
    Temporal,
};

// How surfaces below RenderSettings::reflectionRoughness reflect their surroundings.
enum class Reflections : std::uint8_t {
    Sky,       // the sky, blurred toward the ambient light with roughness (all surfaces)
    RayTraced, // one ray per pixel along the GGX lobe; hits lit by the sun with shadows (gpu::Caps::rayTracing)
};

// The light that reaches surfaces from other surfaces and the sky (the diffuse part; reflections are separate).
enum class IndirectLight : std::uint8_t {
    Sky, // Environment::skyAmbient and groundAmbient by the normal's direction
    // A grid of 16 x 8 x 16 probes around View::probeCenter (probeSpacing apart), traced every frame: one bounce from
    // the surfaces around them and more through the grid's history, with visibility so light does not leak through
    // walls (DDGI). Smooth, a few frames behind changes. gpu::Caps::rayTracing; else Sky.
    Probes,
    // One ray per pixel over the hemisphere, its hits lit with the probe grid (also running): contact detail the grid
    // is too coarse for. gpu::Caps::rayTracing; else Sky.
    RayTraced,
};

// Ray-traced shadows of at most this many local lights per frame (RenderSettings::localLightShadows).
inline constexpr int kMaxRayLocalShadows = 4;

struct RenderSettings {
    float renderScale = 1.0f; // 3D resolution relative to the target, 0.25..1 (scaled up by `upscaling`)
    Upscaling upscaling = Upscaling::Bilinear;
    SunShadows sunShadows = SunShadows::ShadowMaps;
    int shadowCascades = 3; // 0..kMaxCascades; 0: no sun shadows (also no ray-traced ones)
    std::uint32_t shadowMapSize = 2048;
    float shadowDistance = 80.0f; // m: the far end of the last cascade; ray-traced: skinned casters this near
    int shadowFilter = 2;         // 0: one tap, 1: four taps, 2: twelve Poisson taps
    TranslucentShadows translucentShadows = TranslucentShadows::Map;
    // Ray-traced shadows: rays per pixel toward the sun's disc (1..8; the denoise pass and TAA average them), and
    // the disc's size as a multiple of Environment::sunAngularRadius (0: hard shadows).
    int shadowRays = 2;
    float shadowSoftness = 1.0f;
    AmbientOcclusion ambientOcclusion = AmbientOcclusion::None;
    int occlusionRays = 2;        // 1..8 per pixel
    float occlusionRadius = 1.0f; // m
    float occlusionStrength = 1.0f;
    // Ray-traced shadows of the frame's nearest local lights with castsShadows (0..kMaxRayLocalShadows; one ray per
    // pixel and light, soft by LightDesc::sourceRadius; gpu::Caps::rayTracing, else unshadowed).
    int localLightShadows = 0;
    Reflections reflections = Reflections::Sky;
    float reflectionRoughness = 0.5f; // traced reflections up to this roughness, faded into the sky's above
    IndirectLight indirectLight = IndirectLight::Sky;
    float probeSpacing = 2.0f; // m between the probes of the grid
    RayDenoiser rayDenoiser = RayDenoiser::Temporal;
    // The reference path tracer (gpu::Caps::rayTracing): every frame adds one path per pixel to a running mean of
    // the scene's light, until pathSamples or a change of the camera, the scene or the settings, in place of the
    // whole frame up to bloom (no TAA, no game passes before PostHdr). For comparing the real-time effects with,
    // not for play: docs/render.md, "Path tracer".
    bool pathTracer = false;
    int pathBounces = 4;    // 1..16
    int pathSamples = 1024; // the mean stops changing after this many
    AntiAliasing antiAliasing = AntiAliasing::Taa;
    float sharpen = 0.25f; // after TAA, 0..1
    bool bloom = true;
    float bloomStrength = 0.05f;
    float bloomThreshold = 1.6f; // linear HDR value where bloom starts (soft knee)
    bool sky = true;             // false: the background is clearColor
    glm::vec3 clearColor{0.0f};  // linear HDR
    ToneCurve toneCurve = ToneCurve::Aces;
    float vignette = 0.28f;
    bool dither = true;

    friend bool operator==(const RenderSettings&, const RenderSettings&) = default;
};

// What the renderer runs after the device fallbacks: an effect the device cannot run is replaced (TAA by FXAA) or
// left out (shadows), with a log line when it changes.
struct ActiveEffects {
    // RayTraced needs gpu::Caps::rayTracing and the ray query shaders in the build (cmake/QuenchDxc.cmake).
    SunShadows sunShadows = SunShadows::ShadowMaps;
    int shadowCascades = 0;        // 0: no shadow maps (disabled, ray traced, or no depth texture array to sample)
    bool shadowDepthClamp = false; // casters in front of the cascade are clamped, not clipped (gpu::Caps::depthClamp)
    // Map needs an Rgba16Float texture array the device can render to and sample; otherwise Dithered.
    TranslucentShadows translucentShadows = TranslucentShadows::Dithered;
    AmbientOcclusion ambientOcclusion = AmbientOcclusion::None;
    // The traced lighting needs gpu::Caps::rayTracing and the ray query shaders, like RayTraced shadows; the path
    // tracer replaces the frame's lighting (the other traced effects are not run then).
    int localLightShadows = 0;
    Reflections reflections = Reflections::Sky;
    IndirectLight indirectLight = IndirectLight::Sky;
    bool pathTracer = false; // then no TAA and no temporal upscaling
    RayDenoiser rayDenoiser = RayDenoiser::Spatial;
    // With Upscaling::Temporal and TemporalDenoised: Taa's jitter, the upscaler resolves.
    AntiAliasing antiAliasing = AntiAliasing::None;
    Upscaling upscaling = Upscaling::Bilinear;
    bool bloom = false;

    friend bool operator==(const ActiveEffects&, const ActiveEffects&) = default;
};

// A surface shader built with quench_add_shaders() from a file that includes quench/SurfaceEntries.hlsli.
struct SurfaceDesc {
    const ShaderBlob* vertexShader = nullptr;       // surface_vs
    const ShaderBlob* fragmentShader = nullptr;     // surface_fs
    const ShaderBlob* shadowVertexShader = nullptr; // surface_shadow_vs, for surfaces with QUENCH_SURFACE_VERTEX
    // The shadows of cut-out and translucent materials of this surface: surface_shadow_fs and
    // surface_transmittance_fs. Without them those shadows take the standard surface's opacity (base color alpha
    // times texture 0 alpha times the vertex color alpha).
    const ShaderBlob* shadowFragmentShader = nullptr;
    const ShaderBlob* transmittanceShader = nullptr;
    // surface_gbuffer_fs: the surface's normal, roughness, albedo and metallic for the traced reflections, indirect
    // light and the denoising upscaler. Without it the standard surface's are written (the material's values).
    const ShaderBlob* gbufferShader = nullptr;
    std::string_view name{};
};

enum class PassStage : std::uint8_t {
    Shadow,      // in each cascade's depth pass after the scene's casters (PassContext::cascade)
    Opaque,      // in the HDR pass after the opaque surfaces, before the sky; depth test and write as the scene
    Transparent, // in the HDR pass after the transparent surfaces
    PostHdr,     // outside passes, on the anti-aliased HDR image before bloom and tone mapping
    Overlay,     // in the pass on the target after tone mapping (values as PassContext::output; ImGui comes after)
};

// What a game pass gets. The frame uniforms are in uniform slot 0 of both stages when the callback starts (a shader
// reads them through quench/Frame.hlsli); the renderer pushes them again afterwards, so a callback may use slot 0.
struct PassContext {
    gpu::CommandBuffer& commands;
    gpu::RenderPass* pass; // the open pass; null in PostHdr
    const shader::FrameUniforms& frame;
    // Shadow: none. Opaque, Transparent: the HDR target. PostHdr: the HDR image to read or to draw onto with
    // LoadOp::Load. Overlay: the target.
    gpu::TextureRef color;
    // The scene depth (reversed: near 1, far 0). Shadow: the cascade's layer of the shadow map (standard depth).
    // Sampled in PostHdr and Overlay where gpu::TextureUsage::Sampled is set in its desc. Empty in PostHdr with
    // Upscaling::Spatial or Temporal (the image is at the target's size, the depth is not).
    gpu::TextureRef depth;
    gpu::TextureFormat colorFormat = gpu::TextureFormat::Invalid; // pipeline formats of `pass`
    gpu::TextureFormat depthFormat = gpu::TextureFormat::Invalid;
    std::uint32_t width = 0; // pixels of `color` (Shadow: of the shadow map)
    std::uint32_t height = 0;
    int cascade = -1; // Shadow: the cascade being drawn
    // Overlay: what the target's values mean. SDR: display values (sRGB-encoded, or linear for sRGB formats). HDR
    // extended linear: linear values, paper white at output.paperWhite.
    gpu::DisplayOutput output{};
};

using PassFn = std::function<void(PassContext& context)>;

// The camera of a frame. The projection is perspective with reversed depth and no far plane.
struct View {
    glm::mat4 view{1.0f};   // world -> view (right-handed, looking down -z; glm::lookAt)
    float fovY = 1.0f;      // vertical field of view (radians)
    float nearPlane = 0.1f; // m
    bool cut = false;       // a camera cut: no motion from the previous frame, the TAA history restarts
    double time = 0.0;      // seconds, for animated surfaces (FrameUniforms::cameraPos.w)
    // The center of the probe grid (IndirectLight::Probes and RayTraced); unset: the camera. A camera high above
    // the ground (top-down, strategy) sets the point it looks at, so the grid covers the ground.
    std::optional<glm::vec3> probeCenter{};
};

struct RenderStats {
    std::uint32_t instances = 0;       // instances drawn in the HDR pass
    std::uint32_t culled = 0;          // visible instances outside the view
    std::uint32_t draws = 0;           // draw calls of scene surfaces in the HDR pass
    std::uint32_t shadowInstances = 0; // instances drawn into the cascades' depth (summed over cascades)
    std::uint32_t shadowDraws = 0;
    std::uint32_t transmittanceInstances = 0; // translucent casters drawn into the transmittance maps (summed)
    std::uint32_t transmittanceDraws = 0;
    std::uint32_t movedInstances = 0; // instances drawn into the motion vectors
    std::uint32_t lights = 0;         // local lights used this frame
    std::uint32_t renderWidth = 0;    // the 3D resolution
    std::uint32_t renderHeight = 0;
    std::uint32_t pipelines = 0;        // pipelines created so far
    std::uint32_t skinnedInstances = 0; // skinned instances posed on the GPU this frame (drawn in view or shadows)
    std::uint32_t skinnedVertices = 0;
    // GPU time of the latest skinning pass whose time is known (palette upload and compute, a few frames old);
    // -1: none yet, or no GPU timing on this device (gpu::Caps::gpuTiming).
    float skinningGpuMs = -1.0f;
    // Ray tracing: instances in the top level, bottom levels built and refitted this frame.
    std::uint32_t rayInstances = 0;
    std::uint32_t rayBuilds = 0;
    std::uint32_t rayRefits = 0;
    std::uint32_t rayCompactions = 0; // bottom levels compacted this frame
    // GPU memory of the ray tracing data: acceleration structures, the vertex and index tables and the hit texture
    // arrays (not the targets).
    std::uint64_t rayMemoryBytes = 0;
    std::uint32_t hitTextures = 0; // material textures in the hit texture arrays
    std::uint32_t probes = 0;      // probes traced this frame (IndirectLight::Probes and RayTraced)
    std::uint32_t pathSamples = 0; // samples in the path tracer's mean
};

class Renderer {
public:
    // The engine's pipelines are created here; surface pipelines when a surface is first drawn with a blend mode.
    static Result<Renderer> create(gpu::Device& device, const RenderSettings& settings = {});

    Renderer(Renderer&& other) noexcept;
    Renderer& operator=(Renderer&& other) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    void setSettings(const RenderSettings& settings);
    const RenderSettings& settings() const;
    const ActiveEffects& effects() const;
    // What fell back from the settings and why, one line each ("ray-traced shadows: the device has no ray tracing
    // (gpu::Caps::rayTracing); shadow maps"), as the warnings logged when the effects were chosen; empty when every
    // effect runs as set.
    std::span<const std::string> fallbacks() const;

    // Creates the opaque pipeline at once, so a shader that does not fit the surface interface fails here.
    Result<SurfaceId> addSurface(const SurfaceDesc& desc);

    // Callbacks run in the order they were added within a stage, every frame until removed.
    PassId addPass(PassStage stage, std::string name, PassFn fn);
    void removePass(PassId pass);

    // Records the frame into `commands`: the scene seen from `view`, tone mapped into `target` (a color target of any
    // size; sRGB and UNORM formats both get display values). One render per scene and frame: the scene's motion
    // vectors are measured from its previous render. With an HdrExtendedLinear output (an Rgba16Float target, as the
    // swapchain of that composition) the HDR curve maps highlights up to output.headroom (ToneMap.h); other outputs
    // get the SDR curve (Hdr10 is not supported: SDR values, logged once).
    void render(Scene& scene, const View& view, gpu::CommandBuffer& commands, const gpu::TextureRef& target,
                const gpu::DisplayOutput& output = {});

    const RenderStats& stats() const;
    gpu::TextureFormat hdrFormat() const;    // color format of the HDR pass (Opaque, Transparent, PostHdr)
    gpu::TextureFormat depthFormat() const;  // depth format of the HDR pass
    gpu::TextureFormat shadowFormat() const; // depth format of the shadow passes

private:
    Renderer() = default;
    std::unique_ptr<detail::RendererImpl> m_impl;
};

} // namespace quench::render
