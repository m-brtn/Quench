#include <quench/render/Renderer.h>

#include "Clouds.h"
#include "Effects.h"
#include "PathTracer.h"
#include "RayTracing.h"
#include "SceneImpl.h"
#include "ScreenSpace.h"
#include "shaders/PostUniforms.h"

#include <quench/core/Log.h>
#include <quench/core/Profile.h>
#include <quench/render/Cascades.h>
#include <quench/render/Projection.h>

#include <render_frame_shaders.h>
#include <render_raylight_shaders.h>
#include <render_raytrace_shaders.h>
#include <render_skin_shaders.h>
#include <render_standard_shaders.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <span>
#include <vector>

namespace quench::render {
namespace detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureUsage;

constexpr ShaderStage kVertex = ShaderStage::Vertex;
constexpr ShaderStage kFragment = ShaderStage::Fragment;
constexpr int kBloomLevels = 5;
constexpr float kTaaCurrentWeight = 0.1f;
constexpr float kShadowNormalOffset = 1.6f; // in shadow texels of the cascade
constexpr std::uint32_t kVariants = 18;     // 3 blend modes x single / double sided x 3 Shading
// The transmittance maps of translucent casters have 1 / kTransmittanceScale of the shadow map size per side: their
// shadows are soft, and a full-size Rgba16Float array would take twice the depth array's memory.
constexpr std::uint32_t kTransmittanceScale = 2;
constexpr float kTransmittanceBias = 2.0f; // in transmittance texels: receivers nearer behind a caster stay lit
// Local light shadow maps: a spot light has one up to this cone angle (its outer cone's cosine above it), six maps
// (a cube) beyond; a point light's six maps reach this many texels past the cube's faces, so that the filter's taps
// at a face's edge stay inside its map; the normal offset in texels of the map at the receiver's distance; the near
// plane at least the light's source radius, and at least this part of its range.
constexpr int kMaxLocalShadowMaps = kMaxRayLocalShadows * 6;
constexpr float kSpotMapMinCos = 0.5f;
constexpr float kLocalShadowEdge = 3.0f;
constexpr float kLocalShadowNormalOffset = 1.5f;
constexpr float kLocalShadowNear = 0.01f;
// Ray-traced shadows: how far rays toward the sun look (m), the ray origin's offset along the normal per meter of
// view distance (the depth's precision and the normal reconstructed from it), and the denoise pass.
constexpr float kRayShadowLength = 1000.0f;
constexpr float kRayNormalOffset = 0.002f;
// Soft sun shadows (a cone) are noisy in their penumbrae only; ambient occlusion is noisy everywhere.
constexpr int kRayDenoiseShadowRadius = 1;    // pixels: a 3 x 3 box (0 for hard shadows)
constexpr int kRayDenoiseOcclusionRadius = 2; // pixels: a 5 x 5 box
constexpr float kRayDenoiseTolerance = 0.02f; // relative view depth: neighbours further apart are other surfaces
// Reflection, indirect and probe rays: how far they look (m), and the probes' sampling bias per meter of spacing.
constexpr float kRayLightLength = 1000.0f;
constexpr float kProbeBias = 0.3f;
// GTAO (screenspace.hlsl): steps to either side of a slice, XeGTAO's radius multiplier, the longest screen radius
// (relative to the render height: without a depth MIP chain, far samples only miss the cache) and the blur's depth
// tolerance (relative view depth).
constexpr float kGtaoSteps = 4.0f;
constexpr float kGtaoRadiusMultiplier = 1.457f;
constexpr float kGtaoMaxRadius = 0.2f;
constexpr float kGtaoBlurTolerance = 0.02f;
// Screen-space reflections (screenspace.hlsl): march steps, the first step (relative to the view depth), the growth
// of each next step (48 steps reach about 4.8 view depths), the surfaces' thickness and the resolve's depth tolerance
// (relative to the view depth).
constexpr float kSsrSteps = 48.0f;
constexpr float kSsrFirstStep = 0.005f;
constexpr float kSsrGrowth = 1.1f;
constexpr float kSsrThickness = 0.03f;
constexpr float kSsrResolveTolerance = 0.02f;

#include "LtcTable.inc"
constexpr std::uint32_t kLtcSize = 64;
static_assert(std::size(kLtcMatrix) == std::size_t(kLtcSize) * kLtcSize * 4);

constexpr gpu::VertexBufferLayout kVertexLayout{.slot = 0, .stride = sizeof(Vertex)};
constexpr gpu::VertexAttribute kVertexAttributes[] = {
    {.location = 0, .slot = 0, .format = gpu::VertexFormat::Float3, .offset = offsetof(Vertex, position)},
    {.location = 1, .slot = 0, .format = gpu::VertexFormat::Float3, .offset = offsetof(Vertex, normal)},
    {.location = 2, .slot = 0, .format = gpu::VertexFormat::Float2, .offset = offsetof(Vertex, uv)},
    {.location = 3, .slot = 0, .format = gpu::VertexFormat::Float4, .offset = offsetof(Vertex, color)},
    {.location = 4, .slot = 0, .format = gpu::VertexFormat::Float4, .offset = offsetof(Vertex, tangent)},
};
// Shadow and motion vector shaders read only the position: a layout without the other attributes keeps the Vulkan
// validation layer from reporting attributes the shader does not consume.
constexpr gpu::VertexAttribute kPositionAttribute[] = {
    {.location = 0, .slot = 0, .format = gpu::VertexFormat::Float3, .offset = offsetof(Vertex, position)},
};
// Motion vectors of skinned instances: this frame's and the previous frame's skinned positions.
constexpr gpu::VertexBufferLayout kSkinVelocityLayouts[] = {{.slot = 0, .stride = sizeof(Vertex)},
                                                            {.slot = 1, .stride = sizeof(Vertex)}};
constexpr gpu::VertexAttribute kSkinVelocityAttributes[] = {
    {.location = 0, .slot = 0, .format = gpu::VertexFormat::Float3, .offset = offsetof(Vertex, position)},
    {.location = 1, .slot = 1, .format = gpu::VertexFormat::Float3, .offset = offsetof(Vertex, position)},
};
constexpr std::uint32_t kSkinGroup = 64;     // [numthreads] of skin_cs
constexpr std::uint32_t kNotSkinned = ~0u;   // RendererImpl::skinnedOffsets
constexpr std::size_t kSkinTimingFences = 4; // skinning passes whose GPU time is still to be read

// The fragment shader of a surface variant: surface_fs, surface_basic_fs (no area lights, no anisotropy) or
// surface_local_shadows_fs (surface_fs with the local lights' shadow maps).
enum class Shading : std::uint32_t { Full, Basic, LocalShadows };

std::uint32_t variantOf(BlendMode blend, bool doubleSided, Shading shading) {
    return std::uint32_t(blend) * 2 + (doubleSided ? 1 : 0) + std::uint32_t(shading) * 6;
}

ShadowMode shadowModeOf(const MaterialDesc& m) {
    if (m.shadows != ShadowMode::Auto) return m.shadows;
    switch (m.blend) {
    case BlendMode::Opaque: return ShadowMode::Solid;
    case BlendMode::AlphaBlend: return ShadowMode::Translucent;
    case BlendMode::Additive: return ShadowMode::None;
    }
    return ShadowMode::Solid;
}

// Casters in the cascades' depth: standard depth with a slope bias; with depth clamp, casters in front of the
// cascade are flattened onto its near plane instead of clipped.
gpu::RasterizerState shadowRasterizer(const gpu::Caps& caps) {
    return {.depthBias = true,
            .depthBiasConstant = 1.0f,
            .depthBiasSlope = 1.75f,
            .depthBiasClamp = caps.depthBiasClamp ? 0.01f : 0.0f,
            .depthClip = !caps.depthClamp};
}

struct SurfaceEntry {
    const ShaderBlob* vertex = nullptr;
    const ShaderBlob* fragment = nullptr;
    const ShaderBlob* basicFragment = nullptr;       // null: fragment draws the basic variants too
    const ShaderBlob* localShadowFragment = nullptr; // null: fragment draws frames with local shadow maps (unshadowed)
    const ShaderBlob* shadowVertex = nullptr;
    const ShaderBlob* shadowFragment = nullptr;        // null: the standard surface's
    const ShaderBlob* transmittanceFragment = nullptr; // null: the standard surface's
    const ShaderBlob* gbufferFragment = nullptr;       // null: the standard surface's
    std::uint32_t rayHit = 0;                          // SurfaceDesc::rayHit (0: the standard surface in hits)
    std::string name;
    std::array<gpu::GraphicsPipeline, kVariants> pipelines;
    std::array<bool, kVariants> failed{};
    std::array<gpu::GraphicsPipeline, 2> shadowPipelines; // with shadowVertex: single / double sided
    std::array<bool, 2> shadowFailed{};
    gpu::GraphicsPipeline maskPipeline; // cut-out and dithered casters in the depth
    bool maskFailed = false;
    std::array<gpu::GraphicsPipeline, 2> transmittancePipelines; // single / double sided
    std::array<bool, 2> transmittanceFailed{};
    // The ray tracing prepass (camera depth): [doubleSided]; with shadowVertex, and of cut-out materials.
    std::array<gpu::GraphicsPipeline, 2> depthPipelines, depthMaskPipelines;
    std::array<bool, 2> depthFailed{}, depthMaskFailed{};
    // The prepass with the G-buffer (traced reflections and indirect light): [doubleSided].
    std::array<gpu::GraphicsPipeline, 2> gbufferPipelines;
    std::array<bool, 2> gbufferFailed{};
};

struct PassEntry {
    PassStage stage = PassStage::Opaque;
    std::string name;
    PassFn fn;
};

// A run of instances drawn with one call: same pipeline, material and mesh. A skinned instance is a batch of its own
// that draws its range of the skinned vertex buffer.
struct Batch {
    const gpu::GraphicsPipeline* pipeline = nullptr;
    const MeshEntry* mesh = nullptr;
    const MaterialEntry* material = nullptr; // null: the pipeline reads no material (plain shadow casters)
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    gpu::BufferBinding vertices{};                // skinned: this frame's vertices; empty: the mesh's
    gpu::BufferBinding previous{};                // skinned motion vectors: the previous frame's vertices
    const gpu::GraphicsPipeline* depth = nullptr; // the ray tracing prepass; null: not drawn there
    bool depthMaterial = false;                   // the prepass pipeline reads the material (cut-outs)
};

// The pose of one deformed instance (skinned, with blend shapes, or both) in the skinning pass.
struct SkinJob {
    const MeshEntry* mesh = nullptr;
    std::uint32_t firstRow = 0; // of its joints (three rows each) in the palette buffer
    std::uint32_t offset = 0;   // first vertex in the skinned vertex buffer
    // The palette and weights have not changed since the instance was skinned last frame at this offset: copied
    // from there.
    std::uint32_t reuseFrom = kNotSkinned;
    bool dualQuaternion = false;   // the palette's rows are dual quaternions (dualQuaternionRows)
    std::uint32_t weightRow = 0;   // its blend shapes' weights in the palette buffer: (target, weight) per row
    std::uint32_t weightCount = 0; // targets with a weight other than 0
};

// A palette matrix as three rows for Skinning::DualQuaternion: its rotation as a quaternion, the dual part (half the
// translation times the rotation) and its uniform scale in x. The scale is taken out first, so a joint with a uniform
// scale keeps it; a non-uniform scale or a shear is lost (the rotation is the nearest one quat_cast finds).
std::array<glm::vec4, 3> dualQuaternionRows(const glm::mat4& m) {
    const glm::mat3 linear(m);
    const float det = std::abs(glm::determinant(linear));
    const float scale = std::abs(det - 1.0f) < 1e-4f ? 1.0f : std::cbrt(det);
    const glm::quat q = scale > 1e-12f
                            ? glm::normalize(glm::quat_cast(scale == 1.0f ? linear : linear * (1.0f / scale)))
                            : glm::quat(1, 0, 0, 0);
    const glm::vec3 t(m[3]), v(q.x, q.y, q.z);
    return {glm::vec4(v, q.w), glm::vec4(0.5f * (q.w * t + glm::cross(t, v)), -0.5f * glm::dot(t, v)),
            glm::vec4(scale, 0.0f, 0.0f, 0.0f)};
}

struct Item {
    std::uint64_t key = 0; // sort key (opaque, shadows) or distance (transparent)
    float distance = 0.0f;
    std::uint32_t slot = 0; // instance slot
    const gpu::GraphicsPipeline* pipeline = nullptr;
    bool material = true;                         // the pipeline reads the material
    const gpu::GraphicsPipeline* depth = nullptr; // the ray tracing prepass; null: not drawn there
    bool depthMaterial = false;
};

struct Plane {
    glm::vec3 n;
    float d;
};

std::array<Plane, 5> frustumPlanes(const glm::mat4& m) {
    // Gribb-Hartmann from the rows of the view-projection; no far plane (the projection is infinite).
    const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]), r1(m[0][1], m[1][1], m[2][1], m[3][1]),
        r2(m[0][2], m[1][2], m[2][2], m[3][2]), r3(m[0][3], m[1][3], m[2][3], m[3][3]);
    const glm::vec4 p[5] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 - r2};
    std::array<Plane, 5> out{};
    for (int i = 0; i < 5; ++i) {
        const float len = glm::length(glm::vec3(p[i]));
        out[std::size_t(i)] = {glm::vec3(p[i]) / len, p[i].w / len};
    }
    return out;
}

bool sphereVisible(const std::array<Plane, 5>& planes, const glm::vec3& c, float r) {
    for (const Plane& p : planes)
        if (glm::dot(p.n, c) + p.d < -r) return false;
    return true;
}

float maxScale(const glm::mat4& m) {
    return std::sqrt(std::max({glm::dot(glm::vec3(m[0]), glm::vec3(m[0])), glm::dot(glm::vec3(m[1]), glm::vec3(m[1])),
                               glm::dot(glm::vec3(m[2]), glm::vec3(m[2]))}));
}

bool isSrgb(TextureFormat format) { return gpu::formatInfo(format).srgb; }

// A different rotation every frame (the probe rays): an axis and an angle from a hash of the frame.
glm::mat4 frameRotation(std::uint64_t frame) {
    const auto hash = [](std::uint32_t v) {
        v = v * 747796405u + 2891336453u;
        v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
        return float((v >> 22u) ^ v) / 4294967296.0f;
    };
    const auto f = std::uint32_t(frame);
    const float z = hash(f * 3u) * 2.0f - 1.0f, a = hash(f * 3u + 1u) * 6.2831853f,
                angle = hash(f * 3u + 2u) * 6.2831853f;
    const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
    const glm::vec3 axis(r * std::cos(a), z, r * std::sin(a));
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(t * axis.x * axis.x + c, t * axis.x * axis.y + s * axis.z, t * axis.x * axis.z - s * axis.y, 0.0f);
    m[1] = glm::vec4(t * axis.x * axis.y - s * axis.z, t * axis.y * axis.y + c, t * axis.y * axis.z + s * axis.x, 0.0f);
    m[2] = glm::vec4(t * axis.x * axis.z + s * axis.y, t * axis.y * axis.z - s * axis.x, t * axis.z * axis.z + c, 0.0f);
    return m;
}

// FNV-1a over plain values: what the path tracer's mean depends on.
struct StateHash {
    std::uint64_t value = 1469598103934665603ull;
    template <class T>
    void add(const T& v) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&v);
        for (std::size_t i = 0; i < sizeof(T); ++i) value = (value ^ bytes[i]) * 1099511628211ull;
    }
};

} // namespace

struct RendererImpl {
    gpu::Device* device = nullptr;
    RenderSettings settings;
    ActiveEffects effects;
    bool effectsChosen = false;
    std::vector<std::string> fallbacks; // Renderer::fallbacks
    RenderSettings fallbacksSettings;   // the settings `fallbacks` was made for
    DeviceSupport support;
    RenderStats stats;

    TextureFormat hdrFormat = TextureFormat::Invalid;
    TextureFormat depthFormat = TextureFormat::Invalid;
    TextureFormat shadowFormat = TextureFormat::Invalid;
    TextureFormat velocityFormat = TextureFormat::Rg16Float;
    TextureFormat transmittanceFormat = TextureFormat::Rgba16Float;
    TextureFormat ldrFormat = TextureFormat::Rgba8Unorm; // FXAA input; Rgba16Float for HDR output
    bool hdrOutputLogged = false;

    SurfaceEntry standard;
    SlotMap<SurfaceId, SurfaceEntry> surfaces;
    SlotMap<PassId, PassEntry> passes;
    std::vector<PassId> passOrder;
    std::uint32_t pipelineCount = 0;

    gpu::GraphicsPipeline shadowPipeline, skyPipeline, velocityPipeline, velocityObjPipeline, velocitySkinPipeline,
        velocityCutoutPipeline, velocitySkinCutoutPipeline, taaPipeline, bloomDownPipeline, bloomUpPipeline;
    std::array<gpu::GraphicsPipeline, 2> depthPipelines; // the ray tracing prepass of plain surfaces: [doubleSided]
    std::array<bool, 2> depthFailed{};
    gpu::ComputePipeline skinPipeline;      // empty: skinned instances are drawn in their bind pose (logged at init)
    gpu::ComputePipeline skinMorphPipeline; // blend shapes, then skinning
    std::map<TextureFormat, gpu::GraphicsPipeline> tonemapPipelines, fxaaPipelines;

    gpu::Sampler shadowSampler, linearClamp, pointClamp, materialSampler;
    gpu::Texture white, dummyShadow, dummyTransmittance, areaTable, noSkyCube;

    // Targets
    std::uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    ActiveEffects targetEffects;
    std::uint32_t targetShadowSize = 0;
    TextureFormat targetLdrFormat = TextureFormat::Invalid;
    gpu::Texture hdr, depth, velocity, ldr, shadowMap;
    gpu::Texture transmittanceMap; // made when a frame first has translucent casters
    gpu::Texture upscaled;         // Upscaling::Spatial and Temporal: the output-size image
    gpu::Upscaler upscaler;
    bool upscalerReset = true;
    std::array<gpu::Texture, 2> history;
    std::array<gpu::Texture, kBloomLevels> bloom;
    int bloomLevels = 0;
    bool targetsFailed = false;
    bool transmittanceFailed = false;

    // Per-frame buffers
    gpu::Buffer instanceBuffer, velocityBuffer;
    gpu::TransferBuffer transfer;
    std::uint32_t instanceCapacity = 0, velocityCapacity = 0;
    std::vector<shader::InstanceData> instanceData;
    std::vector<shader::VelocityInstance> velocityData;
    std::vector<glm::mat4> normalMatrices;
    std::vector<std::uint8_t> drawable;
    std::vector<Item> opaque, transparent, moved;
    std::array<std::vector<Item>, kMaxCascades> shadowItems, transmittanceItems;
    std::vector<Batch> opaqueBatches, transparentBatches, velocityBatches, velocityCutoutBatches;
    std::array<std::vector<Batch>, kMaxCascades> shadowBatches, transmittanceBatches;
    // Local light shadow maps (RenderSettings::localShadowMapSize, without ray tracing): one layer each.
    struct LocalShadowMap {
        glm::mat4 viewProj;
        std::array<Plane, 5> planes; // the sides and the far plane (the light's range)
    };
    std::vector<LocalShadowMap> localMaps;
    std::array<std::vector<Item>, kMaxLocalShadowMaps> localShadowItems;
    std::array<std::vector<Batch>, kMaxLocalShadowMaps> localShadowBatches;
    gpu::Texture localShadowMap;
    std::uint32_t localShadowFailedSize = 0; // the size whose maps could not be made

    // Skinning: every frame writes the skinned vertices of the instances it draws into skinned[frameIndex & 1], so the
    // other buffer still holds the previous frame's for the motion vectors.
    std::array<gpu::Buffer, 2> skinned;
    std::uint32_t skinnedCapacity = 0; // vertices
    bool skinnedReset = false;         // recreated this frame: no previous vertices
    gpu::Buffer paletteBuffer;
    gpu::TransferBuffer paletteTransfer;
    std::uint32_t paletteCapacity = 0;  // rows (float4)
    std::vector<glm::vec4> paletteData; // per job: its blend shape weights, then three rows per joint
    std::vector<SkinJob> skinJobs;
    std::vector<std::uint32_t> skinnedOffsets; // per instance slot; kNotSkinned
    std::uint32_t skinnedVertices = 0;
    std::uint32_t skinnedReusedCount = 0; // of the last skin(): jobs copied from the previous frame
    std::vector<gpu::Fence> skinFences;   // oldest first
    float skinningGpuMs = -1.0f;

    // Ray tracing (the RayTraced effects, the probe grid and the path tracer)
    RayTracing rayTracing;
    bool rayTracingTried = false;
    const ShaderTable* rayHitShaders = nullptr; // the surfaces' quench_add_ray_hits() set (SurfaceDesc)
    std::vector<RayCaster> rayCasters;
    PathTracer pathTracer;
    bool pathTracerTried = false;
    // The screen-space effects (AmbientOcclusion::ScreenSpace, and the fallbacks of the traced effects)
    ScreenSpace screenSpace;
    bool screenSpaceTried = false;
    // The clouds of the scene's weather (RenderSettings::clouds)
    CloudRenderer clouds;
    bool cloudsTried = false;
    bool targetVelocity = false; // the velocity target was made (TAA, or the temporal ray denoiser)
    // The light (scene slot + 1, 0: none) of each local shadow channel in the previous frame.
    std::array<std::uint32_t, QUENCH_RT_LOCAL_SHADOWS> shadowChannels{};

    // Temporal state
    std::uint64_t frameIndex = 0;
    // Frames since the last cut: the jitter, the shaders' noise and the probe rays follow it, so a frame depends only
    // on the frames since the cut, not on how many came before (in scripted runs, waits for loads in real time).
    std::uint64_t sequenceIndex = 0;
    glm::mat4 prevViewProj{1.0f};
    glm::vec3 prevCameraPos{0.0f}, prevCameraFwd{0.0f, 0.0f, -1.0f};
    bool prevValid = false;
    int historyIndex = 0;
    bool historyValid = false;

    Result<void> init();
    void applySettings();
    Result<gpu::GraphicsPipeline> createPipeline(const gpu::GraphicsPipelineDesc& desc);
    const gpu::GraphicsPipeline* surfacePipeline(SurfaceEntry& surface, std::uint32_t variant);
    const gpu::GraphicsPipeline* surfaceShadowPipeline(SurfaceEntry& surface, bool doubleSided);
    const gpu::GraphicsPipeline* surfaceMaskPipeline(SurfaceEntry& surface);
    const gpu::GraphicsPipeline* surfaceTransmittancePipeline(SurfaceEntry& surface, bool doubleSided);
    const gpu::GraphicsPipeline* surfaceDepthPipeline(SurfaceEntry& surface, bool doubleSided, bool cutOut);
    const gpu::GraphicsPipeline* surfaceGbufferPipeline(SurfaceEntry& surface, bool doubleSided);
    bool rayVelocity() const; // the temporal ray denoiser needs motion vectors
    const gpu::GraphicsPipeline* outputPipeline(std::map<TextureFormat, gpu::GraphicsPipeline>& cache,
                                                const char* entry, TextureFormat format);
    bool ensureTargets(std::uint32_t rw, std::uint32_t rh, std::uint32_t ow, std::uint32_t oh, TextureFormat ldrTarget);
    bool ensureTransmittance();
    bool ensureLocalShadowMaps(std::uint32_t size, std::uint32_t layers);
    bool ensureBuffers(gpu::CommandBuffer& commands);
    std::uint32_t skinnedOffset(const InstanceEntry& inst, std::uint32_t slot, const MeshEntry& mesh);
    bool skin(gpu::CommandBuffer& commands);
    gpu::BufferBinding skinnedVerticesOf(std::uint32_t slot) const;
    void runPasses(PassStage stage, PassContext& context, const shader::FrameUniforms& frame);
    void buildBatches(const SceneImpl& scene, std::vector<Item>& items, std::vector<Batch>& batches);
    // localMap: a local light's shadow map, where translucent casters are always dithered.
    void bindMaterial(gpu::RenderPass& pass, const SceneImpl& scene, const MaterialEntry& material, bool shadowPass,
                      bool localMap = false);
    void render(SceneImpl& scene, const View& view, gpu::CommandBuffer& commands, const gpu::TextureRef& target,
                const gpu::DisplayOutput& output);
};

Result<gpu::GraphicsPipeline> RendererImpl::createPipeline(const gpu::GraphicsPipelineDesc& desc) {
    Result<gpu::GraphicsPipeline> pipeline = device->createPipeline(desc);
    if (pipeline) ++pipelineCount;
    return pipeline;
}

Result<void> RendererImpl::init() {
    const gpu::Caps& caps = device->caps();
    const auto supports = [this](TextureFormat f, gpu::TextureType t, TextureUsage u) {
        return device->supportsFormat(f, t, u);
    };
    const TextureUsage targetSampled = TextureUsage::ColorTarget | TextureUsage::Sampled;
    const TextureUsage depthSampled = TextureUsage::DepthStencilTarget | TextureUsage::Sampled;
    for (const TextureFormat f : {TextureFormat::Rgba16Float, TextureFormat::Rg11b10Ufloat})
        if (supports(f, gpu::TextureType::Texture2D, targetSampled)) {
            hdrFormat = f;
            break;
        }
    if (hdrFormat == TextureFormat::Invalid) return Error{"render: no HDR color format (Rgba16Float, Rg11b10Ufloat)"};
    for (const TextureFormat f : {TextureFormat::Depth32Float, TextureFormat::Depth16Unorm})
        if (supports(f, gpu::TextureType::Texture2D, depthSampled)) {
            depthFormat = f;
            support.sampledDepth = true;
            break;
        }
    if (depthFormat == TextureFormat::Invalid)
        for (const TextureFormat f :
             {TextureFormat::Depth32Float, TextureFormat::Depth24UnormStencil8, TextureFormat::Depth16Unorm})
            if (supports(f, gpu::TextureType::Texture2D, TextureUsage::DepthStencilTarget)) {
                depthFormat = f;
                break;
            }
    if (depthFormat == TextureFormat::Invalid) return Error{"render: no depth format"};
    for (const TextureFormat f : {TextureFormat::Depth32Float, TextureFormat::Depth16Unorm})
        if (supports(f, gpu::TextureType::Texture2DArray, depthSampled)) {
            shadowFormat = f;
            support.shadowArray = true;
            break;
        }
    support.depthClamp = caps.depthClamp;
    support.velocityTarget = supports(velocityFormat, gpu::TextureType::Texture2D, targetSampled);
    support.transmittanceArray = supports(transmittanceFormat, gpu::TextureType::Texture2DArray, targetSampled);
    support.rayTracing =
        caps.rayTracing && support.sampledDepth && shaders::render_raytrace.find("rt_shadow_fs") != nullptr;
    support.rayLighting = support.rayTracing && shaders::render_raylight.find("rt_reflect_fs") != nullptr;
    support.screenSpace = support.sampledDepth &&
                          supports(TextureFormat::Rg16Float, gpu::TextureType::Texture2D, targetSampled) &&
                          supports(TextureFormat::Rgba16Float, gpu::TextureType::Texture2D, targetSampled);
    support.spatialUpscaling = caps.spatialUpscaling;
    support.temporalUpscaling = caps.temporalUpscaling;
    support.denoisedUpscaling = caps.denoisedUpscaling;
    support.cloudLayer = true; // until its pipelines fail (made when clouds are first drawn)
    support.cloudVolume = CloudRenderer::volumeSupported(*device);
    support.skyCube = CloudRenderer::cubeSupported(*device);
    if (!supports(ldrFormat, gpu::TextureType::Texture2D, targetSampled))
        return Error{"render: Rgba8Unorm cannot be rendered to and sampled"};

    // Samplers and placeholder textures.
    struct SamplerSpec {
        gpu::Sampler* out;
        gpu::SamplerDesc desc;
    };
    const SamplerSpec samplers[] = {
        // Shadow maps are read with Gather and compared in the shader: MoltenVK cannot bind comparison samplers
        // through SDL GPU (its portability subset has no mutableComparisonSamplers).
        {&shadowSampler,
         {.minFilter = gpu::Filter::Nearest,
          .magFilter = gpu::Filter::Nearest,
          .mipmapMode = gpu::MipmapMode::Nearest,
          .addressU = gpu::AddressMode::ClampToEdge,
          .addressV = gpu::AddressMode::ClampToEdge,
          .addressW = gpu::AddressMode::ClampToEdge,
          .maxLod = 0.0f,
          .name = "render shadow"}},
        {&linearClamp,
         {.mipmapMode = gpu::MipmapMode::Nearest,
          .addressU = gpu::AddressMode::ClampToEdge,
          .addressV = gpu::AddressMode::ClampToEdge,
          .addressW = gpu::AddressMode::ClampToEdge,
          .maxLod = 0.0f,
          .name = "render linear"}},
        {&pointClamp,
         {.minFilter = gpu::Filter::Nearest,
          .magFilter = gpu::Filter::Nearest,
          .mipmapMode = gpu::MipmapMode::Nearest,
          .addressU = gpu::AddressMode::ClampToEdge,
          .addressV = gpu::AddressMode::ClampToEdge,
          .addressW = gpu::AddressMode::ClampToEdge,
          .maxLod = 0.0f,
          .name = "render point"}},
        {&materialSampler, {.maxAnisotropy = 4.0f, .name = "render material"}},
    };
    for (const SamplerSpec& s : samplers) {
        Result<gpu::Sampler> sampler = device->createSampler(s.desc);
        if (!sampler) return Error{std::format("render: sampler: {}", sampler.error())};
        *s.out = std::move(*sampler);
    }
    Result<gpu::Texture> white1 = device->createTexture({.name = "render white"});
    if (!white1) return Error{std::format("render: placeholder texture: {}", white1.error())};
    white = std::move(*white1);
    const std::uint8_t whitePixel[4] = {255, 255, 255, 255};
    if (Result<void> r = device->upload(gpu::TextureRegion{.texture = white}, std::as_bytes(std::span(whitePixel))); !r)
        return Error{std::format("render: placeholder texture: {}", r.error())};
    // Bound in place of the cascades when there are none: a depth texture array, as the shader declares.
    const TextureFormat dummyFormat = support.shadowArray ? shadowFormat : TextureFormat::Invalid;
    if (dummyFormat == TextureFormat::Invalid) return Error{"render: no depth texture array the device can sample"};
    Result<gpu::Texture> dummy = device->createTexture({.type = gpu::TextureType::Texture2DArray,
                                                        .format = dummyFormat,
                                                        .usage = depthSampled,
                                                        .name = "render no shadows"});
    if (!dummy) return Error{std::format("render: placeholder shadow map: {}", dummy.error())};
    dummyShadow = std::move(*dummy);
    {
        gpu::CommandBuffer commands = device->acquireCommandBuffer();
        if (!commands) return Error{"render: no command buffer"};
        const gpu::DepthTarget clear{.texture = dummyShadow, .store = gpu::StoreOp::Store, .clearDepth = 1.0f};
        commands.beginRenderPass(std::span<const gpu::ColorTarget>{}, &clear).end();
        if (Result<void> r = commands.submit(); !r) return Error{std::format("render: {}", r.error())};
    }
    // Bound in place of the transmittance maps when no cascade has translucent casters (not read then).
    Result<gpu::Texture> noTransmittance = device->createTexture(
        {.type = gpu::TextureType::Texture2DArray, .format = ldrFormat, .name = "render no transmittance"});
    if (!noTransmittance) return Error{std::format("render: placeholder texture: {}", noTransmittance.error())};
    dummyTransmittance = std::move(*noTransmittance);
    const std::uint8_t clearPixel[4] = {255, 255, 255, 0};
    if (Result<void> r =
            device->upload(gpu::TextureRegion{.texture = dummyTransmittance}, std::as_bytes(std::span(clearPixel)));
        !r)
        return Error{std::format("render: placeholder texture: {}", r.error())};
    // Bound in place of the sky cube when there is none (surface sampler 15; not read then).
    Result<gpu::Texture> noCube = device->createTexture(
        {.type = gpu::TextureType::Cube, .format = ldrFormat, .layers = 6, .name = "render no sky cube"});
    if (!noCube) return Error{std::format("render: placeholder cube: {}", noCube.error())};
    noSkyCube = std::move(*noCube);
    for (std::uint32_t face = 0; face < 6; ++face)
        if (Result<void> r = device->upload(gpu::TextureRegion{.texture = noSkyCube, .layer = face},
                                            std::as_bytes(std::span(whitePixel)));
            !r)
            return Error{std::format("render: placeholder cube: {}", r.error())};
    // The area lights' table (quench/AreaLights.hlsli): layer 0 the inverse transforms, layer 1 the lobe's magnitude
    // and Fresnel part.
    Result<gpu::Texture> ltc = device->createTexture({.type = gpu::TextureType::Texture2DArray,
                                                      .format = TextureFormat::Rgba32Float,
                                                      .width = kLtcSize,
                                                      .height = kLtcSize,
                                                      .layers = 2,
                                                      .name = "render area lights"});
    if (!ltc) return Error{std::format("render: area light table: {}", ltc.error())};
    areaTable = std::move(*ltc);
    std::vector<float> amplitude(std::size_t(kLtcSize) * kLtcSize * 4, 0.0f);
    for (std::size_t i = 0; i < std::size_t(kLtcSize) * kLtcSize; ++i) {
        amplitude[i * 4] = kLtcAmplitude[i * 2];
        amplitude[i * 4 + 1] = kLtcAmplitude[i * 2 + 1];
    }
    if (Result<void> r =
            device->upload(gpu::TextureRegion{.texture = areaTable, .layer = 0}, std::as_bytes(std::span(kLtcMatrix)));
        !r)
        return Error{std::format("render: area light table: {}", r.error())};
    if (Result<void> r =
            device->upload(gpu::TextureRegion{.texture = areaTable, .layer = 1}, std::as_bytes(std::span(amplitude)));
        !r)
        return Error{std::format("render: area light table: {}", r.error())};

    // The engine's pipelines.
    const auto& frame = shaders::render_frame;
    const gpu::ColorTargetDesc hdrTarget{hdrFormat};
    const gpu::ColorTargetDesc hdrAdd{hdrFormat, gpu::BlendState::additive()};
    const gpu::ColorTargetDesc velocityTarget{velocityFormat};
    const auto make = [this](gpu::GraphicsPipeline& out, const gpu::GraphicsPipelineDesc& desc) -> Result<void> {
        Result<gpu::GraphicsPipeline> p = createPipeline(desc);
        if (!p) return Error{std::format("render: pipeline '{}': {}", desc.name, p.error())};
        out = std::move(*p);
        return {};
    };
    if (support.shadowArray) {
        if (Result<void> r =
                make(shadowPipeline, {.vertexShader = frame.find("shadow_vs"),
                                      .fragmentShader = frame.find("shadow_fs"),
                                      .vertexBuffers = {&kVertexLayout, 1},
                                      .vertexAttributes = kPositionAttribute,
                                      .rasterizer = shadowRasterizer(caps),
                                      .depth = {.test = true, .write = true, .compare = gpu::CompareOp::LessOrEqual},
                                      .depthFormat = shadowFormat,
                                      .name = "render shadow"});
            !r)
            return r;
    }
    const ShaderBlob* fullscreen = frame.find("fullscreen_vs");
    if (Result<void> r = make(skyPipeline, {.vertexShader = fullscreen,
                                            .fragmentShader = frame.find("sky_fs"),
                                            .depth = {.test = true, .write = false},
                                            .colorTargets = {&hdrTarget, 1},
                                            .depthFormat = depthFormat,
                                            .name = "render sky"});
        !r)
        return r;
    if (support.sampledDepth && support.velocityTarget) {
        if (Result<void> r = make(velocityPipeline, {.vertexShader = fullscreen,
                                                     .fragmentShader = frame.find("velocity_fs"),
                                                     .colorTargets = {&velocityTarget, 1},
                                                     .name = "render velocity"});
            !r)
            return r;
        if (Result<void> r = make(velocityObjPipeline, {.vertexShader = frame.find("velocity_obj_vs"),
                                                        .fragmentShader = frame.find("velocity_obj_fs"),
                                                        .vertexBuffers = {&kVertexLayout, 1},
                                                        .vertexAttributes = kPositionAttribute,
                                                        .depth = {.test = true, .write = false},
                                                        .colorTargets = {&velocityTarget, 1},
                                                        .depthFormat = depthFormat,
                                                        .name = "render velocity objects"});
            !r)
            return r;
        if (Result<void> r = make(velocitySkinPipeline, {.vertexShader = frame.find("velocity_skin_vs"),
                                                         .fragmentShader = frame.find("velocity_obj_fs"),
                                                         .vertexBuffers = kSkinVelocityLayouts,
                                                         .vertexAttributes = kSkinVelocityAttributes,
                                                         .depth = {.test = true, .write = false},
                                                         .colorTargets = {&velocityTarget, 1},
                                                         .depthFormat = depthFormat,
                                                         .name = "render velocity skinned"});
            !r)
            return r;
        if (Result<void> r = make(velocityCutoutPipeline, {.vertexShader = frame.find("velocity_obj_vs"),
                                                           .fragmentShader = frame.find("velocity_cutout_fs"),
                                                           .vertexBuffers = {&kVertexLayout, 1},
                                                           .vertexAttributes = kPositionAttribute,
                                                           .colorTargets = {&velocityTarget, 1},
                                                           .name = "render velocity cut-out"});
            !r)
            return r;
        if (Result<void> r = make(velocitySkinCutoutPipeline, {.vertexShader = frame.find("velocity_skin_vs"),
                                                               .fragmentShader = frame.find("velocity_cutout_fs"),
                                                               .vertexBuffers = kSkinVelocityLayouts,
                                                               .vertexAttributes = kSkinVelocityAttributes,
                                                               .colorTargets = {&velocityTarget, 1},
                                                               .name = "render velocity skinned cut-out"});
            !r)
            return r;
        if (Result<void> r = make(taaPipeline, {.vertexShader = fullscreen,
                                                .fragmentShader = frame.find("taa_fs"),
                                                .colorTargets = {&hdrTarget, 1},
                                                .name = "render taa"});
            !r)
            return r;
    }
    if (Result<void> r = make(bloomDownPipeline, {.vertexShader = fullscreen,
                                                  .fragmentShader = frame.find("bloom_down_fs"),
                                                  .colorTargets = {&hdrTarget, 1},
                                                  .name = "render bloom down"});
        !r)
        return r;
    if (Result<void> r = make(bloomUpPipeline, {.vertexShader = fullscreen,
                                                .fragmentShader = frame.find("bloom_up_fs"),
                                                .colorTargets = {&hdrAdd, 1},
                                                .name = "render bloom up"});
        !r)
        return r;

    Result<gpu::ComputePipeline> skinning =
        device->createComputePipeline({.shader = shaders::render_skin.find("skin_cs"), .name = "render skin"});
    Result<gpu::ComputePipeline> morphing = device->createComputePipeline(
        {.shader = shaders::render_skin.find("skin_morph_cs"), .name = "render skin blend shapes"});
    if (skinning && morphing) {
        skinPipeline = std::move(*skinning);
        skinMorphPipeline = std::move(*morphing);
    } else {
        log::error("render", "skinning pipeline: {}; skinned meshes are drawn in their bind pose",
                   skinning ? morphing.error() : skinning.error());
    }

    standard.vertex = shaders::render_standard.find("surface_vs");
    standard.fragment = shaders::render_standard.find("surface_fs");
    standard.basicFragment = shaders::render_standard.find("surface_basic_fs");
    standard.localShadowFragment = shaders::render_standard.find("surface_local_shadows_fs");
    standard.shadowFragment = shaders::render_standard.find("surface_shadow_fs");
    standard.transmittanceFragment = shaders::render_standard.find("surface_transmittance_fs");
    standard.gbufferFragment = shaders::render_standard.find("surface_gbuffer_fs");
    standard.name = "standard";
    if (!surfacePipeline(standard, variantOf(BlendMode::Opaque, false, Shading::Full)) ||
        !surfacePipeline(standard, variantOf(BlendMode::Opaque, false, Shading::Basic)))
        return Error{"render: the standard surface pipeline failed (logged)"};
    applySettings();
    log::info("render",
              "HDR {}, depth {}{}, shadows {}, velocity {}, depth clamp {}, depth bias clamp {}, ray tracing {}, "
              "upscaling {}",
              gpu::formatInfo(hdrFormat).name, gpu::formatInfo(depthFormat).name,
              support.sampledDepth ? " (sampled)" : "",
              support.shadowArray ? gpu::formatInfo(shadowFormat).name : "unsupported",
              support.velocityTarget ? "Rg16Float" : "unsupported", caps.depthClamp ? "yes" : "no",
              caps.depthBiasClamp ? "yes" : "no", support.rayTracing ? "yes" : "no",
              support.denoisedUpscaling   ? "spatial, temporal, temporal denoised"
              : support.temporalUpscaling ? "spatial, temporal"
              : support.spatialUpscaling  ? "spatial"
                                          : "bilinear");
    return {};
}

void RendererImpl::applySettings() {
    const ActiveEffects next = selectEffects(settings, support);
    if (next == effects && effectsChosen && settings == fallbacksSettings) return;
    fallbacksSettings = settings;
    const char* noRays = device->caps().rayTracing ? "this build has no ray query shaders (cmake/QuenchDxc.cmake)"
                                                   : "the device has no ray tracing (gpu::Caps::rayTracing)";
    fallbacks.clear();
    if (settings.sunShadows == SunShadows::RayTraced && settings.shadowCascades > 0 &&
        next.sunShadows != SunShadows::RayTraced)
        fallbacks.push_back(std::format("ray-traced shadows: {}; shadow maps", noRays));
    if (settings.ambientOcclusion == AmbientOcclusion::RayTraced && next.ambientOcclusion != settings.ambientOcclusion)
        fallbacks.push_back(
            std::format("ray-traced ambient occlusion: {}; {}", noRays,
                        next.ambientOcclusion == AmbientOcclusion::ScreenSpace ? "screen space (GTAO)" : "none"));
    if ((settings.ambientOcclusion == AmbientOcclusion::ScreenSpace ||
         (settings.ambientOcclusion == AmbientOcclusion::RayTraced && settings.screenSpaceFallbacks &&
          next.ambientOcclusion != AmbientOcclusion::RayTraced)) &&
        next.ambientOcclusion == AmbientOcclusion::None)
        fallbacks.emplace_back("screen-space ambient occlusion: needs a depth buffer to sample and 16-bit float "
                               "targets, or its pipelines failed; none");
    if (settings.localLightShadows > 0 && next.localLightShadows == 0)
        fallbacks.push_back(std::format("ray-traced local light shadows: {}; {}", noRays,
                                        next.localShadowMaps > 0 ? "shadow maps" : "unshadowed"));
    const char* noLighting = support.rayTracing ? "this build has no ray query shaders for traced lighting" : noRays;
    if (settings.reflections == Reflections::RayTraced && next.reflections != Reflections::RayTraced)
        fallbacks.push_back(
            std::format("ray-traced reflections: {}; {}", noLighting,
                        next.reflections == Reflections::ScreenSpace ? "screen space (SSR)" : "the sky's"));
    if ((settings.reflections == Reflections::ScreenSpace ||
         (settings.reflections == Reflections::RayTraced && settings.screenSpaceFallbacks &&
          next.reflections != Reflections::RayTraced)) &&
        next.reflections == Reflections::Sky)
        fallbacks.emplace_back("screen-space reflections: need a depth buffer to sample and 16-bit float targets, or "
                               "their pipelines failed; the sky's");
    if (settings.indirectLight != IndirectLight::Sky && next.indirectLight == IndirectLight::Sky)
        fallbacks.push_back(std::format("{}: {}; the sky's ambient light",
                                        settings.indirectLight == IndirectLight::Probes ? "the probe grid"
                                                                                        : "ray-traced indirect light",
                                        noLighting));
    if (settings.pathTracer && !next.pathTracer)
        fallbacks.push_back(std::format("path tracer: {}; the real-time frame", noLighting));
    if (settings.rayDenoiser == RayDenoiser::Temporal && next.rayDenoiser != RayDenoiser::Temporal)
        fallbacks.emplace_back("temporal ray denoiser: needs a sampled depth buffer and Rg16Float targets; spatial");
    if (settings.upscaling == Upscaling::Spatial && next.upscaling != Upscaling::Spatial && settings.renderScale < 1.0f)
        fallbacks.emplace_back("spatial upscaling: not on this device (gpu::Caps::spatialUpscaling); bilinear");
    if (settings.upscaling == Upscaling::TemporalDenoised && next.upscaling != Upscaling::TemporalDenoised &&
        !next.pathTracer)
        fallbacks.emplace_back(next.upscaling == Upscaling::Temporal
                                   ? "denoising upscaler: not on this device (gpu::Caps::denoisedUpscaling) or no ray "
                                     "tracing; temporal upscaling with the engine's denoiser"
                                   : "denoising upscaler: not on this device (gpu::Caps::denoisedUpscaling, "
                                     "temporalUpscaling, TAA's inputs); bilinear");
    if (settings.upscaling == Upscaling::Temporal && next.upscaling != Upscaling::Temporal && !next.pathTracer)
        fallbacks.emplace_back("temporal upscaling: not on this device (gpu::Caps::temporalUpscaling, TAA's inputs); "
                               "bilinear");
    if (next.pathTracer &&
        (settings.upscaling == Upscaling::Temporal || settings.upscaling == Upscaling::TemporalDenoised))
        fallbacks.emplace_back("temporal upscaling: not with the path tracer (it accumulates its own samples); "
                               "spatial or bilinear");
    const bool temporalUpscaler =
        next.upscaling == Upscaling::Temporal || next.upscaling == Upscaling::TemporalDenoised;
    if (settings.antiAliasing != next.antiAliasing && !temporalUpscaler && !next.pathTracer)
        fallbacks.emplace_back("anti-aliasing: TAA needs a sampled depth buffer and Rg16Float targets; using FXAA");
    if (settings.shadowCascades > 0 && next.shadowCascades == 0 && next.sunShadows == SunShadows::ShadowMaps)
        fallbacks.emplace_back("shadows off: no depth texture array the device can sample and render to");
    if (next.shadowCascades > 0 && settings.translucentShadows != next.translucentShadows)
        fallbacks.push_back(
            std::format("translucent shadows: no {} texture array to render to; dithered into the depth map",
                        gpu::formatInfo(transmittanceFormat).name));
    if (settings.sky && settings.clouds == Clouds::Volumetric && next.clouds == Clouds::Layer)
        fallbacks.push_back(std::format("volumetric clouds: no {} 3D texture and Rgba16Float targets to render to, or "
                                        "their pipelines failed; the 2D cloud layer",
                                        gpu::formatInfo(ldrFormat).name));
    if (settings.sky && settings.clouds != Clouds::Off && next.clouds == Clouds::Off)
        fallbacks.emplace_back("clouds: their pipelines failed; none");
    if (next.clouds != Clouds::Off && settings.skyCubeSize > 0 && !next.skyCube)
        fallbacks.emplace_back("sky cube: no Rgba16Float cube to render to; surfaces reflect the sky without clouds");
    if (next == effects && effectsChosen) return;
    for (const std::string& line : fallbacks) log::warning("render", "{}", line);
    effects = next;
    effectsChosen = true;
}

const gpu::GraphicsPipeline* RendererImpl::surfacePipeline(SurfaceEntry& surface, std::uint32_t variant) {
    if (surface.pipelines[variant]) return &surface.pipelines[variant];
    if (surface.failed[variant]) return nullptr;
    const auto blend = BlendMode(variant % 6 / 2);
    const bool doubleSided = variant % 2 != 0;
    const auto shading = Shading(variant / 6);
    const ShaderBlob* fragment = surface.fragment;
    if (shading == Shading::Basic && surface.basicFragment) fragment = surface.basicFragment;
    if (shading == Shading::LocalShadows && surface.localShadowFragment) fragment = surface.localShadowFragment;
    gpu::ColorTargetDesc color{hdrFormat};
    if (blend == BlendMode::AlphaBlend) color.blend = gpu::BlendState::alpha();
    if (blend == BlendMode::Additive) color.blend = gpu::BlendState::additive();
    const std::string name = std::format("surface {} ({})", surface.name, variant);
    Result<gpu::GraphicsPipeline> p = createPipeline({
        .vertexShader = surface.vertex,
        .fragmentShader = fragment,
        .vertexBuffers = {&kVertexLayout, 1},
        .vertexAttributes = kVertexAttributes,
        .rasterizer = {.cull = doubleSided ? gpu::CullMode::None : gpu::CullMode::Back},
        .depth = {.test = true, .write = blend == BlendMode::Opaque},
        .colorTargets = {&color, 1},
        .depthFormat = depthFormat,
        .name = name,
    });
    if (!p) {
        log::error("render", "pipeline '{}': {}", name, p.error());
        surface.failed[variant] = true;
        return nullptr;
    }
    surface.pipelines[variant] = std::move(*p);
    return &surface.pipelines[variant];
}

const gpu::GraphicsPipeline* RendererImpl::surfaceShadowPipeline(SurfaceEntry& surface, bool doubleSided) {
    if (!surface.shadowVertex) return &shadowPipeline;
    const int i = doubleSided ? 1 : 0;
    if (surface.shadowPipelines[std::size_t(i)]) return &surface.shadowPipelines[std::size_t(i)];
    if (surface.shadowFailed[std::size_t(i)]) return nullptr;
    const gpu::Caps& caps = device->caps();
    const std::string name = std::format("surface {} shadow", surface.name);
    Result<gpu::GraphicsPipeline> p = createPipeline({
        .vertexShader = surface.shadowVertex,
        .fragmentShader = shaders::render_frame.find("shadow_fs"),
        .vertexBuffers = {&kVertexLayout, 1},
        .vertexAttributes = kVertexAttributes,
        .rasterizer = shadowRasterizer(caps),
        .depth = {.test = true, .write = true, .compare = gpu::CompareOp::LessOrEqual},
        .depthFormat = shadowFormat,
        .name = name,
    });
    if (!p) {
        log::error("render", "pipeline '{}': {}", name, p.error());
        surface.shadowFailed[std::size_t(i)] = true;
        return nullptr;
    }
    surface.shadowPipelines[std::size_t(i)] = std::move(*p);
    return &surface.shadowPipelines[std::size_t(i)];
}

// Cut-out and dithered casters: the surface function runs to discard, so they take the full vertex and the material.
const gpu::GraphicsPipeline* RendererImpl::surfaceMaskPipeline(SurfaceEntry& surface) {
    if (surface.maskPipeline) return &surface.maskPipeline;
    if (surface.maskFailed) return nullptr;
    const std::string name = std::format("surface {} shadow mask", surface.name);
    Result<gpu::GraphicsPipeline> p = createPipeline({
        .vertexShader = surface.vertex,
        .fragmentShader = surface.shadowFragment ? surface.shadowFragment : standard.shadowFragment,
        .vertexBuffers = {&kVertexLayout, 1},
        .vertexAttributes = kVertexAttributes,
        .rasterizer = shadowRasterizer(device->caps()),
        .depth = {.test = true, .write = true, .compare = gpu::CompareOp::LessOrEqual},
        .depthFormat = shadowFormat,
        .name = name,
    });
    if (!p) {
        log::error("render", "pipeline '{}': {}", name, p.error());
        surface.maskFailed = true;
        return nullptr;
    }
    surface.maskPipeline = std::move(*p);
    return &surface.maskPipeline;
}

// Translucent casters in the transmittance map, without depth: rgb is multiplied (the light left after every caster
// it passes), alpha keeps the largest 1 - depth (the caster nearest to the sun).
const gpu::GraphicsPipeline* RendererImpl::surfaceTransmittancePipeline(SurfaceEntry& surface, bool doubleSided) {
    const std::size_t i = doubleSided ? 1 : 0;
    if (surface.transmittancePipelines[i]) return &surface.transmittancePipelines[i];
    if (surface.transmittanceFailed[i]) return nullptr;
    const gpu::ColorTargetDesc color{transmittanceFormat,
                                     {.enabled = true,
                                      .srcColor = gpu::BlendFactor::DstColor,
                                      .dstColor = gpu::BlendFactor::Zero,
                                      .srcAlpha = gpu::BlendFactor::One,
                                      .dstAlpha = gpu::BlendFactor::One,
                                      .alphaOp = gpu::BlendOp::Max}};
    const std::string name = std::format("surface {} transmittance ({})", surface.name, i);
    Result<gpu::GraphicsPipeline> p = createPipeline({
        .vertexShader = surface.vertex,
        .fragmentShader =
            surface.transmittanceFragment ? surface.transmittanceFragment : standard.transmittanceFragment,
        .vertexBuffers = {&kVertexLayout, 1},
        .vertexAttributes = kVertexAttributes,
        .rasterizer = {.cull = doubleSided ? gpu::CullMode::None : gpu::CullMode::Back,
                       .depthClip = !device->caps().depthClamp},
        .colorTargets = {&color, 1},
        .name = name,
    });
    if (!p) {
        log::error("render", "pipeline '{}': {}", name, p.error());
        surface.transmittanceFailed[i] = true;
        return nullptr;
    }
    surface.transmittancePipelines[i] = std::move(*p);
    return &surface.transmittancePipelines[i];
}

// The ray tracing prepass: the camera's depth of opaque surfaces. Plain ones write it with the position only (or the
// surface's vertex function); cut-out ones run the surface to discard, as in the cascades.
const gpu::GraphicsPipeline* RendererImpl::surfaceDepthPipeline(SurfaceEntry& surface, bool doubleSided, bool cutOut) {
    const std::size_t i = doubleSided ? 1 : 0;
    const bool custom = surface.shadowVertex != nullptr;
    gpu::GraphicsPipeline& out = cutOut   ? surface.depthMaskPipelines[i]
                                 : custom ? surface.depthPipelines[i]
                                          : depthPipelines[i];
    bool& failed = cutOut ? surface.depthMaskFailed[i] : custom ? surface.depthFailed[i] : depthFailed[i];
    if (out) return &out;
    if (failed) return nullptr;
    const std::string name =
        std::format("surface {} depth ({}{})", cutOut || custom ? surface.name : "plain", cutOut ? "cut-out, " : "", i);
    const bool full = cutOut || custom;
    Result<gpu::GraphicsPipeline> p = createPipeline({
        .vertexShader = full ? surface.vertex : shaders::render_frame.find("depth_vs"),
        .fragmentShader = cutOut ? (surface.shadowFragment ? surface.shadowFragment : standard.shadowFragment)
                                 : shaders::render_frame.find("shadow_fs"),
        .vertexBuffers = {&kVertexLayout, 1},
        .vertexAttributes = full ? std::span<const gpu::VertexAttribute>(kVertexAttributes)
                                 : std::span<const gpu::VertexAttribute>(kPositionAttribute),
        .rasterizer = {.cull = doubleSided ? gpu::CullMode::None : gpu::CullMode::Back},
        .depth = {.test = true, .write = true},
        .depthFormat = depthFormat,
        .name = name,
    });
    if (!p) {
        log::error("render", "pipeline '{}': {}", name, p.error());
        failed = true;
        return nullptr;
    }
    out = std::move(*p);
    return &out;
}

// The prepass with the G-buffer: the surface's vertex function and its G-buffer function (or the standard surface's,
// which reads the material), cut-outs discarded.
const gpu::GraphicsPipeline* RendererImpl::surfaceGbufferPipeline(SurfaceEntry& surface, bool doubleSided) {
    const std::size_t i = doubleSided ? 1 : 0;
    if (surface.gbufferPipelines[i]) return &surface.gbufferPipelines[i];
    if (surface.gbufferFailed[i]) return nullptr;
    const std::string name = std::format("surface {} gbuffer ({})", surface.name, i);
    const gpu::ColorTargetDesc colors[] = {{rayTracing.gbufferNormalFormat()}, {rayTracing.gbufferAlbedoFormat()}};
    Result<gpu::GraphicsPipeline> p = createPipeline({
        .vertexShader = surface.vertex,
        .fragmentShader = surface.gbufferFragment ? surface.gbufferFragment : standard.gbufferFragment,
        .vertexBuffers = {&kVertexLayout, 1},
        .vertexAttributes = kVertexAttributes,
        .rasterizer = {.cull = doubleSided ? gpu::CullMode::None : gpu::CullMode::Back},
        .depth = {.test = true, .write = true},
        .colorTargets = colors,
        .depthFormat = depthFormat,
        .name = name,
    });
    if (!p) {
        log::error("render", "pipeline '{}': {}", name, p.error());
        surface.gbufferFailed[i] = true;
        return nullptr;
    }
    surface.gbufferPipelines[i] = std::move(*p);
    return &surface.gbufferPipelines[i];
}

bool RendererImpl::rayVelocity() const {
    const bool traced = effects.sunShadows == SunShadows::RayTraced ||
                        effects.ambientOcclusion == AmbientOcclusion::RayTraced || effects.localLightShadows > 0 ||
                        effects.reflections == Reflections::RayTraced ||
                        effects.indirectLight == IndirectLight::RayTraced;
    return traced && !effects.pathTracer && effects.rayDenoiser == RayDenoiser::Temporal &&
           effects.upscaling != Upscaling::TemporalDenoised && support.velocityTarget;
}

const gpu::GraphicsPipeline* RendererImpl::outputPipeline(std::map<TextureFormat, gpu::GraphicsPipeline>& cache,
                                                          const char* entry, TextureFormat format) {
    if (auto it = cache.find(format); it != cache.end()) return it->second ? &it->second : nullptr;
    const gpu::ColorTargetDesc color{format};
    Result<gpu::GraphicsPipeline> p = createPipeline({.vertexShader = shaders::render_frame.find("fullscreen_vs"),
                                                      .fragmentShader = shaders::render_frame.find(entry),
                                                      .colorTargets = {&color, 1},
                                                      .name = entry});
    if (!p) {
        log::error("render", "pipeline '{}' for {}: {}", entry, gpu::formatInfo(format).name, p.error());
        cache.emplace(format, gpu::GraphicsPipeline{});
        return nullptr;
    }
    return &(cache[format] = std::move(*p));
}

bool RendererImpl::ensureTargets(std::uint32_t rw, std::uint32_t rh, std::uint32_t ow, std::uint32_t oh,
                                 TextureFormat ldrTarget) {
    const bool sizeSame = rw == renderWidth && rh == renderHeight && ow == outputWidth && oh == outputHeight;
    const bool needVelocity = effects.antiAliasing == AntiAliasing::Taa || rayVelocity();
    const bool effectsSame =
        targetEffects.antiAliasing == effects.antiAliasing && targetEffects.shadowCascades == effects.shadowCascades &&
        targetEffects.bloom == effects.bloom && targetEffects.upscaling == effects.upscaling &&
        targetShadowSize == settings.shadowMapSize && targetLdrFormat == ldrTarget && targetVelocity == needVelocity;
    if (sizeSame && effectsSame && hdr) return true;
    if (sizeSame && effectsSame && targetsFailed) return false;
    hdr = {};
    depth = {};
    velocity = {};
    ldr = {};
    shadowMap = {};
    transmittanceMap = {};
    transmittanceFailed = false;
    history = {};
    bloom = {};
    bloomLevels = 0;
    historyValid = false;
    upscaled = {};
    upscaler = {};
    upscalerReset = true;
    renderWidth = rw;
    renderHeight = rh;
    outputWidth = ow;
    outputHeight = oh;
    targetEffects = effects;
    targetShadowSize = settings.shadowMapSize;
    targetLdrFormat = ldrTarget;
    targetVelocity = needVelocity;
    targetsFailed = true;

    const auto create = [this](gpu::Texture& out, const gpu::TextureDesc& desc) {
        Result<gpu::Texture> t = device->createTexture(desc);
        if (!t) {
            log::error("render", "target '{}' {} x {}: {}", desc.name, desc.width, desc.height, t.error());
            return false;
        }
        out = std::move(*t);
        return true;
    };
    const TextureUsage targetSampled = TextureUsage::ColorTarget | TextureUsage::Sampled;
    if (!create(hdr, {.format = hdrFormat, .usage = targetSampled, .width = rw, .height = rh, .name = "render hdr"}))
        return false;
    TextureUsage depthUsage = TextureUsage::DepthStencilTarget;
    if (support.sampledDepth) depthUsage |= TextureUsage::Sampled;
    if (!create(depth, {.format = depthFormat, .usage = depthUsage, .width = rw, .height = rh, .name = "render depth"}))
        return false;
    if (needVelocity &&
        !create(
            velocity,
            {.format = velocityFormat, .usage = targetSampled, .width = rw, .height = rh, .name = "render velocity"}))
        return false;
    if (effects.antiAliasing == AntiAliasing::Taa) {
        if (effects.upscaling != Upscaling::Temporal && effects.upscaling != Upscaling::TemporalDenoised)
            for (gpu::Texture& h : history)
                if (!create(
                        h,
                        {.format = hdrFormat, .usage = targetSampled, .width = rw, .height = rh, .name = "render taa"}))
                    return false;
    }
    if (effects.antiAliasing == AntiAliasing::Fxaa &&
        !create(ldr, {.format = ldrTarget, .usage = targetSampled, .width = ow, .height = oh, .name = "render ldr"}))
        return false;
    if (effects.upscaling != Upscaling::Bilinear) {
        const bool denoised = effects.upscaling == Upscaling::TemporalDenoised;
        const bool temporal = effects.upscaling == Upscaling::Temporal || denoised;
        const gpu::UpscalerType type = denoised   ? gpu::UpscalerType::TemporalDenoised
                                       : temporal ? gpu::UpscalerType::Temporal
                                                  : gpu::UpscalerType::Spatial;
        Result<gpu::Upscaler> u = device->createUpscaler({.type = type,
                                                          .colorFormat = hdrFormat,
                                                          .outputFormat = hdrFormat,
                                                          .depthFormat = depthFormat,
                                                          .motionFormat = velocityFormat,
                                                          .diffuseAlbedoFormat = RayTracing::kAlbedoFormat,
                                                          .specularAlbedoFormat = RayTracing::kAlbedoFormat,
                                                          .normalFormat = RayTracing::gbufferNormalFormat(),
                                                          .roughnessFormat = RayTracing::kRoughnessFormat,
                                                          .inputWidth = rw,
                                                          .inputHeight = rh,
                                                          .outputWidth = ow,
                                                          .outputHeight = oh,
                                                          .name = "render upscaler"});
        if (!u) {
            // Not for this size or format: the next kind down (the denoising upscaler: the temporal one with the
            // engine's denoiser; else scaled up in tone mapping) from now on.
            const char* kind = denoised ? "denoising" : temporal ? "temporal" : "spatial";
            log::error("render", "{} upscaler {} x {} -> {} x {}: {}; {}", kind, rw, rh, ow, oh, u.error(),
                       denoised ? "temporal" : "bilinear");
            (denoised   ? support.denoisedUpscaling
             : temporal ? support.temporalUpscaling
                        : support.spatialUpscaling) = false;
            return false;
        }
        upscaler = std::move(*u);
        if (!create(upscaled, {.format = hdrFormat,
                               .usage = targetSampled | TextureUsage::ShaderWrite,
                               .width = ow,
                               .height = oh,
                               .name = "render upscaled"}))
            return false;
    }
    if (effects.bloom) {
        const bool atOutput = effects.upscaling != Upscaling::Bilinear;
        std::uint32_t w = atOutput ? ow : rw, h = atOutput ? oh : rh;
        for (int i = 0; i < kBloomLevels && std::min(w, h) >= 16; ++i) {
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
            if (!create(bloom[std::size_t(i)],
                        {.format = hdrFormat, .usage = targetSampled, .width = w, .height = h, .name = "render bloom"}))
                return false;
            bloomLevels = i + 1;
        }
    }
    if (effects.shadowCascades > 0 &&
        !create(shadowMap, {.type = gpu::TextureType::Texture2DArray,
                            .format = shadowFormat,
                            .usage = TextureUsage::DepthStencilTarget | TextureUsage::Sampled,
                            .width = settings.shadowMapSize,
                            .height = settings.shadowMapSize,
                            .layers = std::uint32_t(effects.shadowCascades),
                            .name = "render shadow cascades"}))
        return false;
    targetsFailed = false;
    return true;
}

// The cascades' transmittance maps, made with the first translucent caster after the targets; false: none (logged).
bool RendererImpl::ensureTransmittance() {
    if (transmittanceMap) return true;
    if (transmittanceFailed) return false;
    const std::uint32_t size = std::max(1u, settings.shadowMapSize / kTransmittanceScale);
    Result<gpu::Texture> t = device->createTexture({.type = gpu::TextureType::Texture2DArray,
                                                    .format = transmittanceFormat,
                                                    .usage = TextureUsage::ColorTarget | TextureUsage::Sampled,
                                                    .width = size,
                                                    .height = size,
                                                    .layers = std::uint32_t(effects.shadowCascades),
                                                    .name = "render shadow transmittance"});
    if (!t) {
        log::error("render", "target 'render shadow transmittance' {} x {}: {}; translucent casters cast no shadows",
                   size, size, t.error());
        transmittanceFailed = true;
        return false;
    }
    transmittanceMap = std::move(*t);
    return true;
}

// The local lights' shadow maps: grown to the layers a frame needs, made again when the size changes; false: none
// (logged once per size).
bool RendererImpl::ensureLocalShadowMaps(std::uint32_t size, std::uint32_t layers) {
    if (localShadowMap && localShadowMap.desc().width == size && localShadowMap.desc().layers >= layers) return true;
    if (localShadowFailedSize == size) return false;
    localShadowMap = {};
    Result<gpu::Texture> t = device->createTexture({.type = gpu::TextureType::Texture2DArray,
                                                    .format = shadowFormat,
                                                    .usage = TextureUsage::DepthStencilTarget | TextureUsage::Sampled,
                                                    .width = size,
                                                    .height = size,
                                                    .layers = layers,
                                                    .name = "render local shadow maps"});
    if (!t) {
        log::error("render", "target 'render local shadow maps' {} x {} x {}: {}; local lights unshadowed", size, size,
                   layers, t.error());
        localShadowFailedSize = size;
        return false;
    }
    localShadowMap = std::move(*t);
    return true;
}

bool RendererImpl::ensureBuffers(gpu::CommandBuffer& commands) {
    const auto grow = [](std::uint32_t capacity, std::size_t needed) {
        std::uint32_t c = std::max(capacity, 64u);
        while (c < needed) c *= 2;
        return c;
    };
    const std::uint32_t instances = grow(instanceCapacity, instanceData.size());
    const std::uint32_t velocities = grow(velocityCapacity, velocityData.size());
    if (instances != instanceCapacity || !instanceBuffer) {
        Result<gpu::Buffer> b = device->createBuffer({.usage = gpu::BufferUsage::GraphicsStorageRead,
                                                      .size = instances * std::uint32_t(sizeof(shader::InstanceData)),
                                                      .name = "render instances"});
        if (!b) {
            log::error("render", "instance buffer ({} instances): {}", instances, b.error());
            return false;
        }
        instanceBuffer = std::move(*b);
        instanceCapacity = instances;
        transfer = {};
    }
    if (velocities != velocityCapacity || !velocityBuffer) {
        Result<gpu::Buffer> b =
            device->createBuffer({.usage = gpu::BufferUsage::GraphicsStorageRead,
                                  .size = velocities * std::uint32_t(sizeof(shader::VelocityInstance)),
                                  .name = "render moved instances"});
        if (!b) {
            log::error("render", "velocity buffer ({} instances): {}", velocities, b.error());
            return false;
        }
        velocityBuffer = std::move(*b);
        velocityCapacity = velocities;
        transfer = {};
    }
    const std::uint32_t instanceBytes = instanceCapacity * std::uint32_t(sizeof(shader::InstanceData));
    const std::uint32_t velocityBytes = velocityCapacity * std::uint32_t(sizeof(shader::VelocityInstance));
    if (!transfer) {
        Result<gpu::TransferBuffer> t =
            device->createTransferBuffer({.size = instanceBytes + velocityBytes, .name = "render instances"});
        if (!t) {
            log::error("render", "instance transfer buffer: {}", t.error());
            return false;
        }
        transfer = std::move(*t);
    }
    const auto usedInstances = std::uint32_t(instanceData.size() * sizeof(shader::InstanceData));
    const auto usedVelocities = std::uint32_t(velocityData.size() * sizeof(shader::VelocityInstance));
    if (usedInstances + usedVelocities == 0) return true;
    std::span<std::byte> mapped = transfer.map(true);
    if (mapped.empty()) return false;
    std::memcpy(mapped.data(), instanceData.data(), usedInstances);
    std::memcpy(mapped.data() + instanceBytes, velocityData.data(), usedVelocities);
    transfer.unmap();
    gpu::CopyPass copy = commands.beginCopyPass();
    if (usedInstances > 0) copy.upload(transfer, 0, instanceBuffer, 0, usedInstances, true);
    if (usedVelocities > 0) copy.upload(transfer, instanceBytes, velocityBuffer, 0, usedVelocities, true);
    copy.end();
    return true;
}

// The instance's range of this frame's skinned vertices, given out when it is first drawn; kNotSkinned for rigid
// meshes without blend shapes and without the skinning pipeline.
std::uint32_t RendererImpl::skinnedOffset(const InstanceEntry& inst, std::uint32_t slot, const MeshEntry& mesh) {
    if (!deformed(mesh) || !skinPipeline) return kNotSkinned;
    std::uint32_t& offset = skinnedOffsets[slot];
    if (offset != kNotSkinned) return offset;
    offset = skinnedVertices;
    skinnedVertices += mesh.vertexCount;
    const bool unchanged = !inst.posed && inst.skinnedFrame != ~0ull && inst.skinnedFrame + 1 == frameIndex &&
                           inst.palette.size() == mesh.jointCount;
    const bool dualQuaternion = inst.desc.skinning == Skinning::DualQuaternion;
    SkinJob& job = skinJobs.emplace_back(SkinJob{&mesh, std::uint32_t(paletteData.size()), offset,
                                                 unchanged ? inst.skinnedOffset : kNotSkinned, dualQuaternion});
    for (std::size_t k = 0; k < inst.morphWeights.size(); ++k)
        if (inst.morphWeights[k] != 0.0f) {
            if (job.weightCount++ == 0) job.weightRow = std::uint32_t(paletteData.size());
            paletteData.push_back(glm::vec4(float(k), inst.morphWeights[k], 0.0f, 0.0f));
        }
    job.firstRow = std::uint32_t(paletteData.size());
    const auto row = [](const glm::mat4& m, int r) { return glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]); };
    if (inst.palette.size() != mesh.jointCount) {
        const glm::mat4 identity(1.0f);
        const std::array<glm::vec4, 3> rows = dualQuaternion
                                                  ? dualQuaternionRows(identity)
                                                  : std::array{row(identity, 0), row(identity, 1), row(identity, 2)};
        for (std::uint32_t j = 0; j < mesh.jointCount; ++j)
            paletteData.insert(paletteData.end(), rows.begin(), rows.end());
    } else if (dualQuaternion) {
        for (const glm::mat4& m : inst.palette) {
            const std::array<glm::vec4, 3> rows = dualQuaternionRows(m);
            paletteData.insert(paletteData.end(), rows.begin(), rows.end());
        }
    } else {
        for (const glm::mat4& m : inst.palette)
            paletteData.insert(paletteData.end(), {row(m, 0), row(m, 1), row(m, 2)});
    }
    return offset;
}

gpu::BufferBinding RendererImpl::skinnedVerticesOf(std::uint32_t slot) const {
    const std::uint32_t offset = skinnedOffsets[slot];
    if (offset == kNotSkinned) return {};
    return {skinned[frameIndex & 1], offset * std::uint32_t(sizeof(Vertex))};
}

// Uploads the palettes and poses the vertices of skinJobs. With GPU timing in its own command buffer, submitted
// before the frame's, whose GPU time is read from its fence a few frames later.
bool RendererImpl::skin(gpu::CommandBuffer& commands) {
    const bool timed = device->caps().gpuTiming;
    while (!skinFences.empty() && device->isSignaled(skinFences.front())) {
        if (const std::optional<double> ms = device->gpuMilliseconds(skinFences.front())) skinningGpuMs = float(*ms);
        skinFences.erase(skinFences.begin());
    }
    skinnedReset = false;
    skinnedReusedCount = 0;
    if (skinJobs.empty()) return true;
    const auto grow = [](std::uint32_t capacity, std::size_t needed) {
        std::uint32_t c = std::max(capacity, 1024u);
        while (c < needed) c *= 2;
        return c;
    };
    const std::uint32_t vertices = grow(skinnedCapacity, skinnedVertices);
    if (vertices != skinnedCapacity || !skinned[0]) {
        for (int i = 0; i < 2; ++i) {
            // Also read by the hit shading of ray-traced lighting (the posed normals).
            Result<gpu::Buffer> b =
                device->createBuffer({.usage = gpu::BufferUsage::Vertex | gpu::BufferUsage::ComputeStorageWrite |
                                               gpu::BufferUsage::GraphicsStorageRead,
                                      .size = vertices * std::uint32_t(sizeof(Vertex)),
                                      .name = i == 0 ? "render skinned vertices 0" : "render skinned vertices 1"});
            if (!b) {
                log::error("render", "skinned vertex buffer ({} vertices): {}", vertices, b.error());
                skinned = {};
                skinnedCapacity = 0;
                return false;
            }
            skinned[std::size_t(i)] = std::move(*b);
        }
        skinnedCapacity = vertices;
        skinnedReset = true;
    }
    const auto rows = std::uint32_t(paletteData.size());
    if (const std::uint32_t capacity = grow(paletteCapacity, rows); capacity != paletteCapacity || !paletteBuffer) {
        Result<gpu::Buffer> b = device->createBuffer({.usage = gpu::BufferUsage::ComputeStorageRead,
                                                      .size = capacity * std::uint32_t(sizeof(glm::vec4)),
                                                      .name = "render joint palettes"});
        Result<gpu::TransferBuffer> t = device->createTransferBuffer(
            {.size = capacity * std::uint32_t(sizeof(glm::vec4)), .name = "render joint palettes"});
        if (!b || !t) {
            log::error("render", "palette buffer ({} rows): {}", capacity, b ? t.error() : b.error());
            paletteCapacity = 0;
            return false;
        }
        paletteBuffer = std::move(*b);
        paletteTransfer = std::move(*t);
        paletteCapacity = capacity;
    }
    gpu::CommandBuffer own;
    if (timed) own = device->acquireCommandBuffer();
    gpu::CommandBuffer& target = own ? own : commands;
    // Empty when every job is a mesh without joints whose blend shapes are all at weight 0.
    if (const auto bytes = std::uint32_t(paletteData.size() * sizeof(glm::vec4)); bytes > 0) {
        std::span<std::byte> mapped = paletteTransfer.map(true);
        if (mapped.size() < bytes) return false;
        std::memcpy(mapped.data(), paletteData.data(), bytes);
        paletteTransfer.unmap();
        gpu::CopyPass copy = target.beginCopyPass();
        copy.upload(paletteTransfer, 0, paletteBuffer, 0, bytes, true);
    }
    // Unchanged palettes are copied from the previous frame's vertices, unless the buffers are new.
    const auto reused = [&](const SkinJob& job) { return !skinnedReset && job.reuseFrom != kNotSkinned; };
    {
        // Begun even when every job is a copy: it gives the buffer fresh memory (cycle) before the copies write it.
        const gpu::StorageBufferWrite write{.buffer = skinned[frameIndex & 1], .cycle = true};
        gpu::ComputePass pass = target.beginComputePass({&write, 1});
        if (!pass) return false;
        const gpu::ComputePipeline* boundPipeline = nullptr;
        const MeshEntry* bound = nullptr;
        for (const SkinJob& job : skinJobs) {
            if (reused(job)) {
                ++skinnedReusedCount;
                continue;
            }
            // Skinned meshes without weights take the plain entry; blend shapes (and meshes without joints, which
            // are written as they are) the other.
            const bool morph = job.weightCount > 0 || job.mesh->jointCount == 0;
            const gpu::ComputePipeline& pipeline = morph ? skinMorphPipeline : skinPipeline;
            if (&pipeline != boundPipeline) {
                pass.bindPipeline(pipeline);
                boundPipeline = &pipeline;
                bound = nullptr;
            }
            if (job.mesh != bound) {
                // A mesh without joints binds its morph offsets in the skin's slot (not read).
                const gpu::Buffer& skinBuffer = job.mesh->jointCount > 0 ? job.mesh->skin : job.mesh->morphs;
                const std::array<gpu::BufferRef, 4> inputs{job.mesh->vertices, skinBuffer, paletteBuffer,
                                                           job.mesh->morphs};
                pass.bindStorageBuffers(0, std::span(inputs).first(morph ? 4 : 3));
                bound = job.mesh;
            }
            pass.pushUniforms(0, shader::SkinUniforms{glm::vec4(float(job.mesh->vertexCount), float(job.firstRow),
                                                                float(job.offset), job.dualQuaternion ? 1.0f : 0.0f),
                                                      glm::vec4(float(job.weightCount), float(job.weightRow),
                                                                job.mesh->jointCount > 0 ? 1.0f : 0.0f, 0.0f)});
            pass.dispatch((job.mesh->vertexCount + kSkinGroup - 1) / kSkinGroup);
        }
    }
    if (skinnedReusedCount > 0) {
        gpu::CopyPass copy = target.beginCopyPass();
        constexpr auto stride = std::uint32_t(sizeof(Vertex));
        for (const SkinJob& job : skinJobs)
            if (reused(job))
                copy.copy(skinned[(frameIndex + 1) & 1], job.reuseFrom * stride, skinned[frameIndex & 1],
                          job.offset * stride, job.mesh->vertexCount * stride);
    }
    if (own) {
        Result<gpu::Fence> fence = own.submitWithFence();
        if (!fence) {
            log::error("render", "skinning: {}", fence.error());
            return false;
        }
        if (skinFences.size() >= kSkinTimingFences) skinFences.erase(skinFences.begin());
        skinFences.push_back(std::move(*fence));
    }
    return true;
}

void RendererImpl::runPasses(PassStage stage, PassContext& context, const shader::FrameUniforms& frame) {
    bool ran = false;
    for (const PassId id : passOrder) {
        PassEntry* entry = passes.get(id);
        if (!entry || entry->stage != stage) continue;
        context.commands.pushDebugGroup(entry->name.c_str());
        entry->fn(context);
        context.commands.popDebugGroup();
        ran = true;
    }
    if (!ran) return;
    if (context.pass) {
        context.pass->pushUniforms(kVertex, 0, frame);
        context.pass->pushUniforms(kFragment, 0, frame);
    } else {
        context.commands.pushUniforms(kVertex, 0, frame);
        context.commands.pushUniforms(kFragment, 0, frame);
    }
}

// Sorted items -> instance data and batches of equal pipeline, material and mesh.
void RendererImpl::buildBatches(const SceneImpl& scene, std::vector<Item>& items, std::vector<Batch>& batches) {
    batches.clear();
    for (const Item& item : items) {
        const InstanceEntry& inst = *scene.instances.slot(item.slot);
        const MeshEntry* mesh = scene.meshes.get(inst.desc.mesh);
        const MaterialEntry* material = item.material ? scene.materials.get(inst.desc.material) : nullptr;
        const gpu::BufferBinding skinnedVb = skinnedVerticesOf(item.slot);
        Batch* last = batches.empty() ? nullptr : &batches.back();
        if (!last || last->pipeline != item.pipeline || last->mesh != mesh || last->material != material ||
            skinnedVb.buffer || last->vertices.buffer)
            last = &batches.emplace_back(Batch{item.pipeline,
                                               mesh,
                                               material,
                                               std::uint32_t(instanceData.size()),
                                               0,
                                               skinnedVb,
                                               {},
                                               item.depth,
                                               item.depthMaterial});
        last->count++;
        instanceData.push_back({inst.desc.transform, normalMatrices[item.slot], inst.desc.color, inst.desc.custom});
    }
}

void RendererImpl::bindMaterial(gpu::RenderPass& pass, const SceneImpl& scene, const MaterialEntry& material,
                                bool shadowPass, bool localMap) {
    const MaterialDesc& m = material.desc;
    shader::MaterialUniforms u{};
    u.baseColor = m.baseColor;
    const bool dithered = shadowPass && shadowModeOf(m) == ShadowMode::Translucent &&
                          (localMap || effects.translucentShadows == TranslucentShadows::Dithered);
    u.emissive = glm::vec4(m.emissive, dithered ? 1.0f : 0.0f);
    std::array<gpu::TextureSamplerBinding, 4> bindings{};
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const TextureEntry* t = scene.textures.get(m.textures[i]);
        const bool bound = t && t->texture; // an empty texture (still loading) samples as unset
        bindings[i] = {bound ? gpu::TextureRef(t->texture) : gpu::TextureRef(white), materialSampler};
        if (bound) mask |= 1u << i;
    }
    u.surface = glm::vec4(m.roughness, m.metallic, m.alphaCutoff, float(mask));
    for (std::size_t i = 0; i < 4; ++i) u.params[i] = m.params[i];
    u.shading =
        glm::vec4(std::clamp(m.anisotropy, 0.0f, 1.0f), std::cos(m.anisotropyRotation), std::sin(m.anisotropyRotation),
                  material.light == 0      ? 0.0f
                  : material.lightTwoSided ? 2.0f
                                           : 1.0f);
    pass.pushUniforms(kFragment, 2, u);
    pass.bindSamplers(kFragment, 1, bindings);
}

void RendererImpl::render(SceneImpl& scene, const View& view, gpu::CommandBuffer& commands,
                          const gpu::TextureRef& target, const gpu::DisplayOutput& output) {
    stats = {};
    stats.pipelines = pipelineCount;
    const std::uint32_t ow = target.desc.width, oh = target.desc.height;
    if (!commands || !target || ow == 0 || oh == 0) return;
    bool hdrOut = output.composition == gpu::SwapchainComposition::HdrExtendedLinear;
    if ((output.composition == gpu::SwapchainComposition::Hdr10 ||
         (hdrOut && target.desc.format != TextureFormat::Rgba16Float)) &&
        !hdrOutputLogged) {
        log::warning("render", "{} output into {} is not supported: SDR values",
                     gpu::swapchainCompositionName(output.composition), gpu::formatInfo(target.desc.format).name);
        hdrOutputLogged = true;
    }
    hdrOut = hdrOut && target.desc.format == TextureFormat::Rgba16Float;
    const float scale = std::clamp(settings.renderScale, 0.25f, 1.0f);
    const auto rw = std::max(1u, std::uint32_t(std::lround(float(ow) * scale)));
    const auto rh = std::max(1u, std::uint32_t(std::lround(float(oh) * scale)));
    const bool sizeChanged = rw != renderWidth || rh != renderHeight;
    const TextureFormat ldrTarget = hdrOut ? TextureFormat::Rgba16Float : ldrFormat;
    if (!ensureTargets(rw, rh, ow, oh, ldrTarget)) {
        const ActiveEffects before = effects;
        applySettings(); // an upscaler that could not be made: bilinear
        if (effects == before || !ensureTargets(rw, rh, ow, oh, ldrTarget)) return;
    }
    stats.renderWidth = rw;
    stats.renderHeight = rh;
    // taa: the jittered projection and motion vectors; resolved by TAA or by the temporal upscaler.
    const bool taa = effects.antiAliasing == AntiAliasing::Taa;
    const bool upscaling = effects.upscaling != Upscaling::Bilinear;
    const bool taaResolve =
        taa && effects.upscaling != Upscaling::Temporal && effects.upscaling != Upscaling::TemporalDenoised;
    const bool fxaa = effects.antiAliasing == AntiAliasing::Fxaa;
    // Motion vectors: for TAA or the temporal upscaler, or for the temporal ray denoiser (drawn after the prepass).
    const bool rayTemporal = rayVelocity() && velocity;
    const bool motion = (taa || rayTemporal) && velocity;
    // The image after anti-aliasing and upscaling.
    const std::uint32_t iw = upscaling ? ow : rw, ih = upscaling ? oh : rh;
    commands.pushDebugGroup("quench render");

    // ---- Camera
    const float aspect = float(rw) / float(rh);
    const glm::mat4 proj = reversedPerspective(view.fovY, aspect, view.nearPlane);
    const glm::mat4 invView = glm::inverse(view.view);
    const glm::vec3 cameraPos(invView[3]);
    const glm::vec3 cameraFwd = -glm::normalize(glm::vec3(invView[2]));
    const bool cut = view.cut || !prevValid || sizeChanged || glm::length(cameraPos - prevCameraPos) > 6.0f ||
                     glm::dot(cameraFwd, prevCameraFwd) < 0.9f;
    if (cut) sequenceIndex = 0;
    glm::mat4 projJ = proj;
    if (taa) {
        const glm::vec2 j = subpixelJitter(sequenceIndex);
        projJ[2][0] += j.x * 2.0f / float(rw);
        projJ[2][1] += j.y * 2.0f / float(rh);
    }
    const glm::mat4 viewProjStable = proj * view.view;

    shader::FrameUniforms fu{};
    fu.viewProj = projJ * view.view;
    fu.invViewProj = glm::inverse(fu.viewProj);
    fu.viewProjStable = viewProjStable;
    fu.prevViewProj = cut ? viewProjStable : prevViewProj;
    fu.view = view.view;
    fu.cameraPos = glm::vec4(cameraPos, float(view.time));
    fu.cameraFwd = glm::vec4(cameraFwd, view.nearPlane);
    fu.screen = glm::vec4(float(rw), float(rh), 1.0f / float(rw), 1.0f / float(rh));

    // ---- Environment and lights
    const Environment& env = scene.environment;
    fu.skyZenith = glm::vec4(env.skyZenith, 0.0f);
    fu.skyHorizon = glm::vec4(env.skyHorizon, 0.0f);
    fu.skyGround = glm::vec4(env.skyGround, std::clamp(env.atmosphere, 0.0f, 1.0f));
    fu.sunGlow = glm::vec4(env.sunGlow, 0.0f);
    fu.skyAmbient = glm::vec4(env.skyAmbient, 0.0f);
    fu.groundAmbient = glm::vec4(env.groundAmbient, 0.0f);
    fu.fogParams = glm::vec4(std::max(env.fogDensity, 0.0f), std::max(env.fogFalloff, 0.0f), env.fogBaseHeight,
                             std::clamp(env.fogSunScatter, 0.0f, 1.0f));
    fu.fogColor = glm::vec4(env.fogColor, std::clamp(env.fogMaxAmount, 0.0f, 1.0f));
    fu.options = glm::vec4(env.exposure, float(sequenceIndex % 1024), taa ? 1.0f : 0.0f, 0.0f);

    const std::array<Plane, 5> planes = frustumPlanes(fu.viewProj);
    const LightEntry* sun = nullptr;
    struct Candidate {
        float score;
        shader::LocalLight light;
        bool shadows = false; // LightDesc::castsShadows
        bool area = false;
        std::uint32_t key = 0; // scene slot + 1
    };
    std::vector<Candidate> candidates;
    for (std::uint32_t i = 0; i < scene.lights.capacity(); ++i) {
        const LightEntry* light = scene.lights.slot(i);
        if (!light) continue;
        const LightDesc& l = light->desc;
        const glm::vec3 color = l.color * l.intensity;
        const glm::vec3 dir = glm::length(l.direction) > 0.0f ? glm::normalize(l.direction) : glm::vec3(0, -1, 0);
        if (l.type == LightType::Directional && !sun) {
            sun = light;
            continue;
        }
        if (std::max({color.r, color.g, color.b}) <= 0.0f) continue; // switched off
        if (l.type == LightType::Directional) {
            candidates.push_back(
                {-1.0f,
                 {glm::vec4(0.0f), glm::vec4(color, 0.0f), glm::vec4(dir, 0.0f), glm::vec4(0.0f), glm::vec4(0.0f)}});
            continue;
        }
        if (isAreaLight(l.type)) {
            const AreaLightFrame f = areaLightFrame(l);
            const float reach = l.range + f.radius;
            if (l.range <= 0.0f || !sphereVisible(planes, l.position, reach)) continue;
            shader::LocalLight local{};
            local.positionRange = glm::vec4(l.position, l.range);
            local.color = glm::vec4(color, float(QUENCH_LIGHT_RECT + int(l.type) - int(LightType::Rect)));
            local.direction = glm::vec4(f.normal, l.twoSided ? 1.0f : 0.0f);
            local.cone = glm::vec4(f.half.x, 0.0f, f.half.y, 0.0f);
            local.shape = glm::vec4(f.tangent, float(i + 1));
            candidates.push_back(
                {std::max(0.0f, glm::length(l.position - cameraPos) - reach), local, l.castsShadows, true, i + 1});
            continue;
        }
        if (l.range <= 0.0f || !sphereVisible(planes, l.position, l.range)) continue;
        shader::LocalLight local{};
        local.positionRange = glm::vec4(l.position, l.range);
        local.color = glm::vec4(color, l.type == LightType::Spot ? 2.0f : 1.0f);
        const float cosOuter = std::cos(l.outerAngle), cosInner = std::cos(std::min(l.innerAngle, l.outerAngle));
        local.direction = glm::vec4(dir, cosOuter);
        local.cone = glm::vec4(1.0f / std::max(cosInner - cosOuter, 1e-4f), 0.0f, std::max(l.sourceRadius, 0.0f), 0.0f);
        candidates.push_back(
            {std::max(0.0f, glm::length(l.position - cameraPos) - l.range), local, l.castsShadows, false, i + 1});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    // The nearest lights, of which at most QUENCH_MAX_AREA_LIGHTS area lights.
    std::size_t areaCount = 0;
    std::erase_if(candidates, [&areaCount](const Candidate& c) {
        return c.area && ++areaCount > std::size_t(QUENCH_MAX_AREA_LIGHTS);
    });
    shader::LightUniforms lu{};
    const auto lightCount = std::min<std::size_t>(candidates.size(), QUENCH_MAX_LIGHTS);
    areaCount = 0;
    for (std::size_t i = 0; i < lightCount; ++i) {
        lu.lights[i] = candidates[i].light;
        if (candidates[i].area) ++areaCount;
    }
    lu.count = glm::vec4(float(lightCount), float(areaCount), 0.0f, 0.0f);
    stats.lights = std::uint32_t(lightCount);
    stats.areaLights = std::uint32_t(areaCount);

    glm::vec3 towardSun(0.0f, 1.0f, 0.0f);
    if (sun) {
        const LightDesc& l = sun->desc;
        towardSun = glm::length(l.direction) > 0.0f ? -glm::normalize(l.direction) : glm::vec3(0, 1, 0);
        fu.sunDir = glm::vec4(towardSun, 1.0f);
        fu.sunColor = glm::vec4(l.color * l.intensity, 0.0f);
        fu.sunDisc = glm::vec4(env.sunDisc, std::cos(std::clamp(env.sunAngularRadius, 1e-4f, 0.5f)));
    }
    const glm::vec3 sunColor = sun ? sun->desc.color * sun->desc.intensity : glm::vec3(0.0f);
    const bool sunLights = std::max({sunColor.r, sunColor.g, sunColor.b}) > 0.0f;
    const bool sunShadows = sunLights && sun->desc.castsShadows && towardSun.y > -0.1f;
    // The path tracer replaces the frame's lighting: none of the other traced effects, no shadow maps.
    bool pathTracing = effects.pathTracer;
    const int cascades = sunShadows && !pathTracing ? effects.shadowCascades : 0;
    bool rayShadows = sunShadows && effects.sunShadows == SunShadows::RayTraced && !pathTracing;
    bool rayOcclusion = effects.ambientOcclusion == AmbientOcclusion::RayTraced && !pathTracing;
    int localShadows = pathTracing ? 0 : effects.localLightShadows;
    bool rayReflections = !pathTracing && effects.reflections == Reflections::RayTraced;
    bool probes = !pathTracing && effects.indirectLight != IndirectLight::Sky;
    const int probeCascades = std::clamp(settings.probeCascades, 1, kMaxProbeCascades);
    const std::uint32_t probeFlags = (settings.probeRelocation ? 1u : 0u) | (settings.probeClassification ? 2u : 0u);
    bool rayIndirect = !pathTracing && effects.indirectLight == IndirectLight::RayTraced;
    const auto anyTraced = [&] {
        return rayShadows || rayOcclusion || localShadows > 0 || rayReflections || probes || pathTracing;
    };
    if (anyTraced() && !rayTracing.available()) {
        if (!rayTracingTried)
            rayTracing.init(*device, pipelineCount,
                            {&pointClamp, &linearClamp, &materialSampler, &white, &dummyTransmittance, &areaTable});
        rayTracingTried = true;
        if (!rayTracing.available()) {
            support.rayTracing = support.rayLighting = false;
            applySettings();
            rayShadows = rayOcclusion = rayReflections = probes = rayIndirect = pathTracing = false;
            localShadows = 0;
        }
    }
    if ((rayReflections || probes || pathTracing) && !rayTracing.lighting()) {
        support.rayLighting = false; // the pipelines failed (logged)
        applySettings();
        rayReflections = probes = rayIndirect = pathTracing = false;
    }
    if (pathTracing && !pathTracer.ready()) {
        if (!pathTracerTried && !pathTracer.init(*device, rayTracing)) {
            support.rayLighting = false; // logged; the real-time frame from the next one
            applySettings();
        }
        pathTracerTried = true;
        if (!pathTracer.ready()) pathTracing = false;
    }
    if (!pathTracing) pathTracer.release();
    bool screenOcclusion = !pathTracing && effects.ambientOcclusion == AmbientOcclusion::ScreenSpace;
    bool screenReflections = !pathTracing && effects.reflections == Reflections::ScreenSpace;
    if ((screenOcclusion || screenReflections) && !screenSpace.ready()) {
        if (!screenSpaceTried && !screenSpace.init(*device, pipelineCount, pointClamp, linearClamp)) {
            support.screenSpace = false; // logged
            applySettings();
        }
        screenSpaceTried = true;
        screenOcclusion = screenOcclusion && screenSpace.ready();
        screenReflections = screenReflections && screenSpace.ready();
    }
    // The clouds of the scene's weather: their pipelines when they are first drawn.
    Clouds cloudMode = scene.weather ? effects.clouds : Clouds::Off;
    if (cloudMode != Clouds::Off && !clouds.ready()) {
        if (!cloudsTried && !clouds.init(*device, hdrFormat, depthFormat, pipelineCount)) {
            support.cloudLayer = support.cloudVolume = false; // logged
            applySettings();
        }
        cloudsTried = true;
        if (!clouds.ready()) cloudMode = Clouds::Off;
    }
    // Local lights with ray-traced shadows: the nearest ones that cast shadows, one channel each.
    int channels = 0;
    std::array<std::uint32_t, QUENCH_RT_LOCAL_SHADOWS> owners{};
    for (std::size_t i = 0; i < lightCount && channels < localShadows; ++i) {
        shader::LocalLight& l = lu.lights[i];
        if (int(l.color.w) == 0 || !candidates[i].shadows) continue;
        owners[std::size_t(channels)] = candidates[i].key;
        l.cone.y = float(++channels);
    }
    for (std::size_t c = 0; c < owners.size(); ++c)
        if (owners[c] != 0 && owners[c] != shadowChannels[c]) ++stats.localShadowChanges;
    shadowChannels = owners;
    localShadows = channels;
    // Without ray tracing: shadow maps for the nearest lights that cast shadows.
    localMaps.clear();
    const int mapLights = pathTracing ? 0 : effects.localShadowMaps;
    const std::uint32_t mapSize = std::clamp(settings.localShadowMapSize, 64u, 4096u);
    const float cubeTan = 1.0f + 2.0f * kLocalShadowEdge / float(mapSize);
    for (std::size_t i = 0; i < lightCount && int(stats.localShadowLights) < mapLights; ++i) {
        shader::LocalLight& l = lu.lights[i];
        const int type = int(l.color.w);
        if (type == 0 || !candidates[i].shadows) continue;
        const glm::vec3 origin(l.positionRange);
        const auto addMap = [&](const glm::vec3& forward, const glm::vec3& up, float tanHalf, float near, float far) {
            const glm::mat4 viewProj = glm::perspectiveRH_ZO(2.0f * std::atan(tanHalf), 1.0f, near, far) *
                                       glm::lookAtRH(origin, origin + forward, up);
            localMaps.push_back({viewProj, frustumPlanes(viewProj)});
        };
        if (candidates[i].area) {
            // A cube in the light's frame (quenchAreaShadow, quench/Shadows.hlsli): its faces and up vectors as a
            // point light's in that frame, none behind a one-sided rect or disk.
            const glm::vec3 x(l.shape), y(l.direction), z = glm::cross(x, y);
            const float radius = type == QUENCH_LIGHT_DISK ? l.cone.x : glm::length(glm::vec2(l.cone.x, l.cone.z));
            const float far = l.positionRange.w + radius;
            const bool oneSided = type != QUENCH_LIGHT_TUBE && l.direction.w < 0.5f;
            l.cone.y = float(stats.localShadowLights + 1);
            l.cone.w = float(localMaps.size() + 1);
            for (int f = 0; f < 6; ++f) {
                if (oneSided && f == 3) continue;
                glm::vec3 forward(0.0f);
                forward[f / 2] = f % 2 == 0 ? 1.0f : -1.0f;
                const glm::vec3 up = f / 2 == 1 ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
                addMap(x * forward.x + y * forward.y + z * forward.z, x * up.x + y * up.y + z * up.z, cubeTan,
                       far * float(QUENCH_AREA_SHADOW_NEAR), far);
            }
            ++stats.localShadowLights;
            continue;
        }
        const float range = l.positionRange.w;
        const float near = std::min(std::max(l.cone.z, kLocalShadowNear * range), 0.5f * range);
        const bool single = type == 2 && l.direction.w > kSpotMapMinCos;
        const float spotTan = single ? std::tan(std::acos(l.direction.w)) * cubeTan : 0.0f;
        l.shape = glm::vec4(float(localMaps.size() + 1), spotTan, near, 0.0f);
        l.cone.y = float(stats.localShadowLights + 1); // a channel, as the traced shadows have
        if (single) {
            // The up vector of quenchLocalShadow (quench/Shadows.hlsli).
            const glm::vec3 forward(l.direction);
            addMap(forward, std::abs(forward.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0), spotTan, near,
                   range);
        } else {
            // +x, -x, +y, -y, +z, -z with quenchLocalShadow's up vectors.
            for (int f = 0; f < 6; ++f) {
                glm::vec3 forward(0.0f);
                forward[f / 2] = f % 2 == 0 ? 1.0f : -1.0f;
                addMap(forward, f / 2 == 1 ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0), cubeTan, near, range);
            }
        }
        ++stats.localShadowLights;
    }
    if (!localMaps.empty() && !ensureLocalShadowMaps(mapSize, std::uint32_t(localMaps.size()))) {
        for (std::size_t i = 0; i < lightCount; ++i) {
            shader::LocalLight& l = lu.lights[i];
            if (candidates[i].area ? l.cone.w > 0.0f : l.shape.x > 0.0f) {
                if (candidates[i].area)
                    l.cone.w = 0.0f;
                else
                    l.shape = glm::vec4(0.0f);
                l.cone.y = 0.0f;
            }
        }
        localMaps.clear();
        stats.localShadowLights = 0;
    }
    if (mapLights == 0) localShadowMap = {};
    // With an area light among them, every map is read in a screen-space pass (ScreenSpace::resolveLocalShadows): the
    // area lights' soft filter is too costly per surface fragment. Without its pipelines the lights are unshadowed.
    bool screenLocal = false;
    for (std::size_t i = 0; i < lightCount && !localMaps.empty(); ++i)
        screenLocal = screenLocal || (candidates[i].area && lu.lights[i].cone.w > 0.0f);
    if (screenLocal && !screenSpace.ready()) {
        if (!screenSpaceTried && !screenSpace.init(*device, pipelineCount, pointClamp, linearClamp)) {
            support.screenSpace = false; // logged
            applySettings();
        }
        screenSpaceTried = true;
        screenOcclusion = screenOcclusion && screenSpace.ready();
        screenReflections = screenReflections && screenSpace.ready();
        if (!screenSpace.ready()) {
            for (std::size_t i = 0; i < lightCount; ++i) {
                lu.lights[i].cone.y = 0.0f;
                if (candidates[i].area) lu.lights[i].cone.w = 0.0f;
            }
            localMaps.clear();
            stats.localShadowLights = 0;
            screenLocal = false;
        }
    }
    stats.localShadowMaps = std::uint32_t(localMaps.size());
    if (!localMaps.empty())
        fu.localShadows = glm::vec4(float(mapSize), 1.0f / float(mapSize), kLocalShadowNormalOffset, cubeTan);
    bool rayTraced = anyTraced();
    // Hits are shaded (not only tested for shadows): every visible instance is in the top level, with its material.
    const bool hitData = rayReflections || probes || pathTracing;
    const bool denoisedUpscale = effects.upscaling == Upscaling::TemporalDenoised;
    const bool gbuffer = rayReflections || rayIndirect || denoisedUpscale || screenReflections;
    if (!rayTraced) rayTracing.releaseTargets();
    // The screen-space effects read the ray tracing prepass's depth and G-buffer, or those of a prepass of their own.
    const bool screenPrepass = (screenOcclusion || screenReflections || screenLocal) && !rayTraced;
    if (!screenOcclusion && !screenReflections && !screenLocal) screenSpace.releaseTargets();
    Cascades shadow;
    if (cascades > 0)
        shadow = computeCascades(view.view, view.fovY, aspect, view.nearPlane, towardSun,
                                 {.count = cascades,
                                  .distance = settings.shadowDistance,
                                  .mapSize = settings.shadowMapSize,
                                  .splitMix = 0.72f,
                                  .casterDistance = 80.0f});
    fu.cascadeSplits = glm::vec4(1e9f);
    for (int c = 0; c < shadow.count; ++c) {
        fu.shadowViewProj[c] = shadow.viewProj[std::size_t(c)];
        fu.cascadeSplits[c] = shadow.splits[std::size_t(c)];
        fu.cascadeTexel[c] = shadow.texel[std::size_t(c)];
    }
    fu.shadowParams = glm::vec4(float(shadow.count), 1.0f / float(std::max(settings.shadowMapSize, 1u)),
                                float(std::clamp(settings.shadowFilter, 0, 2)), kShadowNormalOffset);

    // ---- Draw lists
    opaque.clear();
    transparent.clear();
    moved.clear();
    for (auto& s : shadowItems) s.clear();
    for (auto& s : transmittanceItems) s.clear();
    for (std::size_t f = 0; f < localMaps.size(); ++f) localShadowItems[f].clear();
    if (effects.translucentShadows != TranslucentShadows::Map) transmittanceMap = {};
    const std::uint32_t slots = scene.instances.capacity();
    normalMatrices.resize(slots);
    skinJobs.clear();
    paletteData.clear();
    skinnedVertices = 0;
    skinnedOffsets.assign(slots, kNotSkinned);
    rayCasters.clear();
    std::array<glm::vec2, kMaxCascades> lightScale{};
    std::array<float, kMaxCascades> lightScaleZ{};
    for (int c = 0; c < shadow.count; ++c) {
        const glm::mat4& m = shadow.viewProj[std::size_t(c)];
        lightScale[std::size_t(c)] = {glm::length(glm::vec3(m[0][0], m[1][0], m[2][0])),
                                      glm::length(glm::vec3(m[0][1], m[1][1], m[2][1]))};
        lightScaleZ[std::size_t(c)] = glm::length(glm::vec3(m[0][2], m[1][2], m[2][2]));
    }
    for (std::uint32_t i = 0; i < slots; ++i) {
        InstanceEntry* inst = scene.instances.slot(i);
        if (!inst || !inst->desc.visible) continue;
        MeshEntry* mesh = scene.meshes.get(inst->desc.mesh);
        const MaterialEntry* material = scene.materials.get(inst->desc.material);
        if (!mesh || !material) continue;
        const MaterialDesc& m = material->desc;
        SurfaceEntry* surface = surfaces.get(m.surface);
        const std::uint32_t surfaceIndex = surface ? m.surface.index + 1 : 0;
        if (!surface) surface = &standard;
        const glm::mat4& model = inst->desc.transform;
        const glm::vec3 center(model * glm::vec4(inst->center, 1.0f));
        const float radius = inst->radius * maxScale(model);
        const bool inView = sphereVisible(planes, center, radius);
        const bool needNormal = inView || ((cascades > 0 || !localMaps.empty()) && inst->desc.castsShadows);
        if (needNormal) normalMatrices[i] = glm::transpose(glm::inverse(model));
        if (inView) {
            const Shading shading =
                !localMaps.empty() && (!screenLocal || m.blend != BlendMode::Opaque) && surface->localShadowFragment
                    ? Shading::LocalShadows
                : areaCount == 0 && !(m.anisotropy > 0.0f) && material->light == 0 && surface->basicFragment
                    ? Shading::Basic
                    : Shading::Full;
            const std::uint32_t variant = variantOf(m.blend, m.doubleSided, shading);
            const gpu::GraphicsPipeline* pipeline = surfacePipeline(*surface, variant);
            if (pipeline) {
                const bool skinnedHere = skinnedOffset(*inst, i, *mesh) != kNotSkinned;
                Item item{0, glm::length(center - cameraPos), i, pipeline};
                if ((rayTraced || screenPrepass) && !pathTracing && m.blend == BlendMode::Opaque) {
                    const bool cutOut = m.alphaCutoff > 0.0f;
                    const bool withGbuffer = gbuffer && (rayTraced || screenReflections);
                    item.depth = withGbuffer ? surfaceGbufferPipeline(*surface, m.doubleSided)
                                             : surfaceDepthPipeline(*surface, m.doubleSided, cutOut);
                    item.depthMaterial = withGbuffer || cutOut;
                }
                if (m.blend == BlendMode::Opaque) {
                    item.key = (std::uint64_t(surfaceIndex & 0x7ff) << 53) | (std::uint64_t(variant) << 48) |
                               (std::uint64_t(inst->desc.material.index & 0xffffff) << 24) |
                               std::uint64_t(inst->desc.mesh.index & 0xffffff);
                    opaque.push_back(item);
                } else {
                    transparent.push_back(item);
                }
                if (motion && (inst->moved || (skinnedHere && inst->posed))) moved.push_back(item);
            }
        } else {
            ++stats.culled;
        }
        const ShadowMode shadowMode = shadowModeOf(m);
        const bool caster = inst->desc.castsShadows && shadowMode != ShadowMode::None;
        // The shapes of area lights are seen by path rays only (the other rays get their light by shading).
        const bool seen = material->light > 0 ? pathTracing : hitData && m.blend != BlendMode::Additive;
        if (rayTraced && (caster || seen)) {
            // Every caster is in the top level (and with hit shading every surface rays can see); skinned ones only
            // near the camera (they are posed for it).
            const bool skinnedMesh = deformed(*mesh) && skinPipeline;
            if (!skinnedMesh || glm::length(center - cameraPos) - radius < settings.shadowDistance) {
                if (skinnedMesh) skinnedOffset(*inst, i, *mesh);
                const bool translucent =
                    caster ? shadowMode == ShadowMode::Translucent : m.blend == BlendMode::AlphaBlend;
                rayCasters.push_back(
                    {i, inst, mesh, material, {}, caster, translucent, m.doubleSided, surface->rayHit});
            }
        }
        if (!inst->desc.castsShadows || shadowMode == ShadowMode::None) continue;
        // Plain casters write depth with the position only; cut-out ones (and translucent ones dithered into the
        // depth) run the surface to discard; translucent ones go into the transmittance maps.
        const std::uint64_t materialKey = std::uint64_t(inst->desc.material.index & 0xffffff) << 24;
        const std::uint64_t meshKey = std::uint64_t(inst->desc.mesh.index & 0xffffff);
        const bool translucent = shadowMode == ShadowMode::Translucent;
        if (!localMaps.empty()) {
            // The local lights' maps have no transmittance: translucent casters are dithered into them.
            Item local{0, 0.0f, i, nullptr, true};
            if (translucent || m.alphaCutoff > 0.0f) {
                local.pipeline = surfaceMaskPipeline(*surface);
                local.key =
                    (std::uint64_t(1) << 62) | (std::uint64_t(surfaceIndex & 0xfff) << 48) | materialKey | meshKey;
            } else {
                local.pipeline = surfaceShadowPipeline(*surface, m.doubleSided);
                local.key = (std::uint64_t(surface->shadowVertex ? surfaceIndex : 0) << 49) |
                            (std::uint64_t(m.doubleSided ? 1 : 0) << 48) | meshKey;
                local.material = false;
            }
            for (std::size_t f = 0; local.pipeline && f < localMaps.size(); ++f) {
                if (!sphereVisible(localMaps[f].planes, center, radius)) continue;
                skinnedOffset(*inst, i, *mesh);
                localShadowItems[f].push_back(local);
            }
        }
        if (cascades == 0) continue;
        const bool mapped = translucent && effects.translucentShadows == TranslucentShadows::Map;
        if (mapped && !ensureTransmittance()) continue;
        Item item{0, 0.0f, i, nullptr, true};
        if (mapped) {
            item.pipeline = surfaceTransmittancePipeline(*surface, m.doubleSided);
            item.key = (std::uint64_t(surfaceIndex & 0xfff) << 49) | (std::uint64_t(m.doubleSided ? 1 : 0) << 48) |
                       materialKey | meshKey;
        } else if (translucent || m.alphaCutoff > 0.0f) {
            item.pipeline = surfaceMaskPipeline(*surface);
            item.key = (std::uint64_t(1) << 62) | (std::uint64_t(surfaceIndex & 0xfff) << 48) | materialKey | meshKey;
        } else {
            item.pipeline = surfaceShadowPipeline(*surface, m.doubleSided);
            item.key = (std::uint64_t(surface->shadowVertex ? surfaceIndex : 0) << 49) |
                       (std::uint64_t(m.doubleSided ? 1 : 0) << 48) | meshKey;
            item.material = false;
        }
        if (!item.pipeline) continue;
        auto& lists = mapped ? transmittanceItems : shadowItems;
        for (int c = 0; c < shadow.count; ++c) {
            const glm::vec4 p = shadow.viewProj[std::size_t(c)] * glm::vec4(center, 1.0f);
            const glm::vec2 s = lightScale[std::size_t(c)];
            if (std::abs(p.x) > 1.0f + radius * s.x || std::abs(p.y) > 1.0f + radius * s.y ||
                p.z > 1.0f + radius * lightScaleZ[std::size_t(c)])
                continue;
            skinnedOffset(*inst, i, *mesh); // every caster path draws the skinned vertices
            lists[std::size_t(c)].push_back(item);
        }
    }
    if (!skin(commands)) std::fill(skinnedOffsets.begin(), skinnedOffsets.end(), kNotSkinned); // the bind pose
    for (RayCaster& c : rayCasters) c.skinned = skinnedVerticesOf(c.slot);
    stats.skinnedInstances = std::uint32_t(skinJobs.size());
    stats.skinnedVertices = skinnedVertices;
    stats.skinnedReused = skinnedReusedCount;
    stats.skinningGpuMs = skinningGpuMs;
    std::sort(opaque.begin(), opaque.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
    std::sort(transparent.begin(), transparent.end(),
              [](const Item& a, const Item& b) { return a.distance > b.distance; });
    instanceData.clear();
    buildBatches(scene, opaque, opaqueBatches);
    buildBatches(scene, transparent, transparentBatches);
    const auto byKey = [](const Item& a, const Item& b) { return a.key < b.key; };
    std::uint32_t transmittanceMask = 0;
    for (int c = 0; c < shadow.count; ++c) {
        const auto cascade = std::size_t(c);
        std::sort(shadowItems[cascade].begin(), shadowItems[cascade].end(), byKey);
        buildBatches(scene, shadowItems[cascade], shadowBatches[cascade]);
        stats.shadowInstances += std::uint32_t(shadowItems[cascade].size());
        std::sort(transmittanceItems[cascade].begin(), transmittanceItems[cascade].end(), byKey);
        buildBatches(scene, transmittanceItems[cascade], transmittanceBatches[cascade]);
        stats.transmittanceInstances += std::uint32_t(transmittanceItems[cascade].size());
        if (transmittanceItems[cascade].empty()) continue;
        transmittanceMask |= 1u << c;
        fu.transmittanceBias[c] =
            kTransmittanceBias * float(kTransmittanceScale) * shadow.texel[cascade] * lightScaleZ[cascade];
    }
    if (transmittanceMask != 0)
        fu.shadowTransmittance =
            glm::vec4(float(transmittanceMask), 1.0f / float(transmittanceMap.desc().width), 0.0f, 0.0f);
    for (std::size_t f = 0; f < localMaps.size(); ++f) {
        std::sort(localShadowItems[f].begin(), localShadowItems[f].end(), byKey);
        buildBatches(scene, localShadowItems[f], localShadowBatches[f]);
        stats.localShadowInstances += std::uint32_t(localShadowItems[f].size());
    }
    velocityData.clear();
    velocityBatches.clear();
    velocityCutoutBatches.clear();
    std::sort(moved.begin(), moved.end(), [&scene](const Item& a, const Item& b) {
        return scene.instances.slot(a.slot)->desc.mesh.index < scene.instances.slot(b.slot)->desc.mesh.index;
    });
    for (const Item& item : moved) {
        const InstanceEntry& inst = *scene.instances.slot(item.slot);
        const MeshEntry* mesh = scene.meshes.get(inst.desc.mesh);
        const MaterialEntry* material = scene.materials.get(inst.desc.material);
        const bool cutOut = material && material->desc.blend == BlendMode::Opaque && material->desc.alphaCutoff > 0.0f;
        std::vector<Batch>& batches = cutOut ? velocityCutoutBatches : velocityBatches;
        const gpu::BufferBinding current = skinnedVerticesOf(item.slot);
        if (current.buffer) {
            const bool hasPrevious = !skinnedReset && inst.skinnedFrame != ~0ull && inst.skinnedFrame + 1 == frameIndex;
            const gpu::BufferBinding previous =
                hasPrevious ? gpu::BufferBinding{skinned[(frameIndex & 1) ^ 1],
                                                 inst.skinnedOffset * std::uint32_t(sizeof(Vertex))}
                            : current;
            batches.push_back({cutOut ? &velocitySkinCutoutPipeline : &velocitySkinPipeline, mesh, nullptr,
                               std::uint32_t(velocityData.size()), 1, current, previous});
        } else {
            if (batches.empty() || batches.back().mesh != mesh || batches.back().vertices.buffer ||
                batches.back().first + batches.back().count != velocityData.size())
                batches.push_back({cutOut ? &velocityCutoutPipeline : &velocityObjPipeline, mesh, nullptr,
                                   std::uint32_t(velocityData.size()), 0});
            batches.back().count++;
        }
        velocityData.push_back({inst.desc.transform, inst.previous});
    }
    stats.instances = std::uint32_t(opaque.size() + transparent.size());
    stats.movedInstances = std::uint32_t(moved.size());
    // Timed ranges (gpu::Caps::gpuPassTiming): upload, accel, prepass, raytrace with rays and denoise (ray tracing),
    // cloud_noise (once), weather, clouds and sky_cube (clouds), shadows, local_shadows and local_filter (local light
    // shadow maps), hdr, taa, upscale, bloom, tonemap.
    commands.beginTiming("upload");
    const bool buffers = ensureBuffers(commands);
    commands.endTiming();
    if (!buffers) {
        commands.popDebugGroup();
        return;
    }

    // ---- 0. Ray tracing: the acceleration structures (skinned ones after the skinning pass) and the tables
    bool rayMask = false; // the top level and the prepass targets are ready
    if (rayTraced) {
        RayTracingStats rs;
        const RayTargets targets{.gbuffer = gbuffer,
                                 .local = localShadows > 0,
                                 .reflections = rayReflections,
                                 .indirect = rayIndirect,
                                 .probeCascades = probes ? probeCascades : 0,
                                 .probeData = probes && probeFlags != 0,
                                 .temporal = rayTemporal,
                                 .denoiseInputs = denoisedUpscale};
        commands.beginTiming("accel");
        rayMask = rayTracing.ensureTargets(rw, rh, depthFormat, targets);
        if (rayMask) {
            rayTracing.beginFrame();
            const gpu::Buffer& posed = skinned[frameIndex & 1];
            rayMask = rayTracing.build(commands, scene, rayCasters, hitData,
                                       skinJobs.empty() || !posed ? gpu::BufferRef{} : gpu::BufferRef(posed), rs);
        }
        commands.endTiming();
        stats.rayInstances = rs.instances;
        stats.rayBuilds = rs.builds;
        stats.rayRefits = rs.refits;
        stats.rayCompactions = rs.compactions;
        stats.rayTopRefit = rs.topRefit;
        stats.rayMemoryBytes = rs.memoryBytes;
        stats.hitTextures = rs.hitTextures;
        stats.rayHitSurfaces = rs.surfaceHits;
    }
    const bool pathFrame = pathTracing && rayMask && pathTracer.ensureTargets(rw, rh);
    std::uint32_t rayBits = 0;
    if (rayMask && !pathFrame) {
        rayBits = (rayShadows ? 1u : 0u) | (rayOcclusion ? 2u : 0u) | (localShadows > 0 ? 4u : 0u) |
                  (rayReflections ? 8u : 0u) | (rayIndirect ? 16u : 0u) | (probes ? 32u : 0u);
        if (probes) {
            const ProbeGridState grid = rayTracing.placeProbes(view.probeCenter.value_or(cameraPos),
                                                               settings.probeSpacing, probeCascades, probeFlags);
            float spacing = grid.spacing;
            for (std::size_t c = 0; c < std::size_t(grid.cascades); ++c, spacing *= 2.0f)
                fu.probeGrid[c] = glm::vec4(glm::vec3(grid.first[c]), spacing);
            fu.probeCounts = glm::vec4(glm::vec3(grid.counts), float(RayTracing::kIrradianceTile));
            fu.probeAtlas = glm::vec4(float(RayTracing::kDistanceTile), kProbeBias * grid.spacing, float(grid.cascades),
                                      float(grid.flags));
        }
        fu.rayLighting = glm::vec4(std::clamp(settings.reflectionRoughness, 0.05f, 1.0f), 0.0f, 0.0f, 0.0f);
    }
    // Screen-space occlusion: into alpha of the traced mask (its rgb the sun's when traced), else of its own mask.
    // Screen-space reflections: read where the previous frame's image is there to take the hits' color from.
    const bool tracedPrepass = rayMask && !pathFrame;
    const bool screenTargets = (screenOcclusion || screenReflections || screenLocal) &&
                               (tracedPrepass || screenPrepass) &&
                               screenSpace.ensureTargets({.width = rw,
                                                          .height = rh,
                                                          .depthFormat = depthFormat,
                                                          .imageFormat = hdrFormat,
                                                          .ownDepth = !tracedPrepass,
                                                          .occlusion = screenOcclusion,
                                                          .reflections = screenReflections,
                                                          .localShadows = screenLocal});
    const bool screenAo = screenOcclusion && screenTargets;
    const bool screenShadows = screenLocal && screenTargets;
    if (screenShadows) rayBits |= 4u;
    const bool screenHistory = screenReflections && screenTargets; // this frame's image is kept for the next
    const bool screenReflect = screenHistory && screenSpace.historyValid() && !cut;
    if (screenAo) rayBits |= 2u;
    if (screenReflect) {
        rayBits |= 64u;
        fu.rayLighting = glm::vec4(std::clamp(settings.reflectionRoughness, 0.05f, 1.0f), 0.0f, 0.0f, 0.0f);
    }
    stats.screenOcclusion = screenAo;
    stats.screenReflections = screenReflect;
    // ---- 0b. The clouds: the weather map, their shadow, the march and the sky cube; then their fields of the frame
    // uniforms and the light under them, for every pass after.
    clouds.render(commands, settings,
                  {.weather = scene.weather ? &*scene.weather : nullptr,
                   .mode = cloudMode,
                   .cube = effects.skyCube,
                   .renderWidth = rw,
                   .renderHeight = rh,
                   .viewProjStable = viewProjStable,
                   .prevViewProjStable = fu.prevViewProj,
                   .cut = cut,
                   .sequence = sequenceIndex},
                  fu, stats);
    fu.options.w = float(rayBits);
    commands.pushUniforms(kVertex, 0, fu);
    commands.pushUniforms(kFragment, 0, fu);
    commands.pushUniforms(kFragment, 1, lu);

    // cascade: the shadow map layer the batches are drawn into; -1: the camera's passes. prepass: the batches'
    // depth pipelines (the ray tracing prepass). localMap: a local light's shadow map (projected as cascade 0).
    const auto drawBatches = [&](gpu::RenderPass& pass, const std::vector<Batch>& batches, int cascade,
                                 std::uint32_t& counter, bool prepass = false, bool localMap = false) {
        const bool shadowPass = cascade >= 0;
        const gpu::GraphicsPipeline* pipeline = nullptr;
        const MaterialEntry* material = nullptr;
        const MeshEntry* mesh = nullptr;
        for (const Batch& b : batches) {
            const gpu::GraphicsPipeline* batchPipeline = prepass ? b.depth : b.pipeline;
            if (!batchPipeline) continue;
            if (batchPipeline != pipeline) {
                pass.bindPipeline(*batchPipeline);
                pipeline = batchPipeline;
            }
            const MaterialEntry* batchMaterial = prepass && !b.depthMaterial ? nullptr : b.material;
            if (batchMaterial && batchMaterial != material) {
                bindMaterial(pass, scene, *batchMaterial, shadowPass, localMap);
                material = batchMaterial;
            }
            if (b.vertices.buffer) {
                pass.bindVertexBuffer(0, b.vertices);
                if (b.previous.buffer) pass.bindVertexBuffer(1, b.previous);
                pass.bindIndexBuffer({b.mesh->indices}, gpu::IndexFormat::Uint32);
                mesh = nullptr;
            } else if (b.mesh != mesh) {
                pass.bindVertexBuffer(0, {b.mesh->vertices});
                pass.bindIndexBuffer({b.mesh->indices}, gpu::IndexFormat::Uint32);
                mesh = b.mesh;
            }
            const shader::DrawUniforms du{
                glm::vec4(float(b.first), shadowPass ? float(cascade) : 0.0f, shadowPass ? 1.0f : 0.0f, 0.0f)};
            pass.pushUniforms(kVertex, 1, du);
            pass.drawIndexed(b.mesh->indexCount, b.count);
            ++counter;
        }
    };
    const gpu::BufferRef instances(instanceBuffer);
    const gpu::TextureSamplerBinding noShadowBinding{dummyShadow, shadowSampler};

    // Motion vectors from a depth of this frame: the camera's motion, then the instances that moved.
    bool velocityDrawn = false;
    const auto drawVelocity = [&](const gpu::Texture& depthTexture) {
        {
            const gpu::ColorTarget color{.texture = velocity, .cycle = true};
            gpu::RenderPass pass = commands.beginRenderPass(color);
            if (pass) {
                pass.bindPipeline(velocityPipeline);
                pass.bindSampler(kFragment, 0, {depthTexture, pointClamp});
                pass.draw(3);
                pass.end();
            }
        }
        if (!velocityBatches.empty()) {
            const gpu::ColorTarget color{.texture = velocity, .load = gpu::LoadOp::Load};
            const gpu::DepthTarget dt{.texture = depthTexture, .load = gpu::LoadOp::Load, .store = gpu::StoreOp::Store};
            gpu::RenderPass pass = commands.beginRenderPass(color, &dt);
            if (pass) {
                const gpu::BufferRef movedBuffer(velocityBuffer);
                pass.bindStorageBuffers(kVertex, 0, {&movedBuffer, 1});
                std::uint32_t draws = 0;
                drawBatches(pass, velocityBatches, -1, draws);
                pass.end();
            }
        }
        if (!velocityCutoutBatches.empty()) {
            const gpu::ColorTarget color{.texture = velocity, .load = gpu::LoadOp::Load};
            gpu::RenderPass pass = commands.beginRenderPass(color);
            if (pass) {
                const gpu::BufferRef movedBuffer(velocityBuffer);
                pass.bindStorageBuffers(kVertex, 0, {&movedBuffer, 1});
                pass.bindSampler(kFragment, 0, {depthTexture, pointClamp});
                std::uint32_t draws = 0;
                drawBatches(pass, velocityCutoutBatches, -1, draws);
                pass.end();
            }
        }
        velocityDrawn = true;
    };

    // ---- 0. Ray tracing: the camera's depth (and G-buffer) of the opaque surfaces, then the traced effects
    shader::RayHitUniforms hu{};
    hu.params =
        glm::vec4(std::clamp(settings.reflectionRoughness, 0.05f, 1.0f), kRayNormalOffset, kRayLightLength, 0.0f);
    hu.rotation = frameRotation(sequenceIndex);
    bool denoiseInputs = false; // the denoising upscaler's G-buffer inputs are written
    if (denoisedUpscale && !(rayMask && !pathFrame)) denoiseInputs = rayTracing.writeDenoiseInputs(commands, false);
    if (rayMask && !pathFrame) {
        commands.beginTiming("prepass");
        {
            const gpu::DepthTarget dt{
                .texture = rayTracing.depth(), .store = gpu::StoreOp::Store, .clearDepth = 0.0f, .cycle = true};
            const gpu::ColorTarget colors[] = {{.texture = rayTracing.gbufferNormal(), .cycle = true},
                                               {.texture = rayTracing.gbufferAlbedo(), .cycle = true}};
            gpu::RenderPass pass = commands.beginRenderPass(
                gbuffer ? std::span<const gpu::ColorTarget>(colors) : std::span<const gpu::ColorTarget>{}, &dt);
            if (pass) {
                pass.bindStorageBuffers(kVertex, 0, {&instances, 1});
                pass.bindSampler(kFragment, 0, noShadowBinding);
                std::uint32_t draws = 0;
                drawBatches(pass, opaqueBatches, -1, draws, true);
                pass.end();
            }
        }
        if (rayTemporal) drawVelocity(rayTracing.depth());
        if (denoisedUpscale) denoiseInputs = rayTracing.writeDenoiseInputs(commands, true);
        commands.endTiming();
        const float cone = std::clamp(env.sunAngularRadius * std::max(settings.shadowSoftness, 0.0f), 0.0f, 0.2f);
        const shader::RayTracingUniforms ru{
            glm::vec4(rayShadows ? float(std::clamp(settings.shadowRays, 1, 8)) : 0.0f, std::tan(cone),
                      kRayShadowLength, kRayNormalOffset),
            glm::vec4(rayOcclusion ? float(std::clamp(settings.occlusionRays, 1, 8)) : 0.0f,
                      std::max(settings.occlusionRadius, 0.01f), std::clamp(settings.occlusionStrength, 0.0f, 1.0f),
                      0.0f),
            glm::vec4(cone > 0.0f && settings.shadowRays > 0 ? float(kRayDenoiseShadowRadius) : 0.0f,
                      kRayDenoiseTolerance, rayOcclusion ? float(kRayDenoiseOcclusionRadius) : 0.0f, 0.0f),
            glm::vec4(float(localShadows), 0.0f, 0.0f, 0.0f)};
        const RayFiltering filtering = effects.upscaling == Upscaling::TemporalDenoised ? RayFiltering::Raw
                                       : rayTemporal                                    ? RayFiltering::Temporal
                                                                                        : RayFiltering::Spatial;
        const FilterInputs inputs{.depth = rayTracing.depth(),
                                  .previousDepth = rayTracing.previousDepth(),
                                  .velocity = rayTemporal ? gpu::TextureRef(velocity) : gpu::TextureRef(white),
                                  .reset = cut};
        commands.beginTiming("raytrace");
        if (rayShadows || rayOcclusion || localShadows > 0)
            rayTracing.traceShadows(commands, ru, lu, inputs, filtering);
        if (probes) stats.probes = rayTracing.updateProbes(commands, hu, lu, cut, settings.probeUpdatePeriod);
        hu.params.w = probes && rayTracing.probesValid() ? 1.0f : 0.0f;
        if (rayReflections) rayTracing.traceReflections(commands, hu, lu, inputs, filtering);
        if (rayIndirect) rayTracing.traceIndirect(commands, hu, lu, inputs, filtering);
        commands.endTiming();
    }
    // ---- 0b. Screen-space effects: the depth (and G-buffer) of the opaque surfaces where nothing is traced (also read
    // by the local shadow maps' screen pass), then GTAO and the reflections. Positions are camera-relative (precise far
    // from the origin).
    const glm::mat4 relativeViewProj = projJ * glm::mat4(glm::mat3(view.view));
    if (screenPrepass && (screenAo || screenReflect || screenShadows)) {
        commands.beginTiming("prepass");
        const gpu::DepthTarget dt{
            .texture = screenSpace.depth(), .store = gpu::StoreOp::Store, .clearDepth = 0.0f, .cycle = true};
        const gpu::ColorTarget colors[] = {{.texture = screenSpace.gbufferNormal(), .cycle = true},
                                           {.texture = screenSpace.gbufferAlbedo(), .cycle = true}};
        gpu::RenderPass pass = commands.beginRenderPass(
            screenReflections ? std::span<const gpu::ColorTarget>(colors) : std::span<const gpu::ColorTarget>{}, &dt);
        if (pass) {
            pass.bindStorageBuffers(kVertex, 0, {&instances, 1});
            pass.bindSampler(kFragment, 0, noShadowBinding);
            drawBatches(pass, opaqueBatches, -1, stats.screenPrepassDraws, true);
            pass.end();
        }
        commands.endTiming();
    }
    if (screenAo || screenReflect) {
        // The camera rays at reversed depth 1 from the jittered projection and the view's rotation.
        const glm::mat4 invProj = glm::inverse(projJ);
        const glm::mat3 rotation(invView);
        const auto rayAt = [&](float x, float y) {
            const glm::vec4 v =
                invProj * glm::vec4(x / float(rw) * 2.0f - 1.0f, 1.0f - y / float(rh) * 2.0f, 1.0f, 1.0f);
            return rotation * (glm::vec3(v) / v.w);
        };
        const glm::vec3 rayOrigin = rayAt(0.0f, 0.0f);
        const float noiseFrame = taa ? float(sequenceIndex % 64) : 0.0f;
        const shader::ScreenSpaceUniforms su{
            glm::vec4(float(std::clamp(settings.occlusionRays, 1, 8)), kGtaoSteps,
                      std::max(settings.occlusionRadius, 0.01f) * kGtaoRadiusMultiplier,
                      std::clamp(settings.occlusionStrength, 0.0f, 1.0f)),
            glm::vec4(noiseFrame, kGtaoMaxRadius * float(rh), kGtaoBlurTolerance, 0.0f),
            glm::vec4(rayOrigin, 0.0f),
            glm::vec4((rayAt(float(rw), 0.0f) - rayOrigin) / float(rw), 0.0f),
            glm::vec4((rayAt(0.0f, float(rh)) - rayOrigin) / float(rh), 0.0f),
            glm::vec4(kSsrSteps, kSsrFirstStep, kSsrThickness, std::clamp(settings.reflectionRoughness, 0.05f, 1.0f)),
            glm::vec4(noiseFrame, kSsrResolveTolerance, kSsrGrowth, 0.0f),
            relativeViewProj,
            screenReflect ? screenSpace.historyViewProj(cameraPos) : relativeViewProj};
        const gpu::Texture& screenDepth = tracedPrepass ? rayTracing.depth() : screenSpace.depth();
        if (screenAo) {
            commands.beginTiming("gtao");
            screenSpace.occlusion(commands, screenDepth, tracedPrepass ? rayTracing.mask() : screenSpace.mask(),
                                  tracedPrepass && rayShadows, su);
            commands.endTiming();
        }
        if (screenReflect) {
            commands.beginTiming("ssr");
            screenSpace.reflect(commands, screenDepth,
                                tracedPrepass ? rayTracing.gbufferNormal() : screenSpace.gbufferNormal(), su);
            commands.endTiming();
        }
    }

    // ---- 1. Sun shadow cascades (standard depth), then the transmittance of their translucent casters
    bool transmittanceCycled = false;
    commands.beginTiming("shadows");
    for (int c = 0; c < shadow.count; ++c) {
        const gpu::DepthTarget dt{.texture = shadowMap,
                                  .layer = std::uint32_t(c),
                                  .store = gpu::StoreOp::Store,
                                  .clearDepth = 1.0f,
                                  .cycle = c == 0};
        gpu::RenderPass pass = commands.beginRenderPass(std::span<const gpu::ColorTarget>{}, &dt);
        if (!pass) continue;
        pass.bindStorageBuffers(kVertex, 0, {&instances, 1});
        pass.bindSampler(kFragment, 0, noShadowBinding);
        drawBatches(pass, shadowBatches[std::size_t(c)], c, stats.shadowDraws);
        PassContext ctx{commands,
                        &pass,
                        fu,
                        {},
                        shadowMap,
                        TextureFormat::Invalid,
                        shadowFormat,
                        settings.shadowMapSize,
                        settings.shadowMapSize,
                        c};
        runPasses(PassStage::Shadow, ctx, fu);
        pass.end();

        if ((transmittanceMask & (1u << c)) == 0) continue;
        const gpu::ColorTarget color{.texture = transmittanceMap,
                                     .layer = std::uint32_t(c),
                                     .clear = {1.0f, 1.0f, 1.0f, 0.0f},
                                     .cycle = !transmittanceCycled};
        transmittanceCycled = true;
        gpu::RenderPass tpass = commands.beginRenderPass(color);
        if (!tpass) continue;
        tpass.bindStorageBuffers(kVertex, 0, {&instances, 1});
        tpass.bindSampler(kFragment, 0, noShadowBinding);
        drawBatches(tpass, transmittanceBatches[std::size_t(c)], c, stats.transmittanceDraws);
        tpass.end();
    }
    commands.endTiming();
    // ---- 1b. The local lights' shadow maps: each drawn as cascade 0 of a frame with the map's projection.
    if (!localMaps.empty()) {
        commands.beginTiming("local_shadows");
        shader::FrameUniforms mapFrame = fu;
        for (std::size_t f = 0; f < localMaps.size(); ++f) {
            const gpu::DepthTarget dt{.texture = localShadowMap,
                                      .layer = std::uint32_t(f),
                                      .store = gpu::StoreOp::Store,
                                      .clearDepth = 1.0f,
                                      .cycle = f == 0};
            gpu::RenderPass pass = commands.beginRenderPass(std::span<const gpu::ColorTarget>{}, &dt);
            if (!pass) continue;
            mapFrame.shadowViewProj[0] = localMaps[f].viewProj;
            pass.pushUniforms(kVertex, 0, mapFrame);
            pass.pushUniforms(kFragment, 0, mapFrame);
            pass.bindStorageBuffers(kVertex, 0, {&instances, 1});
            pass.bindSampler(kFragment, 0, noShadowBinding);
            drawBatches(pass, localShadowBatches[f], 0, stats.localShadowDraws, false, true);
            pass.end();
        }
        commands.pushUniforms(kVertex, 0, fu);
        commands.pushUniforms(kFragment, 0, fu);
        commands.endTiming();
    }
    if (screenShadows) {
        commands.beginTiming("local_filter");
        screenSpace.resolveLocalShadows(commands, screenSpace.depth(), localShadowMap, shadowSampler, lu);
        commands.pushUniforms(kFragment, 1, lu);
        commands.endTiming();
    }

    // ---- 2. HDR pass: opaque surfaces, game passes, sky, transparent surfaces
    bool hasLaterPasses = false;
    for (const PassId id : passOrder)
        if (const PassEntry* e = passes.get(id);
            e && (e->stage == PassStage::PostHdr || e->stage == PassStage::Overlay))
            hasLaterPasses = true;
    const bool keepDepth = taa || hasLaterPasses;
    const glm::vec3 clear = settings.sky ? glm::vec3(0.0f) : settings.clearColor;
    if (pathFrame) {
        // ---- 2. The path tracer's mean in place of the HDR pass (sky, lights and materials as the hits see them)
        StateHash h;
        h.add(fu.viewProj);
        h.add(fu.screen);
        h.add(fu.skyZenith);
        h.add(fu.skyHorizon);
        h.add(fu.skyGround);
        h.add(fu.sunGlow);
        h.add(fu.sunDir);
        h.add(fu.sunColor);
        h.add(fu.sunDisc);
        h.add(lu);
        h.add(scene.serial);
        h.add(std::clamp(settings.pathBounces, 1, 16));
        for (const RayCaster& c : rayCasters) {
            const MaterialDesc& m = c.material->desc;
            h.add(c.slot);
            h.add(c.instance->desc.transform);
            h.add(c.instance->desc.color);
            h.add(c.instance->desc.mesh);
            for (const glm::mat4& joint : c.instance->palette) h.add(joint);
            h.add(m.baseColor);
            h.add(m.emissive);
            h.add(m.roughness);
            h.add(m.metallic);
            h.add(m.alphaCutoff);
            h.add(m.textures);
            h.add(m.anisotropy);
            h.add(m.anisotropyRotation);
            h.add(m.doubleSided);
        }
        const gpu::Texture& radiance = pathTracer.render(commands, rayTracing, hu, lu, h.value, settings.pathBounces,
                                                         settings.pathSamples, pointClamp);
        commands.blit(
            {.source = {.texture = radiance}, .destination = {.texture = hdr}, .filter = gpu::Filter::Nearest});
        stats.pathSamples = pathTracer.samples();
    }
    commands.beginTiming("hdr");
    if (!pathFrame) {
        const gpu::ColorTarget color{.texture = hdr, .clear = {clear.r, clear.g, clear.b, 1.0f}, .cycle = true};
        const gpu::DepthTarget dt{.texture = depth,
                                  .store = keepDepth ? gpu::StoreOp::Store : gpu::StoreOp::DontCare,
                                  .clearDepth = 0.0f,
                                  .cycle = true};
        gpu::RenderPass pass = commands.beginRenderPass(color, &dt);
        if (pass) {
            const gpu::TextureSamplerBinding shadowBinding{
                shadow.count > 0 ? gpu::TextureRef(shadowMap) : gpu::TextureRef(dummyShadow), shadowSampler};
            const gpu::TextureSamplerBinding localMapBinding{
                !localMaps.empty() ? gpu::TextureRef(localShadowMap) : gpu::TextureRef(dummyShadow), shadowSampler};
            const gpu::TextureSamplerBinding transmittanceBinding{
                transmittanceMask != 0 ? gpu::TextureRef(transmittanceMap) : gpu::TextureRef(dummyTransmittance),
                linearClamp};
            // The traced targets are the opaque surfaces' (the prepass depth): transparent ones read white for the
            // sun's mask and no local shadows, reflections or traced indirect light; the probe grid lights both.
            const auto traced = [&](std::uint32_t bit, const gpu::Texture& texture) {
                return gpu::TextureSamplerBinding{
                    (rayBits & bit) != 0 ? gpu::TextureRef(texture) : gpu::TextureRef(white), pointClamp};
            };
            const gpu::TextureSamplerBinding opaqueTraced[] = {
                traced(3u, tracedPrepass ? rayTracing.mask() : screenSpace.mask()),
                (rayBits & 64u) != 0 ? traced(64u, screenSpace.reflections()) : traced(8u, rayTracing.reflections()),
                traced(16u, rayTracing.indirect()),
                traced(4u, screenShadows ? screenSpace.localShadows() : rayTracing.localShadows())};
            const gpu::TextureSamplerBinding noTraced[] = {
                {white, pointClamp}, {white, pointClamp}, {white, pointClamp}, {white, pointClamp}};
            const bool grid = (rayBits & 32u) != 0;
            const gpu::TextureSamplerBinding probeBindings[] = {
                {grid ? gpu::TextureRef(rayTracing.probeIrradiance()) : gpu::TextureRef(white), linearClamp},
                {grid ? gpu::TextureRef(rayTracing.probeDistance()) : gpu::TextureRef(white), linearClamp}};
            shader::FrameUniforms transparentFrame = fu;
            transparentFrame.options.w = float(rayBits & (1u | 2u | 32u));
            const auto bindFrame = [&](bool opaqueSurfaces) {
                if (!opaqueSurfaces && rayBits != 0) pass.pushUniforms(kFragment, 0, transparentFrame);
                pass.pushUniforms(kFragment, 1, lu);
                pass.bindSampler(kFragment, 0, shadowBinding);
                pass.bindSampler(kFragment, 5, transmittanceBinding);
                pass.bindSamplers(kFragment, 6, opaqueSurfaces ? opaqueTraced : noTraced);
                pass.bindSamplers(kFragment, 10, probeBindings);
                pass.bindSampler(kFragment, 12, {areaTable, pointClamp});
                pass.bindSampler(kFragment, 13, localMapBinding);
                pass.bindSampler(kFragment, 14, clouds.shadowBinding(white, linearClamp));
                pass.bindSampler(kFragment, 15, clouds.cubeBinding(noSkyCube, linearClamp));
                pass.bindStorageBuffers(kVertex, 0, {&instances, 1});
            };
            bindFrame(true);
            drawBatches(pass, opaqueBatches, -1, stats.draws);
            PassContext ctx{commands, &pass, fu, hdr, depth, hdrFormat, depthFormat, rw, rh, -1};
            runPasses(PassStage::Opaque, ctx, fu);
            if (settings.sky && clouds.drawsSky()) {
                clouds.drawSky(pass);
                bindFrame(true); // the samplers and uniforms the sky with the clouds took
            } else if (settings.sky) {
                pass.bindPipeline(skyPipeline);
                pass.draw(3);
            }
            if (!transparentBatches.empty()) {
                bindFrame(false);
                drawBatches(pass, transparentBatches, -1, stats.draws);
            }
            runPasses(PassStage::Transparent, ctx, fu);
            pass.end();
        }
    }
    commands.endTiming();
    // The image the next frame's screen-space reflections take their hits' color from.
    if (screenHistory) {
        commands.beginTiming("ssr history");
        screenSpace.keepHistory(commands, hdr, relativeViewProj, cameraPos);
        commands.endTiming();
    } else {
        screenSpace.dropHistory();
    }

    // ---- 3. Motion vectors and TAA
    gpu::TextureRef image = hdr;
    if (taa) {
        commands.beginTiming("taa");
        if (!velocityDrawn) drawVelocity(depth);
        const bool reset = cut || !historyValid;
        const int write = historyIndex ^ 1;
        const gpu::ColorTarget color{
            .texture = history[std::size_t(write)], .load = gpu::LoadOp::DontCare, .cycle = true};
        gpu::RenderPass pass = taaResolve ? commands.beginRenderPass(color) : gpu::RenderPass{};
        if (pass) {
            const shader::PostUniforms pu{glm::vec4(1.0f / float(rw), 1.0f / float(rh), float(rw), float(rh)),
                                          glm::vec4(reset ? 1.0f : 0.0f, kTaaCurrentWeight, 0.0f, 0.0f),
                                          glm::vec4(0.0f), glm::vec4(0.0f)};
            const gpu::TextureSamplerBinding bindings[] = {{hdr, linearClamp},
                                                           {history[std::size_t(historyIndex)], linearClamp},
                                                           {velocity, pointClamp},
                                                           {depth, pointClamp}};
            pass.bindPipeline(taaPipeline);
            pass.bindSamplers(kFragment, 0, bindings);
            pass.pushUniforms(kFragment, 1, pu);
            pass.draw(3);
            pass.end();
        }
        commands.endTiming();
        if (taaResolve) {
            historyIndex = write;
            historyValid = true;
            image = history[std::size_t(write)];
        }
    } else {
        historyValid = false;
    }
    if (upscaling) {
        gpu::UpscaleDesc u{.color = image, .output = upscaled};
        if (effects.upscaling == Upscaling::Temporal || effects.upscaling == Upscaling::TemporalDenoised) {
            const glm::vec2 j = subpixelJitter(sequenceIndex);
            u.depth = depth;
            u.motion = velocity;
            // The image's shift by the jitter in input pixels, y down (the projection offset moves it by -j.x, +j.y;
            // the other signs measured worse against the native frame); the motion texture is current minus
            // previous uv.
            u.jitterX = -j.x;
            u.jitterY = j.y;
            u.motionScaleX = -float(rw);
            u.motionScaleY = -float(rh);
            u.reset = cut || upscalerReset;
        }
        if (effects.upscaling == Upscaling::TemporalDenoised) {
            u.diffuseAlbedo = rayTracing.diffuseAlbedo();
            u.specularAlbedo = rayTracing.specularAlbedo();
            u.normal = rayTracing.gbufferNormal();
            u.roughness = rayTracing.roughness();
            std::memcpy(u.worldToView.data(), &view.view[0][0], sizeof(u.worldToView));
            std::memcpy(u.viewToClip.data(), &proj[0][0], sizeof(u.viewToClip)); // without the jitter
        }
        commands.beginTiming("upscale");
        if (effects.upscaling != Upscaling::TemporalDenoised || denoiseInputs) {
            commands.upscale(upscaler, u);
            upscalerReset = false;
        } else {
            // No G-buffer this frame (the ray tracing targets failed): scaled up as by the bilinear path.
            commands.blit({.source = {.texture = image}, .destination = {.texture = upscaled}});
            upscalerReset = true;
        }
        commands.endTiming();
        image = upscaled;
    }

    // ---- 4. Game passes on the HDR image
    {
        PassContext ctx{commands,
                        nullptr,
                        fu,
                        image,
                        upscaling || pathFrame ? gpu::TextureRef{} : gpu::TextureRef(depth),
                        hdrFormat,
                        upscaling || pathFrame ? TextureFormat::Invalid : depthFormat,
                        iw,
                        ih,
                        -1};
        runPasses(PassStage::PostHdr, ctx, fu);
    }

    // ---- 5. Bloom
    const bool bloomOn = effects.bloom && bloomLevels > 0;
    if (bloomOn) {
        commands.beginTiming("bloom");
        for (int i = 0; i < bloomLevels; ++i) {
            const gpu::TextureRef src = i == 0 ? image : gpu::TextureRef(bloom[std::size_t(i - 1)]);
            const gpu::ColorTarget color{
                .texture = bloom[std::size_t(i)], .load = gpu::LoadOp::DontCare, .cycle = true};
            gpu::RenderPass pass = commands.beginRenderPass(color);
            if (!pass) continue;
            const shader::PostUniforms pu{
                glm::vec4(1.0f / float(src.desc.width), 1.0f / float(src.desc.height), 0.0f, 0.0f),
                glm::vec4(settings.bloomThreshold, 0.0f, 0.0f, i == 0 ? 1.0f : 0.0f), glm::vec4(0.0f), glm::vec4(0.0f)};
            pass.bindPipeline(bloomDownPipeline);
            pass.bindSampler(kFragment, 0, {src, linearClamp});
            pass.pushUniforms(kFragment, 1, pu);
            pass.draw(3);
            pass.end();
        }
        for (int i = bloomLevels - 1; i >= 1; --i) {
            const gpu::Texture& src = bloom[std::size_t(i)];
            const gpu::ColorTarget color{.texture = bloom[std::size_t(i - 1)], .load = gpu::LoadOp::Load};
            gpu::RenderPass pass = commands.beginRenderPass(color);
            if (!pass) continue;
            const shader::PostUniforms pu{
                glm::vec4(1.0f / float(src.desc().width), 1.0f / float(src.desc().height), 0.0f, 0.0f), glm::vec4(0.0f),
                glm::vec4(0.0f), glm::vec4(0.0f)};
            pass.bindPipeline(bloomUpPipeline);
            pass.bindSampler(kFragment, 0, {src, linearClamp});
            pass.pushUniforms(kFragment, 1, pu);
            pass.draw(3);
            pass.end();
        }
        commands.endTiming();
    }

    // ---- 6. Tone mapping (and FXAA) into the target, then the overlay passes
    const TextureFormat outFormat = target.desc.format;
    const TextureFormat toneFormat = fxaa ? (hdrOut ? TextureFormat::Rgba16Float : ldrFormat) : outFormat;
    const gpu::GraphicsPipeline* tonemap = outputPipeline(tonemapPipelines, "tonemap_fs", toneFormat);
    const gpu::GraphicsPipeline* fxaaPipe = fxaa ? outputPipeline(fxaaPipelines, "fxaa_fs", outFormat) : nullptr;
    if (tonemap && (!fxaa || fxaaPipe)) {
        commands.beginTiming("tonemap");
        // Target encoding (quenchToTarget in frame.hlsl): 0 display values, 1 linear for sRGB formats, 2 HDR linear.
        const float targetMode = hdrOut ? 2.0f : isSrgb(outFormat) ? 1.0f : 0.0f;
        const HdrKnee knee = hdrKnee(settings.toneCurve);
        const shader::PostUniforms tu{
            glm::vec4(1.0f / float(iw), 1.0f / float(ih), 0.0f, 0.0f),
            glm::vec4(taaResolve && !upscaling ? std::clamp(settings.sharpen, 0.0f, 1.0f) : 0.0f,
                      bloomOn ? settings.bloomStrength : 0.0f, env.exposure,
                      settings.toneCurve == ToneCurve::Agx ? 1.0f : 0.0f),
            glm::vec4(settings.vignette, fxaa ? 1.0f : 0.0f, targetMode, settings.dither && !hdrOut ? 1.0f : 0.0f),
            hdrOut
                ? glm::vec4(std::max(output.headroom, 1.0f), std::max(output.paperWhite, 1e-3f), knee.scene, knee.slope)
                : glm::vec4(0.0f)};
        const gpu::TextureSamplerBinding bindings[] = {
            {image, linearClamp}, {bloomOn ? gpu::TextureRef(bloom[0]) : gpu::TextureRef(white), linearClamp}};
        const auto overlay = [&](gpu::RenderPass& pass) {
            PassContext ctx{commands, &pass, fu, target, depth, outFormat, TextureFormat::Invalid, ow, oh, -1, output};
            runPasses(PassStage::Overlay, ctx, fu);
        };
        if (fxaa) {
            {
                const gpu::ColorTarget color{.texture = ldr, .load = gpu::LoadOp::DontCare, .cycle = true};
                gpu::RenderPass pass = commands.beginRenderPass(color);
                if (pass) {
                    pass.bindPipeline(*tonemap);
                    pass.bindSamplers(kFragment, 0, bindings);
                    pass.pushUniforms(kFragment, 1, tu);
                    pass.draw(3);
                    pass.end();
                }
            }
            const gpu::ColorTarget color{.texture = target, .load = gpu::LoadOp::DontCare};
            gpu::RenderPass pass = commands.beginRenderPass(color);
            if (pass) {
                const shader::PostUniforms pu{glm::vec4(1.0f / float(ow), 1.0f / float(oh), 0.0f, 0.0f),
                                              glm::vec4(0.0f), glm::vec4(0.0f, 0.0f, targetMode, 0.0f),
                                              glm::vec4(0.0f)};
                pass.bindPipeline(*fxaaPipe);
                pass.bindSampler(kFragment, 0, {ldr, linearClamp});
                pass.pushUniforms(kFragment, 1, pu);
                pass.draw(3);
                overlay(pass);
                pass.end();
            }
        } else {
            const gpu::ColorTarget color{.texture = target, .load = gpu::LoadOp::DontCare};
            gpu::RenderPass pass = commands.beginRenderPass(color);
            if (pass) {
                pass.bindPipeline(*tonemap);
                pass.bindSamplers(kFragment, 0, bindings);
                pass.pushUniforms(kFragment, 1, tu);
                pass.draw(3);
                overlay(pass);
                pass.end();
            }
        }
        commands.endTiming();
    }
    commands.popDebugGroup();

    // ---- Temporal state for the next frame
    prevViewProj = viewProjStable;
    prevCameraPos = cameraPos;
    prevCameraFwd = cameraFwd;
    prevValid = true;
    ++frameIndex;
    ++sequenceIndex;
    for (std::uint32_t i = 0; i < slots; ++i) {
        InstanceEntry* inst = scene.instances.slot(i);
        if (!inst) continue;
        if (inst->moved) {
            inst->previous = inst->desc.transform;
            inst->moved = false;
        }
        if (skinnedOffsets[i] != kNotSkinned) {
            inst->skinnedOffset = skinnedOffsets[i];
            inst->skinnedFrame = frameIndex - 1;
        }
        inst->posed = false;
    }
    stats.pipelines = pipelineCount;
}

} // namespace detail

Result<Renderer> Renderer::create(gpu::Device& device, const RenderSettings& settings) {
    Renderer renderer;
    renderer.m_impl = std::make_unique<detail::RendererImpl>();
    renderer.m_impl->device = &device;
    renderer.m_impl->settings = settings;
    if (Result<void> r = renderer.m_impl->init(); !r) return Error{r.error()};
    return renderer;
}

Renderer::Renderer(Renderer&& other) noexcept = default;
Renderer& Renderer::operator=(Renderer&& other) noexcept = default;
Renderer::~Renderer() = default;

void Renderer::setSettings(const RenderSettings& settings) {
    m_impl->settings = settings;
    m_impl->applySettings();
}

const RenderSettings& Renderer::settings() const { return m_impl->settings; }
const ActiveEffects& Renderer::effects() const { return m_impl->effects; }
std::span<const std::string> Renderer::fallbacks() const { return m_impl->fallbacks; }

Result<SurfaceId> Renderer::addSurface(const SurfaceDesc& desc) {
    const std::string name(desc.name.empty() ? std::string_view("surface") : desc.name);
    if (!desc.vertexShader || !desc.fragmentShader || desc.vertexShader->stage != ShaderStage::Vertex ||
        desc.fragmentShader->stage != ShaderStage::Fragment ||
        (desc.basicFragmentShader && desc.basicFragmentShader->stage != ShaderStage::Fragment) ||
        (desc.localShadowFragmentShader && desc.localShadowFragmentShader->stage != ShaderStage::Fragment) ||
        (desc.shadowVertexShader && desc.shadowVertexShader->stage != ShaderStage::Vertex) ||
        (desc.shadowFragmentShader && desc.shadowFragmentShader->stage != ShaderStage::Fragment) ||
        (desc.transmittanceShader && desc.transmittanceShader->stage != ShaderStage::Fragment) ||
        (desc.gbufferShader && desc.gbufferShader->stage != ShaderStage::Fragment))
        return Error{std::format("surface '{}': needs a vertex and a fragment shader", name)};
    if (desc.rayHit > 0 && !desc.rayHitShaders)
        return Error{std::format("surface '{}': rayHit {} without rayHitShaders", name, desc.rayHit)};
    if (desc.rayHitShaders && m_impl->rayHitShaders && desc.rayHitShaders != m_impl->rayHitShaders)
        return Error{std::format("surface '{}': another quench_add_ray_hits() set than the renderer's surfaces have "
                                 "(a renderer takes one)",
                                 name)};
    detail::SurfaceEntry entry;
    entry.vertex = desc.vertexShader;
    entry.fragment = desc.fragmentShader;
    entry.basicFragment = desc.basicFragmentShader;
    entry.localShadowFragment = desc.localShadowFragmentShader;
    entry.shadowVertex = desc.shadowVertexShader;
    entry.shadowFragment = desc.shadowFragmentShader;
    entry.transmittanceFragment = desc.transmittanceShader;
    entry.gbufferFragment = desc.gbufferShader;
    entry.rayHit = desc.rayHit;
    entry.name = name;
    if (!m_impl->surfacePipeline(entry, detail::variantOf(BlendMode::Opaque, false, detail::Shading::Full)) ||
        (entry.basicFragment &&
         !m_impl->surfacePipeline(entry, detail::variantOf(BlendMode::Opaque, false, detail::Shading::Basic))))
        return Error{std::format("surface '{}': the pipeline could not be created (logged)", name)};
    if (desc.rayHitShaders) {
        m_impl->rayHitShaders = desc.rayHitShaders;
        m_impl->rayTracing.setSurfaceHits(desc.rayHitShaders);
    }
    return m_impl->surfaces.add(std::move(entry));
}

PassId Renderer::addPass(PassStage stage, std::string name, PassFn fn) {
    const PassId id = m_impl->passes.add({stage, std::move(name), std::move(fn)});
    m_impl->passOrder.push_back(id);
    return id;
}

void Renderer::removePass(PassId pass) {
    const bool removed = m_impl->passes.remove(pass);
    QUENCH_ASSERT(removed, "removePass: stale or null handle");
    (void)removed;
    std::erase(m_impl->passOrder, pass);
}

void Renderer::render(Scene& scene, const View& view, gpu::CommandBuffer& commands, const gpu::TextureRef& target,
                      const gpu::DisplayOutput& output) {
    QUENCH_PROFILE_ZONE("render.frame");
    m_impl->render(*scene.m_impl, view, commands, target, output);
}

const RenderStats& Renderer::stats() const { return m_impl->stats; }
gpu::TextureFormat Renderer::hdrFormat() const { return m_impl->hdrFormat; }
gpu::TextureFormat Renderer::depthFormat() const { return m_impl->depthFormat; }
gpu::TextureFormat Renderer::shadowFormat() const { return m_impl->shadowFormat; }

} // namespace quench::render
