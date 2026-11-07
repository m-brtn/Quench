#include "TemporalFilter.h"

#include <quench/core/Log.h>

#include <render_frame_shaders.h>
#include <render_raydenoise_shaders.h>

#include <glm/vec4.hpp>

#include <algorithm>
#include <format>
#include <span>
#include <string>

namespace quench::render::detail {
namespace {

using gpu::TextureFormat;
using gpu::TextureUsage;

constexpr ShaderStage kFragment = ShaderStage::Fragment;
constexpr TextureFormat kLengthFormat = TextureFormat::Rg16Float;

} // namespace

bool TemporalFilter::init(gpu::Device& device, TextureFormat format, const char* name, std::uint32_t& pipelines,
                          bool deviation) {
    m_device = &device;
    m_format = format;
    m_name = name;
    m_deviationFloor = deviation;
    const ShaderBlob* fullscreen = shaders::render_frame.find("fullscreen_vs");
    const gpu::ColorTargetDesc targets[] = {{format}, {kLengthFormat}, {format}};
    Result<gpu::GraphicsPipeline> temporal = device.createPipeline(
        {.vertexShader = fullscreen,
         .fragmentShader = shaders::render_raydenoise.find(deviation ? "rt_temporal_deviation_fs" : "rt_temporal_fs"),
         .colorTargets = {targets, deviation ? 3u : 2u},
         .name = "render ray temporal"});
    Result<gpu::GraphicsPipeline> atrous =
        device.createPipeline({.vertexShader = fullscreen,
                               .fragmentShader = shaders::render_raydenoise.find("rt_atrous_fs"),
                               .colorTargets = {&targets[0], 1},
                               .name = "render ray a-trous"});
    if (!temporal || !atrous) {
        log::error("render", "denoiser pipelines ({}): {}", name, temporal ? atrous.error() : temporal.error());
        return false;
    }
    m_temporal = std::move(*temporal);
    m_atrous = std::move(*atrous);
    pipelines += 2;
    return true;
}

bool TemporalFilter::ensureTargets(std::uint32_t width, std::uint32_t height) {
    if (m_scratch && m_width == width && m_height == height) return true;
    release();
    const auto create = [this, width, height](gpu::Texture& out, TextureFormat format, const char* what) {
        const std::string name = std::format("render {} {}", m_name, what);
        Result<gpu::Texture> t = m_device->createTexture({.format = format,
                                                          .usage = TextureUsage::ColorTarget | TextureUsage::Sampled,
                                                          .width = width,
                                                          .height = height,
                                                          .name = name.c_str()});
        if (!t) {
            log::error("render", "target '{}' {} x {}: {}", name, width, height, t.error());
            return false;
        }
        out = std::move(*t);
        return true;
    };
    for (int i = 0; i < 2; ++i)
        if (!create(m_history[std::size_t(i)], m_format, "history") ||
            !create(m_length[std::size_t(i)], kLengthFormat, "history length") ||
            (m_deviationFloor && !create(m_deviation[std::size_t(i)], m_format, "history deviation"))) {
            release();
            return false;
        }
    if (!create(m_scratch, m_format, "filter")) {
        release();
        return false;
    }
    m_width = width;
    m_height = height;
    return true;
}

void TemporalFilter::release() {
    m_history = {};
    m_length = {};
    m_deviation = {};
    m_scratch = {};
    m_width = m_height = 0;
    m_valid = false;
}

void TemporalFilter::run(gpu::CommandBuffer& commands, const gpu::Texture& signal, const gpu::Texture& output,
                         const FilterInputs& inputs, const FilterParams& params, bool temporal,
                         const gpu::Sampler& point, const gpu::Sampler& linear) {
    shader::RayDenoiseUniforms u{};
    u.temporal = glm::vec4(params.maxHistory, params.depthTolerance, params.alphaScale ? 1.0f : 0.0f,
                           inputs.reset || !m_valid ? 1.0f : 0.0f);
    gpu::TextureRef source = signal;
    if (temporal) {
        const int write = m_index ^ 1;
        const std::size_t extra = m_deviationFloor ? 1 : 0; // the deviation's target and sampler
        const gpu::ColorTarget targets[] = {
            {.texture = m_history[std::size_t(write)], .load = gpu::LoadOp::DontCare, .cycle = true},
            {.texture = m_length[std::size_t(write)], .load = gpu::LoadOp::DontCare, .cycle = true},
            {.texture = m_deviation[std::size_t(write)], .load = gpu::LoadOp::DontCare, .cycle = true}};
        if (gpu::RenderPass pass = commands.beginRenderPass(std::span(targets).first(2 + extra))) {
            u.filter = glm::vec4(0.0f, 0.0f, 0.0f, params.clampGamma);
            const gpu::TextureSamplerBinding samplers[] = {{signal, point},
                                                           {m_history[std::size_t(m_index)], linear},
                                                           {m_length[std::size_t(m_index)], point},
                                                           {inputs.velocity, point},
                                                           {inputs.depth, point},
                                                           {inputs.previousDepth, point},
                                                           {m_deviation[std::size_t(m_index)], linear}};
            pass.bindPipeline(m_temporal);
            pass.bindSamplers(kFragment, 0, std::span(samplers).first(6 + extra));
            pass.pushUniforms(kFragment, 1, u);
            pass.draw(3);
            pass.end();
        }
        m_index = write;
        m_valid = true;
        source = m_history[std::size_t(write)];
    } else {
        m_valid = false;
    }
    const int steps = std::clamp(params.steps, 1, 3);
    for (int i = 0; i < steps; ++i) {
        const gpu::Texture& target = (steps - 1 - i) % 2 == 0 ? output : m_scratch;
        const gpu::ColorTarget color{.texture = target, .load = gpu::LoadOp::DontCare, .cycle = true};
        gpu::RenderPass pass = commands.beginRenderPass(color);
        if (!pass) continue;
        u.filter = glm::vec4(float(1 << i), params.planeTolerance, params.alphaScale ? 1.0f : 0.0f, 0.0f);
        const gpu::TextureSamplerBinding samplers[] = {{source, point}, {inputs.depth, point}};
        pass.bindPipeline(m_atrous);
        pass.bindSamplers(kFragment, 0, samplers);
        pass.pushUniforms(kFragment, 1, u);
        pass.draw(3);
        pass.end();
        source = target;
    }
}

} // namespace quench::render::detail
