#include "Particles.h"

#include "shaders/ParticleUniforms.h"

#include <quench/core/Log.h>
#include <quench/core/Profile.h>
#include <quench/particles/Simulation.h>

#include <render_particles_shaders.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace quench::render::detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureUsage;

constexpr ShaderStage kVertex = ShaderStage::Vertex;
constexpr ShaderStage kFragment = ShaderStage::Fragment;
// The longest step: a longer gap between frames (a hitch, a pause) moves the particles this far only.
constexpr double kMaxStep = 0.25;
// The scene's state buffers start with this many slots and double.
constexpr std::uint32_t kMinPoolSlots = 4096;
constexpr std::uint32_t kMinSpawns = 1024;
constexpr auto kParticleBytes = std::uint32_t(sizeof(particles::Particle));
constexpr auto kInstanceBytes = std::uint32_t(sizeof(particles::ParticleInstance));
constexpr auto kTrailVertexBytes = std::uint32_t(sizeof(particles::TrailVertex));

static_assert(sizeof(particles::Particle) == 48 && sizeof(particles::ParticleInstance) == 48,
              "three float4 per particle (particles.hlsl)");
static_assert(sizeof(particles::TrailVertex) == 32, "two float4 per trail vertex (particles.hlsl)");

std::uint32_t grown(std::uint32_t capacity, std::uint64_t needed, std::uint32_t least) {
    std::uint64_t c = std::max(capacity, least);
    while (c < needed) c *= 2;
    return std::uint32_t(std::min<std::uint64_t>(c, 0xffffffffu));
}

float spriteOf(particles::Sprite sprite) {
    switch (sprite) {
    case particles::Sprite::Glow: return QUENCH_PARTICLE_SPRITE_GLOW;
    case particles::Sprite::Soft: return QUENCH_PARTICLE_SPRITE_SOFT;
    case particles::Sprite::Star: return QUENCH_PARTICLE_SPRITE_STAR;
    case particles::Sprite::Puff: return QUENCH_PARTICLE_SPRITE_PUFF;
    }
    return QUENCH_PARTICLE_SPRITE_GLOW;
}

} // namespace

bool ParticleRenderer::init(gpu::Device& device, TextureFormat hdrFormat, std::uint32_t& pipelines) {
    m_device = &device;
    m_hdrFormat = hdrFormat;
    const auto& set = shaders::render_particles;
    // Premultiplied light over the image without changing its alpha: additive particles write alpha 0.
    gpu::BlendState blend = gpu::BlendState::premultiplied();
    blend.writeMask = gpu::ColorWrite::R | gpu::ColorWrite::G | gpu::ColorWrite::B;
    const gpu::ColorTargetDesc target{hdrFormat, blend};
    const auto make = [&](gpu::GraphicsPipeline& out, const char* vertex, const char* fragment, const char* name) {
        const ShaderBlob* vs = set.find(vertex);
        const ShaderBlob* fs = set.find(fragment);
        if (!vs || !fs) {
            log::error("render", "pipeline '{}': the shader is not in this build", name);
            return false;
        }
        Result<gpu::GraphicsPipeline> p = device.createPipeline(
            {.vertexShader = vs, .fragmentShader = fs, .colorTargets = {&target, 1}, .name = name});
        if (!p) {
            log::error("render", "pipeline '{}': {}", name, p.error());
            return false;
        }
        out = std::move(*p);
        ++pipelines;
        return true;
    };
    if (!make(m_particlePipeline, "particle_vs", "particle_fs", "render particles") ||
        !make(m_trailPipeline, "trail_vs", "trail_fs", "render trails")) {
        *this = ParticleRenderer{};
        log::error("render", "particle pipelines failed (logged); no particles");
        return false;
    }
    if (device.caps().computeShaders) {
        const ShaderBlob* shader = set.find("particle_step_cs");
        Result<gpu::ComputePipeline> p =
            shader ? device.createComputePipeline({.shader = shader, .name = "render particle step"})
                   : Result<gpu::ComputePipeline>(Error{"the shader is not in this build"});
        if (p) {
            m_stepPipeline = std::move(*p);
            ++pipelines;
        } else {
            log::error("render", "pipeline 'render particle step': {}; particles are stepped on the CPU", p.error());
        }
    }
    return true;
}

void ParticleRenderer::release() {
    m_image = {};
    m_spawnBuffer = {};
    m_trailBuffer = {};
    m_headBuffer = {};
    m_spawnCapacity = m_trailCapacity = m_headCapacity = 0;
    m_transfer = {};
    m_transferCapacity = 0;
}

bool ParticleRenderer::ensureBuffer(gpu::Buffer& buffer, std::uint32_t& capacity, std::uint32_t bytes,
                                    gpu::BufferUsage usage, const char* name) {
    if (buffer && capacity >= bytes) return true;
    const std::uint32_t size = grown(capacity, bytes, 64u * 1024u);
    Result<gpu::Buffer> b = m_device->createBuffer({.usage = usage, .size = size, .name = name});
    if (!b) {
        log::error("render", "{} ({} bytes): {}; no particles this frame", name, size, b.error());
        return false;
    }
    buffer = std::move(*b);
    capacity = size;
    return true;
}

bool ParticleRenderer::ensurePool(gpu::CommandBuffer& commands, ParticlePool& pool) {
    const bool compute = m_device->caps().computeShaders;
    if (pool.instances && (pool.state || !compute) && pool.capacity >= pool.end) return true;
    const std::uint32_t slots = grown(pool.capacity, pool.end, kMinPoolSlots);
    const gpu::BufferUsage instanceUsage = gpu::BufferUsage::GraphicsStorageRead |
                                           (compute ? gpu::BufferUsage::ComputeStorageWrite : gpu::BufferUsage::None);
    Result<gpu::Buffer> instances =
        m_device->createBuffer({.usage = instanceUsage, .size = slots * kInstanceBytes, .name = "render particles"});
    Result<gpu::Buffer> state = compute ? m_device->createBuffer({.usage = gpu::BufferUsage::ComputeStorageWrite,
                                                                  .size = slots * kParticleBytes,
                                                                  .name = "render particle state"})
                                        : Result<gpu::Buffer>(gpu::Buffer{});
    if (!instances || !state) {
        log::error("render", "particle buffers ({} slots): {}; no particles this frame", slots,
                   instances ? state.error() : instances.error());
        return false;
    }
    // The GPU's particles so far move to the new state buffer.
    if (pool.state && *state) {
        gpu::CopyPass copy = commands.beginCopyPass();
        copy.copy(pool.state, 0, *state, 0, pool.capacity * kParticleBytes);
        copy.end();
    }
    pool.state = std::move(*state);
    pool.instances = std::move(*instances);
    pool.capacity = slots;
    return true;
}

void ParticleRenderer::step(SceneImpl& scene, const Frame& frame, RenderStats& stats) {
    QUENCH_PROFILE_ZONE("render.particles.step");
    ParticlePool& pool = scene.particlePool;
    const float dt = pool.stepped ? float(std::clamp(frame.time - pool.lastTime, 0.0, kMaxStep)) : 0.0f;
    pool.lastTime = frame.time;
    pool.stepped = true;
    const auto time = float(std::fmod(std::max(frame.time, 0.0), double(QUENCH_PARTICLE_PERIOD)));
    const shader::WindUniforms wind = scene.weather ? scene.weather->windUniforms() : shader::WindUniforms{};
    const bool onGpu = frame.mode == ParticleSimulation::Gpu;
    const ParticleHome home = onGpu ? ParticleHome::Gpu : ParticleHome::Cpu;
    m_gpuSpawns.clear();
    m_cpuSpawns.clear();
    m_cpuInstances.clear();
    m_cpuRanges.clear();
    m_stepUniforms.clear();
    for (std::uint32_t i = 0; i < scene.particleSystems.capacity(); ++i) {
        ParticleEntry* e = scene.particleSystems.slot(i);
        if (!e || !e->visible) continue;
        const std::uint32_t capacity = e->emitter.capacity();
        // A system that moves to the other simulation starts again there.
        if (e->home != home) {
            e->emitter.restart();
            e->home = home;
            e->cpu.reset();
        }
        if (!onGpu && !e->cpu) {
            e->cpu = std::make_unique<particles::Simulation>();
            e->cpu->resize(capacity);
        }
        e->emitter.setRateScale(frame.density);
        particles::ParticleUniforms u = e->uniforms;
        u.wind = wind;
        u.range.x = float(e->offset);
        e->emitter.step(dt, time, u, onGpu ? m_gpuSpawns : m_cpuSpawns);
        stats.particleSpawns += std::uint32_t(u.range.w);
        if (onGpu) {
            m_stepUniforms.push_back(u);
            continue;
        }
        e->cpu->step(u, m_cpuSpawns);
        const std::size_t at = m_cpuInstances.size();
        m_cpuInstances.resize(at + capacity);
        e->cpu->look(u, std::span(m_cpuInstances).subspan(at, capacity));
        m_cpuRanges.push_back({e->offset, capacity});
    }
}

void ParticleRenderer::buildTrails(SceneImpl& scene, const Frame& frame, RenderStats& stats) {
    m_trailVertices.clear();
    m_heads.clear();
    for (std::uint32_t i = 0; i < scene.trails.capacity(); ++i) {
        TrailEntry* e = scene.trails.slot(i);
        if (!e || !e->visible) continue;
        // The strip ages by View::time's change since this frame first saw the trail's last move.
        if (e->anchoredMove != e->moves) {
            e->anchoredMove = e->moves;
            e->anchorView = frame.time;
            e->anchorTrail = e->trail.time();
        }
        const double time = e->anchorTrail + std::max(frame.time - e->anchorView, 0.0);
        const particles::Trail& t = e->trail;
        const particles::TrailDesc& d = t.desc();
        std::uint32_t head = ~0u;
        if (t.emitting() && t.started() && d.headSize > 0.0f) {
            head = std::uint32_t(m_heads.size());
            particles::ParticleInstance h{};
            h.a = glm::vec4(t.head(), d.headSize);
            h.b = glm::vec4(d.headColor, 1.0f);
            m_heads.push_back(h);
        }
        const std::size_t at = m_trailVertices.size();
        std::uint32_t count = 0;
        if (t.visible(time)) {
            m_trailVertices.resize(at + t.maxVertices());
            count = t.build(time, std::span(m_trailVertices).subspan(at));
            m_trailVertices.resize(at + count);
        }
        if (count < 2 && head == ~0u) continue;
        const glm::vec3 place = count > 0 ? glm::vec3(m_trailVertices[at].position) : t.head();
        m_items.push_back({.distance = glm::length(place - glm::vec3(frame.invView[3])),
                           .index = i,
                           .trail = true,
                           .first = std::uint32_t(at),
                           .count = count,
                           .head = head});
        if (count >= 2) {
            ++stats.trails;
            stats.trailVertices += count;
        }
    }
}

bool ParticleRenderer::upload(gpu::CommandBuffer& commands, ParticlePool& pool) {
    const auto spawnBytes = std::uint32_t(m_gpuSpawns.size()) * kParticleBytes;
    const auto cpuBytes = std::uint32_t(m_cpuInstances.size()) * kInstanceBytes;
    const auto trailBytes = std::uint32_t(m_trailVertices.size()) * kTrailVertexBytes;
    const auto headBytes = std::uint32_t(m_heads.size()) * kInstanceBytes;
    if (!m_stepUniforms.empty() &&
        !ensureBuffer(m_spawnBuffer, m_spawnCapacity, std::max(spawnBytes, kMinSpawns * kParticleBytes),
                      gpu::BufferUsage::ComputeStorageRead, "render particle spawns"))
        return false;
    if (trailBytes > 0 && !ensureBuffer(m_trailBuffer, m_trailCapacity, trailBytes,
                                        gpu::BufferUsage::GraphicsStorageRead, "render trails"))
        return false;
    if (headBytes > 0 && !ensureBuffer(m_headBuffer, m_headCapacity, headBytes, gpu::BufferUsage::GraphicsStorageRead,
                                       "render trail heads"))
        return false;
    const std::uint32_t total = spawnBytes + cpuBytes + trailBytes + headBytes;
    if (total == 0) return true;
    if (!m_transfer || m_transferCapacity < total) {
        const std::uint32_t size = grown(m_transferCapacity, total, 64u * 1024u);
        Result<gpu::TransferBuffer> t = m_device->createTransferBuffer({.size = size, .name = "render particles"});
        if (!t) {
            log::error("render", "particle transfer buffer ({} bytes): {}; no particles this frame", size, t.error());
            return false;
        }
        m_transfer = std::move(*t);
        m_transferCapacity = size;
    }
    std::span<std::byte> mapped = m_transfer.map(true);
    if (mapped.size() < total) return false;
    std::uint32_t at = 0;
    const auto put = [&](const void* data, std::uint32_t bytes) {
        const std::uint32_t offset = at;
        if (bytes > 0) std::memcpy(mapped.data() + offset, data, bytes);
        at += bytes;
        return offset;
    };
    const std::uint32_t spawnAt = put(m_gpuSpawns.data(), spawnBytes);
    const std::uint32_t cpuAt = put(m_cpuInstances.data(), cpuBytes);
    const std::uint32_t trailAt = put(m_trailVertices.data(), trailBytes);
    const std::uint32_t headAt = put(m_heads.data(), headBytes);
    m_transfer.unmap();
    gpu::CopyPass copy = commands.beginCopyPass();
    if (spawnBytes > 0) copy.upload(m_transfer, spawnAt, m_spawnBuffer, 0, spawnBytes, true);
    std::uint32_t from = cpuAt;
    for (std::size_t r = 0; r < m_cpuRanges.size(); ++r) {
        const std::uint32_t bytes = m_cpuRanges[r].size * kInstanceBytes;
        copy.upload(m_transfer, from, pool.instances, m_cpuRanges[r].offset * kInstanceBytes, bytes, r == 0);
        from += bytes;
    }
    if (trailBytes > 0) copy.upload(m_transfer, trailAt, m_trailBuffer, 0, trailBytes, true);
    if (headBytes > 0) copy.upload(m_transfer, headAt, m_headBuffer, 0, headBytes, true);
    copy.end();
    return true;
}

gpu::TextureRef ParticleRenderer::render(gpu::CommandBuffer& commands, SceneImpl& scene, const Frame& frame,
                                         const gpu::Sampler& pointSampler, const shader::FrameUniforms& fu,
                                         RenderStats& stats) {
    QUENCH_PROFILE_ZONE("render.particles");
    gpu::TextureRef image = frame.image;
    if (!ready() || !frame.depth || !image || frame.mode == ParticleSimulation::Off) return image;
    const ParticleSimulation mode =
        frame.mode == ParticleSimulation::Gpu && computes() ? ParticleSimulation::Gpu : ParticleSimulation::Cpu;
    Frame f = frame;
    f.mode = mode;
    ParticlePool& pool = scene.particlePool;
    m_items.clear();
    if (scene.particleSystems.size() > 0) {
        if (!ensurePool(commands, pool)) return image;
        step(scene, f, stats);
    }
    buildTrails(scene, f, stats);
    if (!upload(commands, pool)) return image;

    if (!m_stepUniforms.empty()) {
        commands.beginTiming("particles_sim");
        const gpu::StorageBufferWrite writes[] = {{.buffer = pool.state, .cycle = false},
                                                  {.buffer = pool.instances, .cycle = true}};
        if (gpu::ComputePass pass = commands.beginComputePass(writes)) {
            pass.bindPipeline(m_stepPipeline);
            const gpu::BufferRef spawns = m_spawnBuffer;
            pass.bindStorageBuffers(0, {&spawns, 1});
            for (const particles::ParticleUniforms& u : m_stepUniforms) {
                pass.pushUniforms(0, u);
                pass.dispatch((std::uint32_t(u.range.y) + QUENCH_PARTICLE_GROUP - 1) / QUENCH_PARTICLE_GROUP);
            }
            pass.end();
        }
        commands.endTiming();
    }

    const glm::vec3 camera(frame.invView[3]);
    for (std::uint32_t i = 0; i < scene.particleSystems.capacity(); ++i) {
        const ParticleEntry* e = scene.particleSystems.slot(i);
        if (!e || !e->visible || e->home == ParticleHome::None) continue;
        const std::uint32_t alive = e->emitter.alive();
        if (alive == 0) continue;
        ++stats.particleSystems;
        stats.particles += alive;
        stats.particleSlots += e->emitter.capacity();
        m_items.push_back({.distance = glm::length(glm::vec3(e->emitter.transform()[3]) - camera),
                           .index = i,
                           .trail = false,
                           .first = e->offset,
                           .count = e->emitter.capacity(),
                           .head = ~0u});
    }
    if (m_items.empty()) return image;
    std::sort(m_items.begin(), m_items.end(), [](const Item& a, const Item& b) { return a.distance > b.distance; });

    const std::uint32_t w = image.desc.width, h = image.desc.height;
    if (frame.keepImage) {
        if (!m_image || m_image.desc().width != w || m_image.desc().height != h) {
            m_image = {};
            Result<gpu::Texture> t =
                m_device->createTexture({.format = m_hdrFormat,
                                         .usage = TextureUsage::ColorTarget | TextureUsage::Sampled,
                                         .width = w,
                                         .height = h,
                                         .name = "render particle image"});
            if (!t) {
                log::error("render", "particle image {} x {}: {}; no particles", w, h, t.error());
                return image;
            }
            m_image = std::move(*t);
        }
        {
            gpu::CopyPass copy = commands.beginCopyPass();
            copy.copy({.texture = image}, {.texture = m_image});
        }
        image = m_image;
    } else {
        m_image = {};
    }

    commands.beginTiming("particles");
    gpu::RenderPass pass = commands.beginRenderPass(gpu::ColorTarget{.texture = image, .load = gpu::LoadOp::Load});
    if (!pass) {
        commands.endTiming();
        return image;
    }
    shader::ParticleDrawUniforms pd{};
    pd.viewProj = frame.viewProjStable;
    pd.target = glm::vec4(float(w), float(h), 1.0f / float(w), 1.0f / float(h));
    pd.camera = glm::vec4(camera, frame.nearPlane);
    pd.right = glm::vec4(glm::normalize(glm::vec3(frame.invView[0])), frame.proj[1][1] * float(h) * 0.5f);
    pd.up = glm::vec4(glm::normalize(glm::vec3(frame.invView[1])), 0.0f);
    pass.pushUniforms(kVertex, 0, fu);
    pass.pushUniforms(kFragment, 0, fu);
    const gpu::TextureSamplerBinding depth{*frame.depth, pointSampler};
    const gpu::BufferRef instances = pool.instances;
    const gpu::BufferRef trailVertices = m_trailBuffer;
    const gpu::BufferRef heads = m_headBuffer;
    const auto drawParticles = [&](const gpu::BufferRef& buffer, std::uint32_t instanceCount) {
        pass.bindPipeline(m_particlePipeline);
        pass.bindStorageBuffers(kVertex, 0, {&buffer, 1});
        pass.bindSampler(kFragment, 0, depth);
        pass.pushUniforms(kVertex, 1, pd);
        pass.pushUniforms(kFragment, 1, pd);
        pass.draw(6, instanceCount);
    };
    for (const Item& item : m_items) {
        if (!item.trail) {
            const particles::ParticleDesc& d = scene.particleSystems.slot(item.index)->emitter.desc();
            const bool alpha = d.blend == particles::Blend::Alpha;
            pd.draw = glm::vec4(float(item.first), spriteOf(d.sprite),
                                d.facing == particles::Facing::Velocity ? 1.0f : 0.0f, d.stretch);
            pd.fade = glm::vec4(std::max(d.softness, 0.0f), std::max(d.nearFade, 0.0f), std::max(d.minPixels, 0.0f),
                                alpha ? 1.0f : 0.0f);
            pd.light = glm::vec4(alpha && d.lit ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            drawParticles(instances, item.count);
            continue;
        }
        const particles::TrailDesc& d = scene.trails.slot(item.index)->trail.desc();
        if (item.count >= 2) {
            pd.draw = glm::vec4(float(item.first), float(item.count), 0.0f, 0.0f);
            pd.fade = glm::vec4(std::max(d.softness, 0.0f), 0.0f, std::max(d.minPixels, 0.0f),
                                d.blend == particles::Blend::Alpha ? 1.0f : 0.0f);
            pd.light = glm::vec4(0.0f);
            pass.bindPipeline(m_trailPipeline);
            pass.bindStorageBuffers(kVertex, 0, {&trailVertices, 1});
            pass.bindSampler(kFragment, 0, depth);
            pass.pushUniforms(kVertex, 1, pd);
            pass.pushUniforms(kFragment, 1, pd);
            pass.draw((item.count - 1) * 6);
        }
        if (item.head != ~0u) {
            pd.draw = glm::vec4(float(item.head), float(QUENCH_PARTICLE_SPRITE_GLOW), 0.0f, 0.0f);
            pd.fade = glm::vec4(std::max(d.softness, 0.0f), 0.0f, std::max(d.minPixels, 0.0f), 0.0f);
            pd.light = glm::vec4(0.0f);
            drawParticles(heads, 1);
        }
    }
    pass.end();
    commands.endTiming();
    return image;
}

} // namespace quench::render::detail
