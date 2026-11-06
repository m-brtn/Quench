#include "RayTracing.h"

#include <quench/core/Log.h>

#include <render_frame_shaders.h>
#include <render_raytrace_shaders.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>

namespace quench::render::detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureUsage;

constexpr ShaderStage kFragment = ShaderStage::Fragment;
constexpr TextureFormat kMaskFormat = TextureFormat::Rgba8Unorm;
constexpr std::uint8_t kMaskOpaque = 1;      // raytrace.hlsl: opaque and cut-out instances
constexpr std::uint8_t kMaskTranslucent = 2; // raytrace.hlsl: translucent instances (not in ambient occlusion)
// A skinned bottom level is refitted to each pose and built again after this many refits: refits keep the build's
// tree, which loosens as limbs move away from the pose it was built in.
constexpr std::uint32_t kRefitsPerBuild = 60;

std::array<float, 12> rowsOf(const glm::mat4& m) {
    std::array<float, 12> t{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) t[std::size_t(r * 4 + c)] = m[c][r];
    return t;
}

std::uint32_t grown(std::uint32_t capacity, std::size_t needed, std::uint32_t minimum) {
    std::uint32_t c = std::max(capacity, minimum);
    while (c < needed) c *= 2;
    return c;
}

} // namespace

bool RayTracing::init(gpu::Device& device, std::uint32_t& pipelines) {
    m_device = &device;
    m_available = false;
    if (!device.caps().rayTracing) return false;
    const ShaderBlob* trace = shaders::render_raytrace.find("rt_shadow_fs");
    if (!trace) {
        log::warning("render", "ray tracing: this build has no ray query shaders (QUENCH_RAY_QUERY_SHADERS, DXC); "
                               "shadow maps");
        return false;
    }
    const ShaderBlob* fullscreen = shaders::render_frame.find("fullscreen_vs");
    const gpu::ColorTargetDesc color{kMaskFormat};
    Result<gpu::GraphicsPipeline> tracePipeline = device.createPipeline(
        {.vertexShader = fullscreen, .fragmentShader = trace, .colorTargets = {&color, 1}, .name = "render ray trace"});
    Result<gpu::GraphicsPipeline> denoisePipeline =
        device.createPipeline({.vertexShader = fullscreen,
                               .fragmentShader = shaders::render_raytrace.find("rt_denoise_fs"),
                               .colorTargets = {&color, 1},
                               .name = "render ray denoise"});
    if (!tracePipeline || !denoisePipeline) {
        log::error("render", "ray tracing pipelines: {}; shadow maps",
                   tracePipeline ? denoisePipeline.error() : tracePipeline.error());
        return false;
    }
    m_tracePipeline = std::move(*tracePipeline);
    m_denoisePipeline = std::move(*denoisePipeline);
    pipelines += 2;
    // Bound when no caster needs them.
    if (!ensureBuffer(m_vertices, m_vertexBytes, 256, "render ray vertices") ||
        !ensureBuffer(m_indices, m_indexBytes, 256, "render ray indices") ||
        !ensureBuffer(m_instanceTable, m_instanceBytes, 256, "render ray instances"))
        return false;
    m_available = true;
    return true;
}

bool RayTracing::ensureBuffer(gpu::Buffer& buffer, std::uint32_t& capacity, std::uint32_t bytes, const char* name) {
    if (buffer && capacity >= bytes) return true;
    const std::uint32_t size = grown(capacity, bytes, 256);
    Result<gpu::Buffer> b =
        m_device->createBuffer({.usage = gpu::BufferUsage::GraphicsStorageRead, .size = size, .name = name});
    if (!b) {
        log::error("render", "{} ({} bytes): {}", name, size, b.error());
        return false;
    }
    buffer = std::move(*b);
    capacity = size;
    return true;
}

bool RayTracing::ensureTargets(std::uint32_t width, std::uint32_t height, TextureFormat depthFormat) {
    if (m_mask && m_width == width && m_height == height) return true;
    releaseTargets();
    const auto create = [this, width, height](gpu::Texture& out, TextureFormat format, TextureUsage usage,
                                              const char* name) {
        Result<gpu::Texture> t =
            m_device->createTexture({.format = format, .usage = usage, .width = width, .height = height, .name = name});
        if (!t) {
            log::error("render", "target '{}' {} x {}: {}", name, width, height, t.error());
            return false;
        }
        out = std::move(*t);
        return true;
    };
    const TextureUsage targetSampled = TextureUsage::ColorTarget | TextureUsage::Sampled;
    if (!create(m_depth, depthFormat, TextureUsage::DepthStencilTarget | TextureUsage::Sampled, "render ray depth") ||
        !create(m_raw, kMaskFormat, targetSampled, "render ray trace") ||
        !create(m_mask, kMaskFormat, targetSampled, "render ray mask")) {
        releaseTargets();
        return false;
    }
    m_width = width;
    m_height = height;
    return true;
}

void RayTracing::releaseTargets() {
    m_depth = {};
    m_raw = {};
    m_mask = {};
    m_width = m_height = 0;
}

// The vertices and indices of the meshes of cut-out and translucent casters, copied once per set of meshes.
bool RayTracing::updateTables(gpu::CommandBuffer& commands, std::uint64_t scene, std::span<const RayCaster> casters) {
    std::vector<TableMesh> meshes;
    for (const RayCaster& c : casters) {
        const bool cutOut = c.material->desc.alphaCutoff > 0.0f;
        if (!c.translucent && !cutOut) continue;
        meshes.push_back({c.instance->desc.mesh, c.mesh});
    }
    std::sort(meshes.begin(), meshes.end(), [](const TableMesh& a, const TableMesh& b) {
        return a.id.index != b.id.index ? a.id.index < b.id.index : a.id.generation < b.id.generation;
    });
    meshes.erase(
        std::unique(meshes.begin(), meshes.end(), [](const TableMesh& a, const TableMesh& b) { return a.id == b.id; }),
        meshes.end());
    std::uint32_t vertices = 0, indices = 0;
    for (TableMesh& m : meshes) {
        m.firstVertex = vertices;
        m.firstIndex = indices;
        vertices += m.mesh->vertexCount;
        indices += m.mesh->indexCount;
    }
    const bool same = scene == m_tableScene && meshes.size() == m_tableMeshes.size() &&
                      std::equal(meshes.begin(), meshes.end(), m_tableMeshes.begin(),
                                 [](const TableMesh& a, const TableMesh& b) { return a.id == b.id; });
    if (same) return true;
    m_tableScene = scene;
    const auto vertexBytes = std::uint32_t(vertices * sizeof(Vertex));
    const auto indexBytes = std::uint32_t(indices * sizeof(std::uint32_t));
    if (!ensureBuffer(m_vertices, m_vertexBytes, vertexBytes, "render ray vertices") ||
        !ensureBuffer(m_indices, m_indexBytes, indexBytes, "render ray indices")) {
        m_tableMeshes.clear();
        return false;
    }
    if (!meshes.empty()) {
        gpu::CopyPass copy = commands.beginCopyPass();
        for (const TableMesh& m : meshes) {
            copy.copy({m.mesh->vertices}, 0, {m_vertices}, m.firstVertex * std::uint32_t(sizeof(Vertex)),
                      m.mesh->vertexCount * std::uint32_t(sizeof(Vertex)));
            copy.copy({m.mesh->indices}, 0, {m_indices}, m.firstIndex * std::uint32_t(sizeof(std::uint32_t)),
                      m.mesh->indexCount * std::uint32_t(sizeof(std::uint32_t)));
        }
        copy.end();
    }
    m_tableMeshes = std::move(meshes);
    return true;
}

bool RayTracing::build(gpu::CommandBuffer& commands, const SceneImpl& scene, std::span<const RayCaster> casters,
                       RayTracingStats& stats) {
    m_instances.clear();
    m_data.clear();
    m_alphaTextures.clear();
    if (!m_available || casters.empty()) return false;
    if (!updateTables(commands, scene.serial, casters)) return false;

    gpu::AccelerationStructurePass pass = commands.beginAccelerationStructurePass();
    if (!pass) return false;
    const auto create = [this](const gpu::AccelerationStructureDesc& desc) -> gpu::AccelerationStructure {
        Result<gpu::AccelerationStructure> s = m_device->createAccelerationStructure(desc);
        if (!s) {
            log::error("render", "acceleration structure '{}': {}", desc.name, s.error());
            return {};
        }
        return std::move(*s);
    };
    for (const RayCaster& c : casters) {
        MeshEntry& mesh = *c.mesh;
        const std::uint32_t triangles = mesh.indexCount / 3;
        if (triangles == 0) continue;
        gpu::RayGeometry geometry{.vertices = {mesh.vertices},
                                  .vertexStride = std::uint32_t(sizeof(Vertex)),
                                  .vertexCount = mesh.vertexCount,
                                  .indices = {mesh.indices},
                                  .triangleCount = triangles};
        const gpu::AccelerationStructure* bottom = nullptr;
        if (c.skinned.buffer) {
            InstanceEntry& inst = *c.instance;
            geometry.vertices = c.skinned.buffer;
            geometry.vertexOffset = c.skinned.offset;
            const bool fresh = !inst.rayStructure || inst.rayStructure.desc().vertexCount != mesh.vertexCount ||
                               inst.rayStructure.desc().triangleCount != triangles;
            if (fresh)
                inst.rayStructure = create({.vertexCount = mesh.vertexCount,
                                            .triangleCount = triangles,
                                            .allowRefit = true,
                                            .name = "render ray skinned"});
            if (!inst.rayStructure) continue;
            if (fresh || inst.rayRefits >= kRefitsPerBuild) {
                pass.build(inst.rayStructure, geometry);
                // A crowd made in one frame would otherwise build again all in one frame.
                inst.rayRefits = fresh ? m_skinnedBuilds++ % kRefitsPerBuild : 0;
                ++stats.builds;
            } else {
                pass.refit(inst.rayStructure, geometry);
                ++inst.rayRefits;
                ++stats.refits;
            }
            bottom = &inst.rayStructure;
        } else {
            if (!mesh.rayStructure) {
                mesh.rayStructure =
                    create({.vertexCount = mesh.vertexCount, .triangleCount = triangles, .name = "render ray mesh"});
                if (!mesh.rayStructure) continue;
                pass.build(mesh.rayStructure, geometry);
                ++stats.builds;
            }
            bottom = &mesh.rayStructure;
        }

        const MaterialDesc& m = c.material->desc;
        const bool cutOut = m.alphaCutoff > 0.0f;
        gpu::RayInstance instance{.bottomLevel = bottom,
                                  .transform = rowsOf(c.instance->desc.transform),
                                  .mask = c.translucent ? kMaskTranslucent : kMaskOpaque,
                                  .flags = c.translucent || cutOut ? gpu::RayInstanceFlags::NonOpaque
                                                                   : gpu::RayInstanceFlags::Opaque};
        if (c.translucent || cutOut) {
            const auto table = std::lower_bound(
                m_tableMeshes.begin(), m_tableMeshes.end(), c.instance->desc.mesh, [](const TableMesh& t, MeshId id) {
                    return t.id.index != id.index ? t.id.index < id.index : t.id.generation < id.generation;
                });
            int alphaSlot = -1;
            const TextureEntry* texture = scene.textures.get(m.textures[0]);
            if (texture && texture->texture) {
                const auto it = std::find(m_alphaTextures.begin(), m_alphaTextures.end(), &texture->texture);
                if (it != m_alphaTextures.end()) {
                    alphaSlot = int(it - m_alphaTextures.begin());
                } else if (m_alphaTextures.size() < QUENCH_RT_ALPHA_TEXTURES) {
                    alphaSlot = int(m_alphaTextures.size());
                    m_alphaTextures.push_back(&texture->texture);
                } else if (!m_alphaTexturesLogged) {
                    log::warning("render",
                                 "ray tracing: more than {} textures on cut-out and translucent casters; "
                                 "the others cast as if untextured",
                                 QUENCH_RT_ALPHA_TEXTURES);
                    m_alphaTexturesLogged = true;
                }
            }
            instance.id = std::uint32_t(m_data.size());
            m_data.push_back(
                {glm::vec4(std::bit_cast<float>(table->firstIndex), std::bit_cast<float>(table->firstVertex),
                           c.translucent ? 2.0f : 1.0f, float(alphaSlot)),
                 m.baseColor * c.instance->desc.color, glm::vec4(m.alphaCutoff, 0.0f, 0.0f, 0.0f)});
        }
        m_instances.push_back(instance);
    }
    if (m_instances.empty()) return false;
    const auto needed = std::uint32_t(m_instances.size());
    if (!m_top || m_top.desc().instanceCapacity < needed) {
        m_top = create({.level = gpu::AccelerationStructureLevel::Top,
                        .instanceCapacity = grown(m_top ? m_top.desc().instanceCapacity : 0, needed, 64),
                        .name = "render ray scene"});
        if (!m_top) return false;
    }
    pass.build(m_top, m_instances);
    pass.end();
    stats.instances = needed;

    if (!m_data.empty()) {
        const auto bytes = std::uint32_t(m_data.size() * sizeof(shader::RayInstanceData));
        if (!ensureBuffer(m_instanceTable, m_instanceBytes, bytes, "render ray instances")) return false;
        if (!m_transfer || m_transferBytes < bytes) {
            const std::uint32_t size = grown(m_transferBytes, bytes, 1024);
            Result<gpu::TransferBuffer> t = m_device->createTransferBuffer({.size = size, .name = "render ray"});
            if (!t) {
                log::error("render", "ray tracing transfer buffer: {}", t.error());
                return false;
            }
            m_transfer = std::move(*t);
            m_transferBytes = size;
        }
        std::span<std::byte> mapped = m_transfer.map(true);
        if (mapped.size() < bytes) return false;
        std::memcpy(mapped.data(), m_data.data(), bytes);
        m_transfer.unmap();
        gpu::CopyPass copy = commands.beginCopyPass();
        copy.upload(m_transfer, 0, m_instanceTable, 0, bytes, true);
    }
    return true;
}

void RayTracing::trace(gpu::CommandBuffer& commands, const shader::RayTracingUniforms& uniforms,
                       const gpu::Sampler& point, const gpu::Sampler& material, const gpu::Texture& white) {
    // Nothing to blur (hard shadows, no occlusion): the rays write the mask directly.
    const bool denoise = uniforms.denoise.x > 0.0f || uniforms.denoise.z > 0.0f;
    commands.beginTiming("rays");
    const gpu::ColorTarget traced{.texture = denoise ? m_raw : m_mask, .load = gpu::LoadOp::DontCare, .cycle = true};
    if (gpu::RenderPass pass = commands.beginRenderPass(traced)) {
        std::array<gpu::TextureSamplerBinding, 1 + QUENCH_RT_ALPHA_TEXTURES> samplers{};
        samplers[0] = {m_depth, point};
        for (std::size_t i = 0; i < QUENCH_RT_ALPHA_TEXTURES; ++i)
            samplers[i + 1] = {
                i < m_alphaTextures.size() ? gpu::TextureRef(*m_alphaTextures[i]) : gpu::TextureRef(white), material};
        const std::array<gpu::BufferRef, 3> buffers{m_instanceTable, m_vertices, m_indices};
        pass.bindPipeline(m_tracePipeline);
        pass.bindSamplers(kFragment, 0, samplers);
        pass.bindStorageBuffers(kFragment, 0, buffers);
        pass.bindAccelerationStructure(kFragment, 0, m_top);
        pass.pushUniforms(kFragment, 1, uniforms);
        pass.draw(3);
        pass.end();
    }
    commands.endTiming();
    if (!denoise) return;
    commands.beginTiming("denoise");
    const gpu::ColorTarget color{.texture = m_mask, .load = gpu::LoadOp::DontCare, .cycle = true};
    if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
        const gpu::TextureSamplerBinding samplers[] = {{m_raw, point}, {m_depth, point}};
        pass.bindPipeline(m_denoisePipeline);
        pass.bindSamplers(kFragment, 0, samplers);
        pass.pushUniforms(kFragment, 1, uniforms);
        pass.draw(3);
        pass.end();
    }
    commands.endTiming();
}

} // namespace quench::render::detail
