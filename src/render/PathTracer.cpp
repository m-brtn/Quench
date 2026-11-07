#include "PathTracer.h"

#include <quench/core/Log.h>

#include <glm/vec4.hpp>

#include <algorithm>

namespace quench::render::detail {
namespace {

// 32-bit floats: a mean of thousands of samples in half floats stops moving long before it converges.
constexpr gpu::TextureFormat kAccumulationFormat = gpu::TextureFormat::Rgba32Float;

} // namespace

bool PathTracer::init(gpu::Device& device, RayTracing& rays, std::uint32_t& pipelines) {
    m_device = &device;
    Result<gpu::GraphicsPipeline> p = rays.hitPipeline("pt_fs", kAccumulationFormat, "render path tracer");
    Result<gpu::GraphicsPipeline> maps =
        rays.hitPipeline("pt_maps_fs", kAccumulationFormat, "render path tracer (normal maps)");
    if (!p || !maps) {
        log::error("render", "path tracer pipeline: {}", !p ? p.error() : maps.error());
        return false;
    }
    m_pipeline = std::move(*p);
    m_mapsPipeline = std::move(*maps);
    pipelines += 2;
    return true;
}

bool PathTracer::ensureTargets(std::uint32_t width, std::uint32_t height) {
    if (m_accumulation[0] && m_width == width && m_height == height) return true;
    release();
    for (int i = 0; i < 2; ++i) {
        Result<gpu::Texture> t =
            m_device->createTexture({.format = kAccumulationFormat,
                                     .usage = gpu::TextureUsage::ColorTarget | gpu::TextureUsage::Sampled,
                                     .width = width,
                                     .height = height,
                                     .name = "render path tracer"});
        if (!t) {
            log::error("render", "target 'render path tracer' {} x {}: {}", width, height, t.error());
            release();
            return false;
        }
        m_accumulation[std::size_t(i)] = std::move(*t);
    }
    m_width = width;
    m_height = height;
    return true;
}

void PathTracer::release() {
    m_accumulation = {};
    m_width = m_height = 0;
    m_samples = 0;
}

const gpu::Texture& PathTracer::render(gpu::CommandBuffer& commands, RayTracing& rays, shader::RayHitUniforms uniforms,
                                       const shader::LightUniforms& lights, std::uint64_t state, int bounces,
                                       int maxSamples, const gpu::Sampler& point) {
    if (state != m_state) {
        m_state = state;
        m_samples = 0;
    }
    if (m_samples >= std::uint32_t(std::max(maxSamples, 1))) return m_accumulation[std::size_t(m_index)];
    const int write = m_index ^ 1;
    uniforms.path = glm::vec4(float(std::clamp(bounces, 1, 16)), float(m_samples), m_samples == 0 ? 1.0f : 0.0f, 0.0f);
    commands.beginTiming("path");
    const gpu::ColorTarget color{
        .texture = m_accumulation[std::size_t(write)], .load = gpu::LoadOp::DontCare, .cycle = true};
    if (gpu::RenderPass pass = commands.beginRenderPass(color)) {
        pass.bindPipeline(rays.normalMaps() ? m_mapsPipeline : m_pipeline);
        rays.bindHits(pass, false);
        pass.bindSampler(ShaderStage::Fragment, 7, {m_accumulation[std::size_t(m_index)], point});
        pass.pushUniforms(ShaderStage::Fragment, 1, uniforms);
        pass.pushUniforms(ShaderStage::Fragment, 2, lights);
        pass.draw(3);
        pass.end();
    }
    commands.endTiming();
    m_index = write;
    ++m_samples;
    return m_accumulation[std::size_t(m_index)];
}

} // namespace quench::render::detail
