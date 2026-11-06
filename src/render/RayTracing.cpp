#include "RayTracing.h"

#include <quench/core/Log.h>

#include <render_frame_shaders.h>
#include <render_raydenoise_shaders.h>
#include <render_raylight_shaders.h>
#include <render_raytrace_shaders.h>

#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace quench::render::detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureUsage;

constexpr ShaderStage kFragment = ShaderStage::Fragment;
constexpr TextureFormat kMaskFormat = TextureFormat::Rgba8Unorm;
constexpr TextureFormat kLightFormat = TextureFormat::Rgba16Float;
constexpr TextureFormat kNormalFormat = TextureFormat::Rgba16Float;
constexpr TextureFormat kDistanceFormat = TextureFormat::Rg16Float;
// A skinned bottom level is refitted to each pose and built again after this many refits: refits keep the build's
// tree, which loosens as limbs move away from the pose it was built in. The top level likewise as instances move.
constexpr std::uint32_t kRefitsPerBuild = 60;
constexpr std::uint32_t kTopRefitsPerBuild = 30;
// The hit texture arrays: every material texture once, scaled to one size without mip levels.
constexpr std::uint32_t kHitTextureSize = 256;
constexpr std::uint32_t kMaxHitLayers = 64;

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

bool idLess(MeshId a, MeshId b) { return a.index != b.index ? a.index < b.index : a.generation < b.generation; }

std::uint64_t textureBytes(const gpu::Texture& t) {
    if (!t) return 0;
    const gpu::TextureDesc& d = t.desc();
    return gpu::levelByteSize(d.format, d.width, d.height, d.layers);
}

} // namespace

bool RayTracing::init(gpu::Device& device, std::uint32_t& pipelines, const RaySamplers& samplers) {
    m_device = &device;
    m_samplers = samplers;
    m_available = false;
    m_lighting = false;
    if (!device.caps().rayTracing) return false;
    const ShaderBlob* trace = shaders::render_raytrace.find("rt_shadow_fs");
    if (!trace) {
        log::warning("render", "ray tracing: this build has no ray query shaders (QUENCH_RAY_QUERY_SHADERS, DXC); "
                               "shadow maps");
        return false;
    }
    const ShaderBlob* fullscreen = shaders::render_frame.find("fullscreen_vs");
    const auto make = [&](gpu::GraphicsPipeline& out, const ShaderBlob* shader,
                          std::span<const gpu::ColorTargetDesc> targets, const char* name) {
        if (!shader) {
            log::error("render", "pipeline '{}': the shader is not in this build", name);
            return false;
        }
        Result<gpu::GraphicsPipeline> p = device.createPipeline(
            {.vertexShader = fullscreen, .fragmentShader = shader, .colorTargets = targets, .name = name});
        if (!p) {
            log::error("render", "pipeline '{}': {}", name, p.error());
            return false;
        }
        out = std::move(*p);
        ++pipelines;
        return true;
    };
    const gpu::ColorTargetDesc mask{kMaskFormat};
    const gpu::ColorTargetDesc maskAndLocal[] = {{kMaskFormat}, {kMaskFormat}};
    const gpu::ColorTargetDesc denoiseInputs[] = {{kAlbedoFormat}, {kAlbedoFormat}, {kRoughnessFormat}};
    if (!make(m_tracePipeline, trace, {&mask, 1}, "render ray trace") ||
        !make(m_denoiseInputsPipeline, shaders::render_raydenoise.find("rt_denoise_inputs_fs"), denoiseInputs,
              "render denoise inputs") ||
        !make(m_traceLocalPipeline, shaders::render_raytrace.find("rt_shadow_local_fs"), maskAndLocal,
              "render ray trace local") ||
        !make(m_denoisePipeline, shaders::render_raytrace.find("rt_denoise_fs"), {&mask, 1}, "render ray denoise") ||
        !m_maskFilter.init(device, kMaskFormat, "ray mask", pipelines) ||
        !m_localFilter.init(device, kMaskFormat, "ray local shadows", pipelines)) {
        log::error("render", "ray tracing pipelines failed (logged); shadow maps");
        return false;
    }
    // Bound when no instance needs them.
    if (!ensureBuffer(m_vertices, m_vertexBytes, 256, "render ray vertices") ||
        !ensureBuffer(m_indices, m_indexBytes, 256, "render ray indices") ||
        !ensureBuffer(m_instanceTable, m_instanceBytes, 256, "render ray instances"))
        return false;
    m_available = true;

    if (!shaders::render_raylight.find("rt_reflect_fs")) {
        log::warning("render", "ray tracing: this build has no ray query shaders for reflections, indirect light "
                               "and the path tracer; sky reflections and ambient light");
        return true;
    }
    const gpu::ColorTargetDesc light{kLightFormat}, distance{kDistanceFormat};
    m_lighting =
        make(m_reflectPipeline, shaders::render_raylight.find("rt_reflect_fs"), {&light, 1}, "render ray reflect") &&
        make(m_indirectPipeline, shaders::render_raylight.find("rt_indirect_fs"), {&light, 1}, "render ray indirect") &&
        make(m_probeTracePipeline, shaders::render_raylight.find("rt_probe_trace_fs"), {&light, 1},
             "render probe rays") &&
        make(m_probeIrradiancePipeline, shaders::render_raylight.find("rt_probe_irradiance_fs"), {&light, 1},
             "render probe irradiance") &&
        make(m_probeDistancePipeline, shaders::render_raylight.find("rt_probe_distance_fs"), {&distance, 1},
             "render probe distance") &&
        m_reflectionFilter.init(device, kLightFormat, "ray reflections", pipelines) &&
        m_indirectFilter.init(device, kLightFormat, "ray indirect", pipelines);
    if (!m_lighting)
        log::error("render", "ray-traced lighting pipelines failed (logged); sky reflections and ambient light");
    return true;
}

Result<gpu::GraphicsPipeline> RayTracing::hitPipeline(const char* entry, TextureFormat format, const char* name) {
    const ShaderBlob* shader = shaders::render_raylight.find(entry);
    if (!shader) return Error{std::format("the shader '{}' is not in this build", entry)};
    const gpu::ColorTargetDesc color{format};
    return m_device->createPipeline({.vertexShader = shaders::render_frame.find("fullscreen_vs"),
                                     .fragmentShader = shader,
                                     .colorTargets = {&color, 1},
                                     .name = name});
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

bool RayTracing::createTarget(gpu::Texture& out, TextureFormat format, std::uint32_t width, std::uint32_t height,
                              const char* name, TextureUsage usage) {
    Result<gpu::Texture> t =
        m_device->createTexture({.format = format, .usage = usage, .width = width, .height = height, .name = name});
    if (!t) {
        log::error("render", "target '{}' {} x {}: {}", name, width, height, t.error());
        return false;
    }
    out = std::move(*t);
    return true;
}

bool RayTracing::ensureTargets(std::uint32_t width, std::uint32_t height, TextureFormat depthFormat,
                               const RayTargets& targets) {
    if (m_mask && m_width == width && m_height == height && m_targets == targets) return true;
    releaseTargets();
    const TextureUsage depthUsage = TextureUsage::DepthStencilTarget | TextureUsage::Sampled;
    bool ok = createTarget(m_depth[0], depthFormat, width, height, "render ray depth 0", depthUsage) &&
              createTarget(m_depth[1], depthFormat, width, height, "render ray depth 1", depthUsage) &&
              createTarget(m_raw, kMaskFormat, width, height, "render ray trace") &&
              createTarget(m_mask, kMaskFormat, width, height, "render ray mask") &&
              m_maskFilter.ensureTargets(width, height);
    if (ok && targets.gbuffer)
        ok = createTarget(m_gbufferNormal, kNormalFormat, width, height, "render gbuffer normal") &&
             createTarget(m_gbufferAlbedo, TextureFormat::Rgba8Unorm, width, height, "render gbuffer albedo");
    if (ok && targets.gbuffer && targets.denoiseInputs)
        ok = createTarget(m_diffuseAlbedo, kAlbedoFormat, width, height, "render diffuse albedo") &&
             createTarget(m_specularAlbedo, kAlbedoFormat, width, height, "render specular albedo") &&
             createTarget(m_roughness, kRoughnessFormat, width, height, "render roughness");
    if (ok && targets.local)
        ok = createTarget(m_localRaw, kMaskFormat, width, height, "render ray local trace") &&
             createTarget(m_local, kMaskFormat, width, height, "render ray local shadows") &&
             m_localFilter.ensureTargets(width, height);
    if (ok && targets.reflections)
        ok = createTarget(m_reflectionsRaw, kLightFormat, width, height, "render ray reflections trace") &&
             createTarget(m_reflections, kLightFormat, width, height, "render ray reflections") &&
             m_reflectionFilter.ensureTargets(width, height);
    if (ok && targets.indirect)
        ok = createTarget(m_indirectRaw, kLightFormat, width, height, "render ray indirect trace") &&
             createTarget(m_indirect, kLightFormat, width, height, "render ray indirect") &&
             m_indirectFilter.ensureTargets(width, height);
    if (ok && targets.probes) {
        const auto probes = std::uint32_t(kProbeCounts.x * kProbeCounts.y * kProbeCounts.z);
        const auto columns = std::uint32_t(kProbeCounts.x * kProbeCounts.y), rows = std::uint32_t(kProbeCounts.z);
        ok = createTarget(m_probeRays, kLightFormat, kProbeRays, probes, "render probe rays");
        for (int i = 0; ok && i < 2; ++i)
            ok = createTarget(m_probeIrradiance[std::size_t(i)], kLightFormat, columns * kIrradianceTile,
                              rows * kIrradianceTile, "render probe irradiance") &&
                 createTarget(m_probeDistance[std::size_t(i)], kDistanceFormat, columns * kDistanceTile,
                              rows * kDistanceTile, "render probe distance");
    }
    if (!ok) {
        releaseTargets();
        return false;
    }
    m_width = width;
    m_height = height;
    m_targets = targets;
    m_recreated = true;
    return true;
}

void RayTracing::releaseTargets() {
    m_depth = {};
    m_gbufferNormal = {};
    m_gbufferAlbedo = {};
    m_diffuseAlbedo = {};
    m_specularAlbedo = {};
    m_roughness = {};
    m_raw = {};
    m_mask = {};
    m_localRaw = {};
    m_local = {};
    m_reflectionsRaw = {};
    m_reflections = {};
    m_indirectRaw = {};
    m_indirect = {};
    m_probeRays = {};
    m_probeIrradiance = {};
    m_probeDistance = {};
    m_probesValid = false;
    m_maskFilter.release();
    m_localFilter.release();
    m_reflectionFilter.release();
    m_indirectFilter.release();
    m_width = m_height = 0;
    m_targets = {};
}

// The vertices and indices of the meshes the shaders read: every instance's for the hit passes, else those of
// cut-out and translucent casters; copied once per set of meshes.
bool RayTracing::updateTables(gpu::CommandBuffer& commands, std::uint64_t scene, std::span<const RayCaster> casters,
                              bool all) {
    std::vector<TableMesh> meshes;
    for (const RayCaster& c : casters) {
        const bool cutOut = c.material->desc.alphaCutoff > 0.0f;
        if (!all && !c.translucent && !cutOut) continue;
        meshes.push_back({c.instance->desc.mesh, c.mesh});
    }
    std::sort(meshes.begin(), meshes.end(), [](const TableMesh& a, const TableMesh& b) { return idLess(a.id, b.id); });
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

void RayTracing::updateHitArray(gpu::CommandBuffer& commands, HitArray& array, std::uint64_t scene,
                                std::span<const std::pair<TextureId, const gpu::Texture*>> wanted,
                                std::vector<int>& layers) {
    layers.assign(wanted.size(), -1);
    for (HitLayer& l : array.layers) l.used = false;
    const auto find = [&](TextureId id, const gpu::Texture* t) {
        for (std::size_t i = 0; i < array.layers.size(); ++i) {
            const HitLayer& l = array.layers[i];
            if (l.scene == scene && l.id == id && l.native == gpu::TextureRef(*t).native) return int(i);
        }
        return -1;
    };
    std::vector<std::size_t> missing;
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        const int layer = find(wanted[i].first, wanted[i].second);
        if (layer >= 0) {
            array.layers[std::size_t(layer)].used = true;
            layers[i] = layer;
        } else {
            missing.push_back(i);
        }
    }
    if (missing.empty()) return;
    // Grow when the textures of this frame do not fit; the copies are made again into the new array.
    const std::size_t count = std::min<std::size_t>(wanted.size(), kMaxHitLayers);
    const std::uint32_t capacity = array.texture ? array.texture.desc().layers : 0;
    if (count > capacity) {
        const std::uint32_t layersNeeded = std::min(grown(capacity, count, 4), kMaxHitLayers);
        Result<gpu::Texture> t = m_device->createTexture({.type = gpu::TextureType::Texture2DArray,
                                                          .format = array.format,
                                                          .usage = TextureUsage::ColorTarget | TextureUsage::Sampled,
                                                          .width = kHitTextureSize,
                                                          .height = kHitTextureSize,
                                                          .layers = layersNeeded,
                                                          .name = array.name});
        if (!t) {
            log::error("render", "hit texture array ({} layers): {}; hits read their material's values only",
                       layersNeeded, t.error());
            return;
        }
        array.texture = std::move(*t);
        array.layers.assign(layersNeeded, {});
        std::fill(layers.begin(), layers.end(), -1);
        missing.clear();
        for (std::size_t i = 0; i < wanted.size(); ++i) missing.push_back(i);
    }
    for (const std::size_t i : missing) {
        // A layer not used this frame, empty ones first.
        int free = -1;
        for (std::size_t l = 0; l < array.layers.size(); ++l) {
            if (array.layers[l].used) continue;
            if (free < 0 || !array.layers[l].native) free = int(l);
            if (!array.layers[l].native) break;
        }
        if (free < 0) {
            if (!m_hitLayersLogged) {
                log::warning("render",
                             "ray tracing: more than {} material textures; hits on the others read their "
                             "material's values only",
                             kMaxHitLayers);
                m_hitLayersLogged = true;
            }
            continue;
        }
        const gpu::Texture& source = *wanted[i].second;
        const gpu::TextureDesc& d = source.desc();
        // The mip level nearest the array's size (at least its size where there is one).
        std::uint32_t level = 0;
        while (level + 1 < d.mipLevels && std::min(d.width >> (level + 1), d.height >> (level + 1)) >= kHitTextureSize)
            ++level;
        commands.blit({.source = {.texture = source, .mipLevel = level},
                       .destination = {.texture = array.texture,
                                       .layer = std::uint32_t(free),
                                       .width = kHitTextureSize,
                                       .height = kHitTextureSize},
                       .filter = gpu::Filter::Linear});
        array.layers[std::size_t(free)] = {scene, wanted[i].first, gpu::TextureRef(source).native, true};
        layers[i] = free;
    }
}

bool RayTracing::build(gpu::CommandBuffer& commands, const SceneImpl& scene, std::span<const RayCaster> casters,
                       bool hitData, const gpu::BufferRef& skinned, RayTracingStats& stats) {
    m_instances.clear();
    m_data.clear();
    m_alphaTextures.clear();
    m_skinned = skinned;
    if (!m_available || casters.empty()) return false;
    if (!updateTables(commands, scene.serial, casters, hitData)) return false;
    m_tableAll = hitData;

    // The hit texture arrays: base color and emissive (sRGB), metallic-roughness (linear).
    std::vector<int> colorLayers, linearLayers;
    std::vector<std::pair<TextureId, const gpu::Texture*>> colorWanted, linearWanted;
    const auto want = [&scene](std::vector<std::pair<TextureId, const gpu::Texture*>>& list, TextureId id) {
        const TextureEntry* t = scene.textures.get(id);
        if (!t || !t->texture) return; // unset or still loading
        for (const auto& w : list)
            if (w.first == id) return;
        list.emplace_back(id, &t->texture);
    };
    if (hitData) {
        if (m_hitColor.format == TextureFormat::Invalid) {
            m_hitColor.format = TextureFormat::Rgba8UnormSrgb;
            m_hitColor.name = "render hit colors";
            m_hitLinear.format = TextureFormat::Rgba8Unorm;
            m_hitLinear.name = "render hit data";
        }
        for (const RayCaster& c : casters) {
            const MaterialDesc& m = c.material->desc;
            want(colorWanted, m.textures[0]);
            want(colorWanted, m.textures[3]);
            want(linearWanted, m.textures[1]);
        }
        updateHitArray(commands, m_hitColor, scene.serial, colorWanted, colorLayers);
        updateHitArray(commands, m_hitLinear, scene.serial, linearWanted, linearLayers);
        stats.hitTextures =
            std::uint32_t(std::count_if(colorLayers.begin(), colorLayers.end(), [](int l) { return l >= 0; }) +
                          std::count_if(linearLayers.begin(), linearLayers.end(), [](int l) { return l >= 0; }));
    }
    const auto layerOf = [](const std::vector<std::pair<TextureId, const gpu::Texture*>>& list,
                            const std::vector<int>& layers, TextureId id) {
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].first == id) return float(layers[i]);
        return -1.0f;
    };

    // The bottom levels and the top level in passes of their own, timed as "blas" and "tlas"; a frame that builds,
    // refits or compacts no bottom level has no "blas" pass.
    gpu::AccelerationStructurePass bottomPass;
    bool bottomFailed = false;
    const auto bottomLevels = [&]() -> gpu::AccelerationStructurePass* {
        if (!bottomPass && !bottomFailed) {
            commands.beginTiming("blas");
            bottomPass = commands.beginAccelerationStructurePass();
            if (!bottomPass) {
                commands.endTiming();
                bottomFailed = true;
            }
        }
        return bottomPass ? &bottomPass : nullptr;
    };
    const auto endBottomLevels = [&] {
        if (!bottomPass) return;
        bottomPass.end();
        commands.endTiming();
    };
    const auto create = [this](const gpu::AccelerationStructureDesc& desc) -> gpu::AccelerationStructure {
        Result<gpu::AccelerationStructure> s = m_device->createAccelerationStructure(desc);
        if (!s) {
            log::error("render", "acceleration structure '{}': {}", desc.name, s.error());
            return {};
        }
        return std::move(*s);
    };
    bool replaced = false; // a bottom level the top level's last build had is another object now
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
            if (fresh) {
                inst.rayStructure = create({.vertexCount = mesh.vertexCount,
                                            .triangleCount = triangles,
                                            .allowRefit = true,
                                            .name = "render ray skinned"});
                replaced = true;
            }
            if (!inst.rayStructure) continue;
            gpu::AccelerationStructurePass* pass = bottomLevels();
            if (!pass) return false;
            if (fresh || inst.rayRefits >= kRefitsPerBuild) {
                pass->build(inst.rayStructure, geometry);
                // A crowd made in one frame would otherwise build again all in one frame.
                inst.rayRefits = fresh ? m_skinnedBuilds++ % kRefitsPerBuild : 0;
                ++stats.builds;
            } else {
                pass->refit(inst.rayStructure, geometry);
                ++inst.rayRefits;
                ++stats.refits;
            }
            bottom = &inst.rayStructure;
        } else {
            using Compaction = MeshEntry::Compaction;
            if (!mesh.rayStructure) {
                mesh.rayStructure =
                    create({.vertexCount = mesh.vertexCount, .triangleCount = triangles, .name = "render ray mesh"});
                if (!mesh.rayStructure) continue;
                gpu::AccelerationStructurePass* pass = bottomLevels();
                if (!pass) return false;
                pass->build(mesh.rayStructure, geometry);
                // Rigid meshes do not change: a compacted copy replaces the built one once its size is known.
                pass->queryCompactedSize(mesh.rayStructure);
                mesh.rayCompaction = Compaction::Queried;
                ++stats.builds;
                replaced = true;
            } else if (mesh.rayCompaction == Compaction::Queried) {
                if (const std::optional<std::uint32_t> size = m_device->compactedSize(mesh.rayStructure)) {
                    gpu::AccelerationStructure compacted =
                        *size > 0 ? create({.compactedSize = *size, .name = "render ray mesh"})
                                  : gpu::AccelerationStructure{};
                    mesh.rayCompaction = Compaction::Done;
                    gpu::AccelerationStructurePass* pass = compacted ? bottomLevels() : nullptr;
                    if (compacted && !pass) return false;
                    if (compacted) {
                        pass->copyAndCompact(mesh.rayStructure, compacted);
                        stats.compactedBytes +=
                            std::max(mesh.rayStructure.bytes(), compacted.bytes()) - compacted.bytes();
                        mesh.rayStructure = std::move(compacted); // the command buffer keeps the source alive
                        ++stats.compactions;
                        replaced = true;
                    }
                }
            }
            bottom = &mesh.rayStructure;
        }

        const MaterialDesc& m = c.material->desc;
        const bool cutOut = m.alphaCutoff > 0.0f;
        std::uint8_t mask = 0;
        const bool emitter = c.material->light > 0;
        if (c.caster) mask |= c.translucent ? QUENCH_RT_MASK_TRANSLUCENT : QUENCH_RT_MASK_CASTER;
        if (hitData)
            mask |= emitter         ? QUENCH_RT_MASK_EMITTER
                    : c.translucent ? QUENCH_RT_MASK_SEE_THROUGH
                                    : QUENCH_RT_MASK_VISIBLE;
        if (mask == 0) continue;
        gpu::RayInstance instance{.bottomLevel = bottom,
                                  .transform = rowsOf(c.instance->desc.transform),
                                  .mask = mask,
                                  .flags = c.translucent || cutOut ? gpu::RayInstanceFlags::NonOpaque
                                                                   : gpu::RayInstanceFlags::Opaque};
        if (hitData || c.translucent || cutOut) {
            const auto table = std::lower_bound(m_tableMeshes.begin(), m_tableMeshes.end(), c.instance->desc.mesh,
                                                [](const TableMesh& t, MeshId id) { return idLess(t.id, id); });
            int alphaSlot = -1;
            const TextureEntry* texture = scene.textures.get(m.textures[0]);
            if ((c.translucent || cutOut) && texture && texture->texture) {
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
            const std::uint32_t skinnedFirst =
                c.skinned.buffer ? 1 + c.skinned.offset / std::uint32_t(sizeof(Vertex)) : 0;
            const glm::mat3 normal = glm::transpose(glm::inverse(glm::mat3(c.instance->desc.transform)));
            shader::RayInstanceData d{};
            d.geometry = glm::vec4(std::bit_cast<float>(table->firstIndex), std::bit_cast<float>(table->firstVertex),
                                   std::bit_cast<float>(skinnedFirst),
                                   c.translucent ? 2.0f
                                   : cutOut      ? 1.0f
                                                 : 0.0f);
            d.color = m.baseColor * c.instance->desc.color;
            d.emissive = glm::vec4(m.emissive, cutOut ? m.alphaCutoff : 0.0f);
            // The shape of a one-sided area light: its back is a black surface (RtSurface::back).
            const bool twoSided = c.material->light > 0 ? c.material->lightTwoSided : m.doubleSided;
            d.surface = glm::vec4(m.roughness, m.metallic, float(alphaSlot), twoSided ? 1.0f : 0.0f);
            d.layers = glm::vec4(layerOf(colorWanted, colorLayers, m.textures[0]),
                                 layerOf(linearWanted, linearLayers, m.textures[1]),
                                 layerOf(colorWanted, colorLayers, m.textures[3]), float(c.material->light));
            d.normal0 = glm::vec4(normal[0][0], normal[1][0], normal[2][0], std::clamp(m.anisotropy, 0.0f, 1.0f));
            d.normal1 = glm::vec4(normal[0][1], normal[1][1], normal[2][1], std::cos(m.anisotropyRotation));
            d.normal2 = glm::vec4(normal[0][2], normal[1][2], normal[2][2], std::sin(m.anisotropyRotation));
            instance.id = std::uint32_t(m_data.size());
            m_data.push_back(d);
        }
        m_instances.push_back(instance);
    }
    endBottomLevels();
    if (m_instances.empty()) return false;
    const auto needed = std::uint32_t(m_instances.size());
    if (!m_top || m_top.desc().instanceCapacity < needed) {
        m_top = create({.level = gpu::AccelerationStructureLevel::Top,
                        .instanceCapacity = grown(m_top ? m_top.desc().instanceCapacity : 0, needed, 64),
                        .allowRefit = true,
                        .name = "render ray scene"});
        if (!m_top) return false;
        m_topBottoms.clear();
    }
    // The same bottom levels in the same order (instances that only moved): a refit, and a build now and then, as
    // a refitted top level grows worse the farther its instances move.
    const bool refit = !replaced && m_topRefits < kTopRefitsPerBuild &&
                       std::ranges::equal(m_instances, m_topBottoms, {}, &gpu::RayInstance::bottomLevel);
    commands.beginTiming("tlas");
    gpu::AccelerationStructurePass pass = commands.beginAccelerationStructurePass();
    if (!pass) {
        commands.endTiming();
        return false;
    }
    if (refit) {
        pass.refit(m_top, m_instances);
        ++m_topRefits;
    } else {
        pass.build(m_top, m_instances);
        m_topBottoms.clear();
        for (const gpu::RayInstance& i : m_instances) m_topBottoms.push_back(i.bottomLevel);
        m_topRefits = 0;
        std::vector<const gpu::AccelerationStructure*> distinct = m_topBottoms;
        std::ranges::sort(distinct);
        const auto [last, end] = std::ranges::unique(distinct);
        distinct.erase(last, end);
        m_structureBytes = m_top.bytes();
        for (const gpu::AccelerationStructure* b : distinct) m_structureBytes += b->bytes();
    }
    pass.end();
    commands.endTiming();
    stats.instances = needed;
    stats.topRefit = refit;

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
    stats.memoryBytes = memoryBytes();
    return true;
}

std::uint64_t RayTracing::memoryBytes() const {
    return m_structureBytes + m_vertexBytes + m_indexBytes + m_instanceBytes + textureBytes(m_hitColor.texture) +
           textureBytes(m_hitLinear.texture) + textureBytes(m_probeIrradiance[0]) * 2 +
           textureBytes(m_probeDistance[0]) * 2 + textureBytes(m_probeRays);
}

void RayTracing::bindHits(gpu::RenderPass& pass, bool probes) {
    const gpu::Sampler& point = *m_samplers.point;
    const gpu::Sampler& linear = *m_samplers.linear;
    const gpu::Sampler& material = *m_samplers.material;
    const gpu::TextureRef white(*m_samplers.white), array(*m_samplers.array);
    const bool grid = probes && m_probesValid;
    const gpu::TextureSamplerBinding samplers[] = {
        {depth(), point},
        {m_gbufferNormal ? gpu::TextureRef(m_gbufferNormal) : white, point},
        {m_hitColor.texture ? gpu::TextureRef(m_hitColor.texture) : array, material},
        {m_hitLinear.texture ? gpu::TextureRef(m_hitLinear.texture) : array, material},
        {grid ? gpu::TextureRef(probeIrradiance()) : white, linear},
        {grid ? gpu::TextureRef(probeDistance()) : white, linear},
        {*m_samplers.areaTable, point}};
    pass.bindSamplers(kFragment, 0, samplers);
    const std::array<gpu::BufferRef, 4> buffers{m_instanceTable, m_vertices,
                                                m_skinned ? m_skinned : gpu::BufferRef(m_vertices), m_indices};
    pass.bindStorageBuffers(kFragment, 0, buffers);
    pass.bindAccelerationStructure(kFragment, 0, m_top);
}

void RayTracing::filter(gpu::CommandBuffer& commands, TemporalFilter& f, const gpu::Texture& raw,
                        const gpu::Texture& output, const FilterInputs& inputs, const FilterParams& params,
                        RayFiltering filtering) {
    FilterInputs in = inputs;
    in.reset = in.reset || m_frameReset;
    f.run(commands, raw, output, in, params, filtering == RayFiltering::Temporal, *m_samplers.point,
          *m_samplers.linear);
}

void RayTracing::traceShadows(gpu::CommandBuffer& commands, const shader::RayTracingUniforms& uniforms,
                              const shader::LightUniforms& lights, const FilterInputs& inputs, RayFiltering filtering) {
    const bool local = uniforms.local.x > 0.0f && m_local;
    // Nothing to filter in the sun's mask (hard shadows, no occlusion): the rays write it directly.
    const bool soft = uniforms.denoise.x > 0.0f || uniforms.denoise.z > 0.0f;
    const bool rawMask = filtering == RayFiltering::Raw || !soft;
    commands.beginTiming("rays");
    std::array<gpu::ColorTarget, 2> targets{
        gpu::ColorTarget{.texture = rawMask ? m_mask : m_raw, .load = gpu::LoadOp::DontCare, .cycle = true},
        gpu::ColorTarget{.texture = filtering == RayFiltering::Raw ? m_local : m_localRaw,
                         .load = gpu::LoadOp::DontCare,
                         .cycle = true}};
    if (gpu::RenderPass pass = commands.beginRenderPass(std::span(targets.data(), local ? 2 : 1))) {
        std::array<gpu::TextureSamplerBinding, 1 + QUENCH_RT_ALPHA_TEXTURES> samplers{};
        samplers[0] = {depth(), *m_samplers.point};
        for (std::size_t i = 0; i < QUENCH_RT_ALPHA_TEXTURES; ++i)
            samplers[i + 1] = {i < m_alphaTextures.size() ? gpu::TextureRef(*m_alphaTextures[i])
                                                          : gpu::TextureRef(*m_samplers.white),
                               *m_samplers.material};
        const std::array<gpu::BufferRef, 3> buffers{m_instanceTable, m_vertices, m_indices};
        pass.bindPipeline(local ? m_traceLocalPipeline : m_tracePipeline);
        pass.bindSamplers(kFragment, 0, samplers);
        pass.bindStorageBuffers(kFragment, 0, buffers);
        pass.bindAccelerationStructure(kFragment, 0, m_top);
        pass.pushUniforms(kFragment, 1, uniforms);
        if (local) pass.pushUniforms(kFragment, 2, lights);
        pass.draw(3);
        pass.end();
    }
    commands.endTiming();
    if (filtering == RayFiltering::Raw) return;
    commands.beginTiming("denoise");
    if (!soft) {
        m_maskFilter.invalidate();
    } else if (filtering == RayFiltering::Temporal) {
        // Sun penumbrae and occlusion: a short history (moving casters), clamped; two a-trous passes.
        filter(commands, m_maskFilter, m_raw, m_mask, inputs,
               {.maxHistory = 16.0f, .clampGamma = 1.5f, .steps = 2, .planeTolerance = uniforms.denoise.y}, filtering);
    } else {
        const gpu::ColorTarget color{.texture = m_mask, .load = gpu::LoadOp::DontCare, .cycle = true};
        if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
            const gpu::TextureSamplerBinding samplers[] = {{m_raw, *m_samplers.point}, {depth(), *m_samplers.point}};
            pass.bindPipeline(m_denoisePipeline);
            pass.bindSamplers(kFragment, 0, samplers);
            pass.pushUniforms(kFragment, 1, uniforms);
            pass.draw(3);
            pass.end();
        }
    }
    if (local)
        filter(commands, m_localFilter, m_localRaw, m_local, inputs,
               {.maxHistory = 16.0f, .clampGamma = 1.5f, .steps = 2, .planeTolerance = uniforms.denoise.y}, filtering);
    commands.endTiming();
}

ProbeGridState RayTracing::placeProbes(const glm::vec3& camera, float spacing) {
    ProbeGridState g;
    g.spacing = std::max(spacing, 0.1f);
    g.counts = kProbeCounts;
    const glm::ivec3 cell(glm::floor(camera / g.spacing));
    g.first = cell - kProbeCounts / 2;
    if (std::abs(g.spacing - m_grid.spacing) > 1e-4f) m_probesValid = false;
    m_grid = g;
    return g;
}

void RayTracing::updateProbes(gpu::CommandBuffer& commands, shader::RayHitUniforms uniforms,
                              const shader::LightUniforms& lights, bool reset) {
    if (!m_lighting || !m_probeRays) return;
    const bool restart = reset || m_frameReset || !m_probesValid;
    uniforms.probes = glm::vec4(float(kProbeRays), m_grid.spacing * 2.0f, 0.97f, restart ? 1.0f : 0.0f);
    uniforms.previousGrid = glm::vec4(glm::vec3(m_previousFirst), 0.0f);
    // The rays see the grid's last state (no probes in the first frame).
    uniforms.params.w = restart ? 0.0f : 1.0f;
    commands.beginTiming("probes");
    {
        const gpu::ColorTarget color{.texture = m_probeRays, .load = gpu::LoadOp::DontCare, .cycle = true};
        if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
            pass.bindPipeline(m_probeTracePipeline);
            bindHits(pass, !restart);
            pass.pushUniforms(kFragment, 1, uniforms);
            pass.pushUniforms(kFragment, 2, lights);
            pass.draw(3);
            pass.end();
        }
    }
    const int write = m_probeIndex ^ 1;
    const auto update = [&](const gpu::GraphicsPipeline& pipeline, const gpu::Texture& target,
                            const gpu::Texture& previous) {
        const gpu::ColorTarget color{.texture = target, .load = gpu::LoadOp::DontCare, .cycle = true};
        gpu::RenderPass pass = commands.beginRenderPass(color);
        if (!pass) return;
        const gpu::TextureSamplerBinding samplers[] = {{m_probeRays, *m_samplers.point}, {previous, *m_samplers.point}};
        pass.bindPipeline(pipeline);
        pass.bindSamplers(kFragment, 0, samplers);
        pass.pushUniforms(kFragment, 1, uniforms);
        pass.draw(3);
        pass.end();
    };
    update(m_probeIrradiancePipeline, m_probeIrradiance[std::size_t(write)],
           m_probeIrradiance[std::size_t(m_probeIndex)]);
    update(m_probeDistancePipeline, m_probeDistance[std::size_t(write)], m_probeDistance[std::size_t(m_probeIndex)]);
    commands.endTiming();
    m_probeIndex = write;
    m_probesValid = true;
    m_previousFirst = m_grid.first;
}

void RayTracing::traceReflections(gpu::CommandBuffer& commands, const shader::RayHitUniforms& uniforms,
                                  const shader::LightUniforms& lights, const FilterInputs& inputs,
                                  RayFiltering filtering) {
    if (!m_lighting || !m_reflections) return;
    commands.beginTiming("reflections");
    const gpu::ColorTarget color{.texture = filtering == RayFiltering::Raw ? m_reflections : m_reflectionsRaw,
                                 .load = gpu::LoadOp::DontCare,
                                 .cycle = true};
    if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
        pass.bindPipeline(m_reflectPipeline);
        bindHits(pass, uniforms.params.w > 0.5f);
        pass.pushUniforms(kFragment, 1, uniforms);
        pass.pushUniforms(kFragment, 2, lights);
        pass.draw(3);
        pass.end();
    }
    // Smooth surfaces keep a short history and a narrow filter (their reflections are sharp), rough ones long ones.
    if (filtering != RayFiltering::Raw)
        filter(commands, m_reflectionFilter, m_reflectionsRaw, m_reflections, inputs,
               {.maxHistory = 24.0f, .clampGamma = 1.25f, .alphaScale = true, .steps = 2, .planeTolerance = 0.02f},
               filtering);
    commands.endTiming();
}

void RayTracing::traceIndirect(gpu::CommandBuffer& commands, const shader::RayHitUniforms& uniforms,
                               const shader::LightUniforms& lights, const FilterInputs& inputs,
                               RayFiltering filtering) {
    if (!m_lighting || !m_indirect) return;
    commands.beginTiming("indirect");
    const gpu::ColorTarget color{.texture = filtering == RayFiltering::Raw ? m_indirect : m_indirectRaw,
                                 .load = gpu::LoadOp::DontCare,
                                 .cycle = true};
    if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
        pass.bindPipeline(m_indirectPipeline);
        bindHits(pass, uniforms.params.w > 0.5f);
        pass.pushUniforms(kFragment, 1, uniforms);
        pass.pushUniforms(kFragment, 2, lights);
        pass.draw(3);
        pass.end();
    }
    // One ray per pixel is the noisiest signal: a long history, a loose clamp and the widest filter.
    if (filtering != RayFiltering::Raw)
        filter(commands, m_indirectFilter, m_indirectRaw, m_indirect, inputs,
               {.maxHistory = 32.0f, .clampGamma = 2.5f, .steps = 2, .planeTolerance = 0.03f}, filtering);
    commands.endTiming();
}

bool RayTracing::writeDenoiseInputs(gpu::CommandBuffer& commands, bool prepass) {
    if (!m_diffuseAlbedo || !m_gbufferNormal) return false;
    if (!prepass) {
        const gpu::ColorTarget sky[] = {{.texture = m_diffuseAlbedo, .clear = {1.0f, 1.0f, 1.0f, 1.0f}, .cycle = true},
                                        {.texture = m_specularAlbedo, .clear = {0.0f, 0.0f, 0.0f, 1.0f}, .cycle = true},
                                        {.texture = m_roughness, .clear = {1.0f, 0.0f, 0.0f, 0.0f}, .cycle = true},
                                        {.texture = m_gbufferNormal, .clear = {0.0f, 1.0f, 0.0f, 1.0f}, .cycle = true}};
        gpu::RenderPass pass = commands.beginRenderPass(sky);
        if (!pass) return false;
        pass.end();
        return true;
    }
    const gpu::ColorTarget targets[] = {{.texture = m_diffuseAlbedo, .load = gpu::LoadOp::DontCare, .cycle = true},
                                        {.texture = m_specularAlbedo, .load = gpu::LoadOp::DontCare, .cycle = true},
                                        {.texture = m_roughness, .load = gpu::LoadOp::DontCare, .cycle = true}};
    gpu::RenderPass pass = commands.beginRenderPass(targets);
    if (!pass) return false;
    const gpu::TextureSamplerBinding samplers[] = {
        {depth(), *m_samplers.point}, {m_gbufferNormal, *m_samplers.point}, {m_gbufferAlbedo, *m_samplers.point}};
    pass.bindPipeline(m_denoiseInputsPipeline);
    pass.bindSamplers(kFragment, 0, samplers);
    pass.draw(3);
    pass.end();
    return true;
}

} // namespace quench::render::detail
