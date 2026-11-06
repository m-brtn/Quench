#include <quench/render/Renderer.h>

#include "Effects.h"
#include "RayTracing.h"
#include "SceneImpl.h"
#include "shaders/PostUniforms.h"

#include <quench/core/Log.h>
#include <quench/render/Cascades.h>
#include <quench/render/Projection.h>

#include <render_frame_shaders.h>
#include <render_raytrace_shaders.h>
#include <render_skin_shaders.h>
#include <render_standard_shaders.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
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
constexpr std::uint32_t kVariants = 6;      // 3 blend modes x single / double sided
// The transmittance maps of translucent casters have 1 / kTransmittanceScale of the shadow map size per side: their
// shadows are soft, and a full-size Rgba16Float array would take twice the depth array's memory.
constexpr std::uint32_t kTransmittanceScale = 2;
constexpr float kTransmittanceBias = 2.0f; // in transmittance texels: receivers nearer behind a caster stay lit
// Ray-traced shadows: how far rays toward the sun look (m), the ray origin's offset along the normal per meter of
// view distance (the depth's precision and the normal reconstructed from it), and the denoise pass.
constexpr float kRayShadowLength = 1000.0f;
constexpr float kRayNormalOffset = 0.002f;
// Soft sun shadows (a cone) are noisy in their penumbrae only; ambient occlusion is noisy everywhere.
constexpr int kRayDenoiseShadowRadius = 1;    // pixels: a 3 x 3 box (0 for hard shadows)
constexpr int kRayDenoiseOcclusionRadius = 2; // pixels: a 5 x 5 box
constexpr float kRayDenoiseTolerance = 0.02f; // relative view depth: neighbours further apart are other surfaces

constexpr gpu::VertexBufferLayout kVertexLayout{.slot = 0, .stride = sizeof(Vertex)};
constexpr gpu::VertexAttribute kVertexAttributes[] = {
    {.location = 0, .slot = 0, .format = gpu::VertexFormat::Float3, .offset = offsetof(Vertex, position)},
    {.location = 1, .slot = 0, .format = gpu::VertexFormat::Float3, .offset = offsetof(Vertex, normal)},
    {.location = 2, .slot = 0, .format = gpu::VertexFormat::Float2, .offset = offsetof(Vertex, uv)},
    {.location = 3, .slot = 0, .format = gpu::VertexFormat::Float4, .offset = offsetof(Vertex, color)},
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

std::uint32_t variantOf(BlendMode blend, bool doubleSided) { return std::uint32_t(blend) * 2 + (doubleSided ? 1 : 0); }

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
    const ShaderBlob* shadowVertex = nullptr;
    const ShaderBlob* shadowFragment = nullptr;        // null: the standard surface's
    const ShaderBlob* transmittanceFragment = nullptr; // null: the standard surface's
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

struct SkinJob {
    const MeshEntry* mesh = nullptr;
    std::uint32_t firstJoint = 0;
    std::uint32_t offset = 0; // first vertex in the skinned vertex buffer
};

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

} // namespace

struct RendererImpl {
    gpu::Device* device = nullptr;
    RenderSettings settings;
    ActiveEffects effects;
    bool effectsChosen = false;
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
        taaPipeline, bloomDownPipeline, bloomUpPipeline;
    std::array<gpu::GraphicsPipeline, 2> depthPipelines; // the ray tracing prepass of plain surfaces: [doubleSided]
    std::array<bool, 2> depthFailed{};
    gpu::ComputePipeline skinPipeline; // empty: skinned instances are drawn in their bind pose (logged at init)
    std::map<TextureFormat, gpu::GraphicsPipeline> tonemapPipelines, fxaaPipelines;

    gpu::Sampler shadowSampler, linearClamp, pointClamp, materialSampler;
    gpu::Texture white, dummyShadow, dummyTransmittance;

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
    std::vector<Batch> opaqueBatches, transparentBatches, velocityBatches;
    std::array<std::vector<Batch>, kMaxCascades> shadowBatches, transmittanceBatches;

    // Skinning: every frame writes the skinned vertices of the instances it draws into skinned[frameIndex & 1], so the
    // other buffer still holds the previous frame's for the motion vectors.
    std::array<gpu::Buffer, 2> skinned;
    std::uint32_t skinnedCapacity = 0; // vertices
    bool skinnedReset = false;         // recreated this frame: no previous vertices
    gpu::Buffer paletteBuffer;
    gpu::TransferBuffer paletteTransfer;
    std::uint32_t paletteCapacity = 0;  // joints
    std::vector<glm::vec4> paletteData; // three rows per joint
    std::vector<SkinJob> skinJobs;
    std::vector<std::uint32_t> skinnedOffsets; // per instance slot; kNotSkinned
    std::uint32_t skinnedVertices = 0;
    std::vector<gpu::Fence> skinFences; // oldest first
    float skinningGpuMs = -1.0f;

    // Ray tracing (SunShadows::RayTraced, AmbientOcclusion::RayTraced)
    RayTracing rayTracing;
    bool rayTracingTried = false;
    std::vector<RayCaster> rayCasters;

    // Temporal state
    std::uint64_t frameIndex = 0;
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
    const gpu::GraphicsPipeline* outputPipeline(std::map<TextureFormat, gpu::GraphicsPipeline>& cache,
                                                const char* entry, TextureFormat format);
    bool ensureTargets(std::uint32_t rw, std::uint32_t rh, std::uint32_t ow, std::uint32_t oh, TextureFormat ldrTarget);
    bool ensureTransmittance();
    bool ensureBuffers(gpu::CommandBuffer& commands);
    std::uint32_t skinnedOffset(const InstanceEntry& inst, std::uint32_t slot, const MeshEntry& mesh);
    bool skin(gpu::CommandBuffer& commands);
    gpu::BufferBinding skinnedVerticesOf(std::uint32_t slot) const;
    void runPasses(PassStage stage, PassContext& context, const shader::FrameUniforms& frame);
    void buildBatches(const SceneImpl& scene, std::vector<Item>& items, std::vector<Batch>& batches);
    void bindMaterial(gpu::RenderPass& pass, const SceneImpl& scene, const MaterialEntry& material, bool shadowPass);
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
    support.spatialUpscaling = caps.spatialUpscaling;
    support.temporalUpscaling = caps.temporalUpscaling;
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

    if (Result<gpu::ComputePipeline> p =
            device->createComputePipeline({.shader = shaders::render_skin.find("skin_cs"), .name = "render skin"});
        p)
        skinPipeline = std::move(*p);
    else
        log::error("render", "skinning pipeline: {}; skinned meshes are drawn in their bind pose", p.error());

    standard.vertex = shaders::render_standard.find("surface_vs");
    standard.fragment = shaders::render_standard.find("surface_fs");
    standard.shadowFragment = shaders::render_standard.find("surface_shadow_fs");
    standard.transmittanceFragment = shaders::render_standard.find("surface_transmittance_fs");
    standard.name = "standard";
    if (!surfacePipeline(standard, variantOf(BlendMode::Opaque, false)))
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
              support.temporalUpscaling  ? "spatial, temporal"
              : support.spatialUpscaling ? "spatial"
                                         : "bilinear");
    return {};
}

void RendererImpl::applySettings() {
    const ActiveEffects next = selectEffects(settings, support);
    if (next == effects && effectsChosen) return;
    const char* noRays = device->caps().rayTracing ? "this build has no ray query shaders (cmake/QuenchDxc.cmake)"
                                                   : "the device has no ray tracing (gpu::Caps::rayTracing)";
    if (settings.sunShadows == SunShadows::RayTraced && settings.shadowCascades > 0 &&
        next.sunShadows != SunShadows::RayTraced)
        log::warning("render", "ray-traced shadows: {}; shadow maps", noRays);
    if (settings.ambientOcclusion == AmbientOcclusion::RayTraced && next.ambientOcclusion != settings.ambientOcclusion)
        log::warning("render", "ray-traced ambient occlusion: {}; none", noRays);
    if (settings.upscaling == Upscaling::Spatial && next.upscaling != Upscaling::Spatial && settings.renderScale < 1.0f)
        log::warning("render", "spatial upscaling: not on this device (gpu::Caps::spatialUpscaling); bilinear");
    if (settings.upscaling == Upscaling::Temporal && next.upscaling != Upscaling::Temporal)
        log::warning("render", "temporal upscaling: not on this device (gpu::Caps::temporalUpscaling, TAA's inputs); "
                               "bilinear");
    if (settings.antiAliasing != next.antiAliasing && next.upscaling != Upscaling::Temporal)
        log::warning("render", "anti-aliasing: TAA needs a sampled depth buffer and Rg16Float targets; using FXAA");
    if (settings.shadowCascades > 0 && next.shadowCascades == 0 && next.sunShadows == SunShadows::ShadowMaps)
        log::warning("render", "shadows off: no depth texture array the device can sample and render to");
    if (next.shadowCascades > 0 && settings.translucentShadows != next.translucentShadows)
        log::warning("render", "translucent shadows: no {} texture array to render to; dithered into the depth map",
                     gpu::formatInfo(transmittanceFormat).name);
    effects = next;
    effectsChosen = true;
}

const gpu::GraphicsPipeline* RendererImpl::surfacePipeline(SurfaceEntry& surface, std::uint32_t variant) {
    if (surface.pipelines[variant]) return &surface.pipelines[variant];
    if (surface.failed[variant]) return nullptr;
    const auto blend = BlendMode(variant / 2);
    const bool doubleSided = variant % 2 != 0;
    gpu::ColorTargetDesc color{hdrFormat};
    if (blend == BlendMode::AlphaBlend) color.blend = gpu::BlendState::alpha();
    if (blend == BlendMode::Additive) color.blend = gpu::BlendState::additive();
    const std::string name = std::format("surface {} ({})", surface.name, variant);
    Result<gpu::GraphicsPipeline> p = createPipeline({
        .vertexShader = surface.vertex,
        .fragmentShader = surface.fragment,
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
    const bool effectsSame = targetEffects.antiAliasing == effects.antiAliasing &&
                             targetEffects.shadowCascades == effects.shadowCascades &&
                             targetEffects.bloom == effects.bloom && targetEffects.upscaling == effects.upscaling &&
                             targetShadowSize == settings.shadowMapSize && targetLdrFormat == ldrTarget;
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
    if (effects.antiAliasing == AntiAliasing::Taa) {
        if (!create(velocity, {.format = velocityFormat,
                               .usage = targetSampled,
                               .width = rw,
                               .height = rh,
                               .name = "render velocity"}))
            return false;
        if (effects.upscaling != Upscaling::Temporal)
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
        const bool temporal = effects.upscaling == Upscaling::Temporal;
        Result<gpu::Upscaler> u =
            device->createUpscaler({.type = temporal ? gpu::UpscalerType::Temporal : gpu::UpscalerType::Spatial,
                                    .colorFormat = hdrFormat,
                                    .outputFormat = hdrFormat,
                                    .depthFormat = depthFormat,
                                    .motionFormat = velocityFormat,
                                    .inputWidth = rw,
                                    .inputHeight = rh,
                                    .outputWidth = ow,
                                    .outputHeight = oh,
                                    .name = "render upscaler"});
        if (!u) {
            // Not for this size or format: the frame is scaled up in tone mapping from now on.
            log::error("render", "{} upscaler {} x {} -> {} x {}: {}; bilinear", temporal ? "temporal" : "spatial", rw,
                       rh, ow, oh, u.error());
            (temporal ? support.temporalUpscaling : support.spatialUpscaling) = false;
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
// meshes and without the skinning pipeline.
std::uint32_t RendererImpl::skinnedOffset(const InstanceEntry& inst, std::uint32_t slot, const MeshEntry& mesh) {
    if (mesh.jointCount == 0 || !skinPipeline) return kNotSkinned;
    std::uint32_t& offset = skinnedOffsets[slot];
    if (offset != kNotSkinned) return offset;
    offset = skinnedVertices;
    skinnedVertices += mesh.vertexCount;
    skinJobs.push_back({&mesh, std::uint32_t(paletteData.size() / 3), offset});
    const auto row = [](const glm::mat4& m, int r) { return glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]); };
    if (inst.palette.size() == mesh.jointCount) {
        for (const glm::mat4& m : inst.palette)
            paletteData.insert(paletteData.end(), {row(m, 0), row(m, 1), row(m, 2)});
    } else {
        const glm::mat4 identity(1.0f);
        for (std::uint32_t j = 0; j < mesh.jointCount; ++j)
            paletteData.insert(paletteData.end(), {row(identity, 0), row(identity, 1), row(identity, 2)});
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
    if (skinJobs.empty()) return true;
    const auto grow = [](std::uint32_t capacity, std::size_t needed) {
        std::uint32_t c = std::max(capacity, 1024u);
        while (c < needed) c *= 2;
        return c;
    };
    const std::uint32_t vertices = grow(skinnedCapacity, skinnedVertices);
    if (vertices != skinnedCapacity || !skinned[0]) {
        for (int i = 0; i < 2; ++i) {
            Result<gpu::Buffer> b =
                device->createBuffer({.usage = gpu::BufferUsage::Vertex | gpu::BufferUsage::ComputeStorageWrite,
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
    const auto joints = std::uint32_t(paletteData.size() / 3);
    if (const std::uint32_t capacity = grow(paletteCapacity, joints); capacity != paletteCapacity || !paletteBuffer) {
        Result<gpu::Buffer> b = device->createBuffer({.usage = gpu::BufferUsage::ComputeStorageRead,
                                                      .size = capacity * 3 * std::uint32_t(sizeof(glm::vec4)),
                                                      .name = "render joint palettes"});
        Result<gpu::TransferBuffer> t = device->createTransferBuffer(
            {.size = capacity * 3 * std::uint32_t(sizeof(glm::vec4)), .name = "render joint palettes"});
        if (!b || !t) {
            log::error("render", "palette buffer ({} joints): {}", capacity, b ? t.error() : b.error());
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
    const auto bytes = std::uint32_t(paletteData.size() * sizeof(glm::vec4));
    std::span<std::byte> mapped = paletteTransfer.map(true);
    if (mapped.size() < bytes) return false;
    std::memcpy(mapped.data(), paletteData.data(), bytes);
    paletteTransfer.unmap();
    {
        gpu::CopyPass copy = target.beginCopyPass();
        copy.upload(paletteTransfer, 0, paletteBuffer, 0, bytes, true);
    }
    {
        const gpu::StorageBufferWrite write{.buffer = skinned[frameIndex & 1], .cycle = true};
        gpu::ComputePass pass = target.beginComputePass({&write, 1});
        if (!pass) return false;
        pass.bindPipeline(skinPipeline);
        const MeshEntry* bound = nullptr;
        for (const SkinJob& job : skinJobs) {
            if (job.mesh != bound) {
                const std::array<gpu::BufferRef, 3> inputs{job.mesh->vertices, job.mesh->skin, paletteBuffer};
                pass.bindStorageBuffers(0, inputs);
                bound = job.mesh;
            }
            pass.pushUniforms(0, shader::SkinUniforms{glm::vec4(float(job.mesh->vertexCount), float(job.firstJoint),
                                                                float(job.offset), 0.0f)});
            pass.dispatch((job.mesh->vertexCount + kSkinGroup - 1) / kSkinGroup);
        }
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
                                bool shadowPass) {
    const MaterialDesc& m = material.desc;
    shader::MaterialUniforms u{};
    u.baseColor = m.baseColor;
    const bool dithered = shadowPass && shadowModeOf(m) == ShadowMode::Translucent &&
                          effects.translucentShadows == TranslucentShadows::Dithered;
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
    const bool taaResolve = taa && effects.upscaling != Upscaling::Temporal;
    const bool fxaa = effects.antiAliasing == AntiAliasing::Fxaa;
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
    glm::mat4 projJ = proj;
    if (taa) {
        const glm::vec2 j = subpixelJitter(frameIndex);
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
    fu.options = glm::vec4(env.exposure, float(frameIndex % 1024), taa ? 1.0f : 0.0f, 0.0f);

    const std::array<Plane, 5> planes = frustumPlanes(fu.viewProj);
    const LightEntry* sun = nullptr;
    struct Candidate {
        float score;
        shader::LocalLight light;
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
                {-1.0f, {glm::vec4(0.0f), glm::vec4(color, 0.0f), glm::vec4(dir, 0.0f), glm::vec4(0.0f)}});
            continue;
        }
        if (l.range <= 0.0f || !sphereVisible(planes, l.position, l.range)) continue;
        shader::LocalLight local{};
        local.positionRange = glm::vec4(l.position, l.range);
        local.color = glm::vec4(color, l.type == LightType::Spot ? 2.0f : 1.0f);
        const float cosOuter = std::cos(l.outerAngle), cosInner = std::cos(std::min(l.innerAngle, l.outerAngle));
        local.direction = glm::vec4(dir, cosOuter);
        local.cone = glm::vec4(1.0f / std::max(cosInner - cosOuter, 1e-4f), 0.0f, 0.0f, 0.0f);
        candidates.push_back({std::max(0.0f, glm::length(l.position - cameraPos) - l.range), local});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    shader::LightUniforms lu{};
    const auto lightCount = std::min<std::size_t>(candidates.size(), QUENCH_MAX_LIGHTS);
    for (std::size_t i = 0; i < lightCount; ++i) lu.lights[i] = candidates[i].light;
    lu.count = glm::vec4(float(lightCount), 0.0f, 0.0f, 0.0f);
    stats.lights = std::uint32_t(lightCount);

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
    const int cascades = sunShadows ? effects.shadowCascades : 0;
    const bool rayShadows = sunShadows && effects.sunShadows == SunShadows::RayTraced;
    const bool rayOcclusion = effects.ambientOcclusion == AmbientOcclusion::RayTraced;
    bool rayTraced = rayShadows || rayOcclusion;
    if (rayTraced && !rayTracing.available()) {
        if (!rayTracingTried) rayTracing.init(*device, pipelineCount);
        rayTracingTried = true;
        if (!rayTracing.available()) {
            support.rayTracing = false;
            applySettings();
            rayTraced = false;
        }
    }
    if (!rayTraced) rayTracing.releaseTargets();
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
        const bool needNormal = inView || (cascades > 0 && inst->desc.castsShadows);
        if (needNormal) normalMatrices[i] = glm::transpose(glm::inverse(model));
        if (inView) {
            const std::uint32_t variant = variantOf(m.blend, m.doubleSided);
            const gpu::GraphicsPipeline* pipeline = surfacePipeline(*surface, variant);
            if (pipeline) {
                const bool skinnedHere = skinnedOffset(*inst, i, *mesh) != kNotSkinned;
                Item item{0, glm::length(center - cameraPos), i, pipeline};
                if (rayTraced && m.blend == BlendMode::Opaque) {
                    const bool cutOut = m.alphaCutoff > 0.0f;
                    item.depth = surfaceDepthPipeline(*surface, m.doubleSided, cutOut);
                    item.depthMaterial = cutOut;
                }
                if (m.blend == BlendMode::Opaque) {
                    item.key = (std::uint64_t(surfaceIndex & 0xfff) << 52) | (std::uint64_t(variant) << 48) |
                               (std::uint64_t(inst->desc.material.index & 0xffffff) << 24) |
                               std::uint64_t(inst->desc.mesh.index & 0xffffff);
                    opaque.push_back(item);
                } else {
                    transparent.push_back(item);
                }
                if (taa && (inst->moved || (skinnedHere && inst->posed))) moved.push_back(item);
            }
        } else {
            ++stats.culled;
        }
        const ShadowMode shadowMode = shadowModeOf(m);
        if (rayTraced && inst->desc.castsShadows && shadowMode != ShadowMode::None) {
            // Every caster is in the top level; skinned ones only near the camera (they are posed for it).
            const bool skinnedMesh = mesh->jointCount > 0 && skinPipeline;
            if (!skinnedMesh || glm::length(center - cameraPos) - radius < settings.shadowDistance) {
                if (skinnedMesh) skinnedOffset(*inst, i, *mesh);
                rayCasters.push_back({i, inst, mesh, material, {}, shadowMode == ShadowMode::Translucent});
            }
        }
        if (cascades == 0 || !inst->desc.castsShadows || shadowMode == ShadowMode::None) continue;
        // Plain casters write depth with the position only; cut-out ones (and translucent ones dithered into the
        // depth) run the surface to discard; translucent ones go into the transmittance maps.
        const std::uint64_t materialKey = std::uint64_t(inst->desc.material.index & 0xffffff) << 24;
        const std::uint64_t meshKey = std::uint64_t(inst->desc.mesh.index & 0xffffff);
        const bool translucent = shadowMode == ShadowMode::Translucent;
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
    velocityData.clear();
    velocityBatches.clear();
    std::sort(moved.begin(), moved.end(), [&scene](const Item& a, const Item& b) {
        return scene.instances.slot(a.slot)->desc.mesh.index < scene.instances.slot(b.slot)->desc.mesh.index;
    });
    for (const Item& item : moved) {
        const InstanceEntry& inst = *scene.instances.slot(item.slot);
        const MeshEntry* mesh = scene.meshes.get(inst.desc.mesh);
        const gpu::BufferBinding current = skinnedVerticesOf(item.slot);
        if (current.buffer) {
            const bool hasPrevious = !skinnedReset && inst.skinnedFrame != ~0ull && inst.skinnedFrame + 1 == frameIndex;
            const gpu::BufferBinding previous =
                hasPrevious ? gpu::BufferBinding{skinned[(frameIndex & 1) ^ 1],
                                                 inst.skinnedOffset * std::uint32_t(sizeof(Vertex))}
                            : current;
            velocityBatches.push_back(
                {&velocitySkinPipeline, mesh, nullptr, std::uint32_t(velocityData.size()), 1, current, previous});
        } else {
            if (velocityBatches.empty() || velocityBatches.back().mesh != mesh ||
                velocityBatches.back().vertices.buffer)
                velocityBatches.push_back({&velocityObjPipeline, mesh, nullptr, std::uint32_t(velocityData.size()), 0});
            velocityBatches.back().count++;
        }
        velocityData.push_back({inst.desc.transform, inst.previous});
    }
    stats.instances = std::uint32_t(opaque.size() + transparent.size());
    stats.movedInstances = std::uint32_t(moved.size());
    // Timed ranges (gpu::Caps::gpuPassTiming): upload, accel, prepass, raytrace with rays and denoise (ray tracing),
    // shadows, hdr, taa, upscale, bloom, tonemap.
    commands.beginTiming("upload");
    const bool buffers = ensureBuffers(commands);
    commands.endTiming();
    if (!buffers) {
        commands.popDebugGroup();
        return;
    }

    // ---- 0. Ray tracing: the acceleration structures of the casters (skinned ones after the skinning pass)
    bool rayMask = false;
    if (rayTraced) {
        RayTracingStats rs;
        commands.beginTiming("accel");
        rayMask = rayTracing.ensureTargets(rw, rh, depthFormat) && rayTracing.build(commands, scene, rayCasters, rs);
        commands.endTiming();
        stats.rayInstances = rs.instances;
        stats.rayBuilds = rs.builds;
        stats.rayRefits = rs.refits;
    }
    fu.options.w = rayMask ? float((rayShadows ? 1 : 0) | (rayOcclusion ? 2 : 0)) : 0.0f;
    commands.pushUniforms(kVertex, 0, fu);
    commands.pushUniforms(kFragment, 0, fu);
    commands.pushUniforms(kFragment, 1, lu);

    // cascade: the shadow map layer the batches are drawn into; -1: the camera's passes. prepass: the batches'
    // depth pipelines (the ray tracing prepass).
    const auto drawBatches = [&](gpu::RenderPass& pass, const std::vector<Batch>& batches, int cascade,
                                 std::uint32_t& counter, bool prepass = false) {
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
                bindMaterial(pass, scene, *batchMaterial, shadowPass);
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

    // ---- 0. Ray tracing: the camera's depth of the opaque surfaces, traced toward the sun and over the hemisphere
    if (rayMask) {
        commands.beginTiming("prepass");
        {
            const gpu::DepthTarget dt{
                .texture = rayTracing.depth(), .store = gpu::StoreOp::Store, .clearDepth = 0.0f, .cycle = true};
            gpu::RenderPass pass = commands.beginRenderPass(std::span<const gpu::ColorTarget>{}, &dt);
            if (pass) {
                pass.bindStorageBuffers(kVertex, 0, {&instances, 1});
                pass.bindSampler(kFragment, 0, noShadowBinding);
                std::uint32_t draws = 0;
                drawBatches(pass, opaqueBatches, -1, draws, true);
                pass.end();
            }
        }
        commands.endTiming();
        const float cone = std::clamp(env.sunAngularRadius * std::max(settings.shadowSoftness, 0.0f), 0.0f, 0.2f);
        const shader::RayTracingUniforms ru{
            glm::vec4(rayShadows ? float(std::clamp(settings.shadowRays, 1, 8)) : 0.0f, std::tan(cone),
                      kRayShadowLength, kRayNormalOffset),
            glm::vec4(rayOcclusion ? float(std::clamp(settings.occlusionRays, 1, 8)) : 0.0f,
                      std::max(settings.occlusionRadius, 0.01f), std::clamp(settings.occlusionStrength, 0.0f, 1.0f),
                      0.0f),
            glm::vec4(cone > 0.0f && settings.shadowRays > 0 ? float(kRayDenoiseShadowRadius) : 0.0f,
                      kRayDenoiseTolerance, rayOcclusion ? float(kRayDenoiseOcclusionRadius) : 0.0f, 0.0f)};
        commands.beginTiming("raytrace");
        rayTracing.trace(commands, ru, pointClamp, materialSampler, white);
        commands.endTiming();
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

    // ---- 2. HDR pass: opaque surfaces, game passes, sky, transparent surfaces
    bool hasLaterPasses = false;
    for (const PassId id : passOrder)
        if (const PassEntry* e = passes.get(id);
            e && (e->stage == PassStage::PostHdr || e->stage == PassStage::Overlay))
            hasLaterPasses = true;
    const bool keepDepth = taa || hasLaterPasses;
    const glm::vec3 clear = settings.sky ? glm::vec3(0.0f) : settings.clearColor;
    commands.beginTiming("hdr");
    {
        const gpu::ColorTarget color{.texture = hdr, .clear = {clear.r, clear.g, clear.b, 1.0f}, .cycle = true};
        const gpu::DepthTarget dt{.texture = depth,
                                  .store = keepDepth ? gpu::StoreOp::Store : gpu::StoreOp::DontCare,
                                  .clearDepth = 0.0f,
                                  .cycle = true};
        gpu::RenderPass pass = commands.beginRenderPass(color, &dt);
        if (pass) {
            const gpu::TextureSamplerBinding shadowBinding{
                shadow.count > 0 ? gpu::TextureRef(shadowMap) : gpu::TextureRef(dummyShadow), shadowSampler};
            const gpu::TextureSamplerBinding transmittanceBinding{
                transmittanceMask != 0 ? gpu::TextureRef(transmittanceMap) : gpu::TextureRef(dummyTransmittance),
                linearClamp};
            // The ray-traced mask is the opaque surfaces' (the prepass depth): transparent ones read white.
            const gpu::TextureSamplerBinding maskBinding{
                rayMask ? gpu::TextureRef(rayTracing.mask()) : gpu::TextureRef(white), pointClamp};
            const gpu::TextureSamplerBinding noMaskBinding{white, pointClamp};
            const auto bindFrame = [&](bool opaqueSurfaces) {
                pass.pushUniforms(kFragment, 1, lu);
                pass.bindSampler(kFragment, 0, shadowBinding);
                pass.bindSampler(kFragment, 5, transmittanceBinding);
                pass.bindSampler(kFragment, 6, opaqueSurfaces ? maskBinding : noMaskBinding);
                pass.bindStorageBuffers(kVertex, 0, {&instances, 1});
            };
            bindFrame(true);
            drawBatches(pass, opaqueBatches, -1, stats.draws);
            PassContext ctx{commands, &pass, fu, hdr, depth, hdrFormat, depthFormat, rw, rh, -1};
            runPasses(PassStage::Opaque, ctx, fu);
            if (settings.sky) {
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

    // ---- 3. Motion vectors and TAA
    gpu::TextureRef image = hdr;
    if (taa) {
        commands.beginTiming("taa");
        {
            const gpu::ColorTarget color{.texture = velocity, .cycle = true};
            gpu::RenderPass pass = commands.beginRenderPass(color);
            if (pass) {
                pass.bindPipeline(velocityPipeline);
                pass.bindSampler(kFragment, 0, {depth, pointClamp});
                pass.draw(3);
                pass.end();
            }
        }
        if (!velocityBatches.empty()) {
            const gpu::ColorTarget color{.texture = velocity, .load = gpu::LoadOp::Load};
            const gpu::DepthTarget dt{.texture = depth, .load = gpu::LoadOp::Load, .store = gpu::StoreOp::Store};
            gpu::RenderPass pass = commands.beginRenderPass(color, &dt);
            if (pass) {
                const gpu::BufferRef movedBuffer(velocityBuffer);
                pass.bindStorageBuffers(kVertex, 0, {&movedBuffer, 1});
                std::uint32_t draws = 0;
                drawBatches(pass, velocityBatches, -1, draws);
                pass.end();
            }
        }
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
        if (effects.upscaling == Upscaling::Temporal) {
            const glm::vec2 j = subpixelJitter(frameIndex);
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
        commands.beginTiming("upscale");
        commands.upscale(upscaler, u);
        commands.endTiming();
        upscalerReset = false;
        image = upscaled;
    }

    // ---- 4. Game passes on the HDR image
    {
        PassContext ctx{commands,
                        nullptr,
                        fu,
                        image,
                        upscaling ? gpu::TextureRef{} : gpu::TextureRef(depth),
                        hdrFormat,
                        upscaling ? TextureFormat::Invalid : depthFormat,
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

Result<SurfaceId> Renderer::addSurface(const SurfaceDesc& desc) {
    const std::string name(desc.name.empty() ? std::string_view("surface") : desc.name);
    if (!desc.vertexShader || !desc.fragmentShader || desc.vertexShader->stage != ShaderStage::Vertex ||
        desc.fragmentShader->stage != ShaderStage::Fragment ||
        (desc.shadowVertexShader && desc.shadowVertexShader->stage != ShaderStage::Vertex) ||
        (desc.shadowFragmentShader && desc.shadowFragmentShader->stage != ShaderStage::Fragment) ||
        (desc.transmittanceShader && desc.transmittanceShader->stage != ShaderStage::Fragment))
        return Error{std::format("surface '{}': needs a vertex and a fragment shader", name)};
    detail::SurfaceEntry entry;
    entry.vertex = desc.vertexShader;
    entry.fragment = desc.fragmentShader;
    entry.shadowVertex = desc.shadowVertexShader;
    entry.shadowFragment = desc.shadowFragmentShader;
    entry.transmittanceFragment = desc.transmittanceShader;
    entry.name = name;
    if (!m_impl->surfacePipeline(entry, detail::variantOf(BlendMode::Opaque, false)))
        return Error{std::format("surface '{}': the pipeline could not be created (logged)", name)};
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
    m_impl->render(*scene.m_impl, view, commands, target, output);
}

const RenderStats& Renderer::stats() const { return m_impl->stats; }
gpu::TextureFormat Renderer::hdrFormat() const { return m_impl->hdrFormat; }
gpu::TextureFormat Renderer::depthFormat() const { return m_impl->depthFormat; }
gpu::TextureFormat Renderer::shadowFormat() const { return m_impl->shadowFormat; }

} // namespace quench::render
