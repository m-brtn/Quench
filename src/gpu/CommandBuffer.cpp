// Command recording: argument checks and pass bookkeeping; the commands are the backend's (src/gpu/Backend.h).

#include "gpu/Backend.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>
#include <quench/core/Profile.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

namespace quench::gpu {

using detail::Access;

namespace {

#if QUENCH_TRACY
// The CPU side of the timing ranges: a zone "pass.<name>" per range while it is recorded, on any backend. The ranges
// of the command buffers a thread records, innermost last.
struct PassZone {
    void* commands;
    profile::Zone zone;
};
thread_local std::vector<PassZone> g_passZones;
#endif

void beginPassZone([[maybe_unused]] void* commands, [[maybe_unused]] const char* name) {
#if QUENCH_TRACY
    profile::Zone zone;
    if (profile::connected()) {
        char buffer[64];
        const int length = std::snprintf(buffer, sizeof(buffer), "pass.%s", name);
        zone = profile::beginZone({buffer, std::size_t(std::clamp(length, 0, int(sizeof(buffer)) - 1))});
    }
    g_passZones.push_back({commands, zone});
#endif
}

// The innermost range of `commands`, or all of them (a command buffer submitted or cancelled inside its ranges).
void endPassZones([[maybe_unused]] void* commands, [[maybe_unused]] bool all) {
#if QUENCH_TRACY
    while (!g_passZones.empty() && g_passZones.back().commands == commands) {
        profile::endZone(g_passZones.back().zone);
        g_passZones.pop_back();
        if (!all) break;
    }
#endif
}

} // namespace

// ------------------------------------------------------------------ CommandBuffer

CommandBuffer Device::acquireCommandBuffer() {
    CommandBuffer commands;
    void* native = m_impl->acquireCommandBuffer();
    if (!native) return commands;
    commands.m_device = m_impl.get();
    commands.m_native = native;
    commands.m_timed = m_impl->caps.gpuPassTiming && m_impl->passTiming;
    return commands;
}

CommandBuffer::~CommandBuffer() {
    if (!m_native) return;
    QUENCH_ASSERT(!m_passOpen, "command buffer destroyed with an open pass");
    endPassZones(m_native, true);
    if (m_swapchainAcquired) {
        // A command buffer that acquired a swapchain texture cannot be cancelled (SDL); presenting is the lesser harm.
        QUENCH_ASSERT(false, "a command buffer that acquired a swapchain texture must be submitted");
        if (Result<void> submitted = m_device->submit(m_native); !submitted) log::error("gpu", "{}", submitted.error());
    } else {
        m_device->cancel(m_native);
    }
}

CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_native(std::exchange(other.m_native, nullptr)),
      m_passOpen(std::exchange(other.m_passOpen, false)),
      m_swapchainAcquired(std::exchange(other.m_swapchainAcquired, false)),
      m_timed(std::exchange(other.m_timed, false)) {
    QUENCH_ASSERT(!m_passOpen, "a command buffer with an open pass must not move");
}

CommandBuffer& CommandBuffer::operator=(CommandBuffer&& other) noexcept {
    if (this != &other) {
        QUENCH_ASSERT(!other.m_passOpen, "a command buffer with an open pass must not move");
        this->~CommandBuffer();
        m_device = std::exchange(other.m_device, nullptr);
        m_native = std::exchange(other.m_native, nullptr);
        m_passOpen = std::exchange(other.m_passOpen, false);
        m_swapchainAcquired = std::exchange(other.m_swapchainAcquired, false);
        m_timed = std::exchange(other.m_timed, false);
    }
    return *this;
}

RenderPass CommandBuffer::beginRenderPass(std::span<const ColorTarget> colors, const DepthTarget* depth) {
    QUENCH_ASSERT(m_native, "beginRenderPass on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "a pass is already open");
    QUENCH_ASSERT(colors.size() <= 4, "at most 4 color targets");
    QUENCH_ASSERT(!colors.empty() || depth, "a render pass needs a target");
    RenderPass pass;
    std::uint32_t samples = 0;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const ColorTarget& in = colors[i];
        QUENCH_ASSERT(in.texture, "color target without a texture");
        QUENCH_ASSERT(samples == 0 || samples == in.texture.desc.sampleCount, "targets differ in sample count");
        samples = in.texture.desc.sampleCount;
        pass.m_targets.colorFormats[i] = in.texture.desc.format;
    }
    pass.m_targets.colorTargets = std::uint32_t(colors.size());
    if (depth) {
        QUENCH_ASSERT(depth->texture, "depth target without a texture");
        QUENCH_ASSERT(depth->mipLevel < 256 && depth->layer < 256, "depth target level and layer are 0..255");
        QUENCH_ASSERT(samples == 0 || samples == depth->texture.desc.sampleCount, "targets differ in sample count");
        samples = depth->texture.desc.sampleCount;
        pass.m_targets.depthFormat = depth->texture.desc.format;
    }
    pass.m_targets.sampleCount = samples;
    pass.m_native = m_device->beginRenderPass(m_native, colors, depth);
    if (!pass.m_native) return pass;
    pass.m_commands = this;
    m_passOpen = true;
    return pass;
}

CopyPass CommandBuffer::beginCopyPass() {
    QUENCH_ASSERT(m_native, "beginCopyPass on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "a pass is already open");
    CopyPass pass;
    pass.m_native = m_device->beginCopyPass(m_native);
    if (!pass.m_native) return pass;
    pass.m_commands = this;
    m_passOpen = true;
    return pass;
}

ComputePass CommandBuffer::beginComputePass(std::span<const StorageBufferWrite> writes) {
    QUENCH_ASSERT(m_native, "beginComputePass on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "a pass is already open");
    QUENCH_ASSERT(writes.size() <= 8, "at most 8 read-write storage buffers");
    for ([[maybe_unused]] const StorageBufferWrite& write : writes)
        QUENCH_ASSERT(any(write.buffer.desc.usage & BufferUsage::ComputeStorageWrite),
                      "a compute pass writes only ComputeStorageWrite buffers");
    ComputePass pass;
    pass.m_native = m_device->beginComputePass(m_native, writes);
    if (!pass.m_native) return pass;
    pass.m_commands = this;
    m_passOpen = true;
    return pass;
}

AccelerationStructurePass CommandBuffer::beginAccelerationStructurePass() {
    QUENCH_ASSERT(m_native, "beginAccelerationStructurePass on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "a pass is already open");
    QUENCH_ASSERT(m_device->caps.rayTracing, "acceleration structures need Caps::rayTracing");
    AccelerationStructurePass pass;
    if (!m_device->caps.rayTracing) return pass;
    pass.m_native = m_device->beginAccelerationStructurePass(m_native);
    if (!pass.m_native) return pass;
    pass.m_commands = this;
    m_passOpen = true;
    return pass;
}

void CommandBuffer::upscale(const Upscaler& upscaler, const UpscaleDesc& desc) {
    QUENCH_ASSERT(m_native && !m_passOpen, "upscale needs a command buffer outside passes");
    QUENCH_ASSERT(upscaler, "upscale with an empty upscaler");
    [[maybe_unused]] const UpscalerDesc& u = upscaler.desc();
    QUENCH_ASSERT(desc.color && desc.output, "upscale without a color input or an output");
    QUENCH_ASSERT(desc.color.desc.width == u.inputWidth && desc.color.desc.height == u.inputHeight &&
                      desc.output.desc.width == u.outputWidth && desc.output.desc.height == u.outputHeight,
                  "upscale textures differ in size from the upscaler");
    QUENCH_ASSERT(any(desc.output.desc.usage & TextureUsage::ShaderWrite) &&
                      any(desc.output.desc.usage & TextureUsage::Sampled),
                  "the upscale output needs ShaderWrite and Sampled");
    QUENCH_ASSERT(u.type == UpscalerType::Spatial || (desc.depth && desc.motion),
                  "temporal upscaling needs the depth and the motion");
    QUENCH_ASSERT(u.type != UpscalerType::TemporalDenoised ||
                      (desc.diffuseAlbedo && desc.specularAlbedo && desc.normal && desc.roughness),
                  "temporal denoised upscaling needs the albedos, the normal and the roughness");
    QUENCH_ASSERT(desc.exposure > 0.0f, "upscale exposure not above 0");
    m_device->upscale(m_native, Access::native(upscaler), desc);
}

void CommandBuffer::pushUniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size) {
    QUENCH_ASSERT(m_native, "pushUniforms on an empty command buffer");
    QUENCH_ASSERT(slot < 4, "uniform slots are 0..3");
    m_device->pushUniforms(m_native, stage, slot, data, size);
}

void CommandBuffer::generateMipmaps(const TextureRef& texture) {
    QUENCH_ASSERT(m_native && !m_passOpen, "generateMipmaps needs a command buffer outside passes");
    QUENCH_ASSERT(texture.desc.mipLevels > 1, "generateMipmaps on a texture with one level");
    QUENCH_ASSERT(any(texture.desc.usage & TextureUsage::Sampled) &&
                      any(texture.desc.usage & TextureUsage::ColorTarget),
                  "generateMipmaps needs Sampled and ColorTarget usage");
    m_device->generateMipmaps(m_native, texture);
}

void CommandBuffer::blit(const BlitDesc& blit) {
    QUENCH_ASSERT(m_native && !m_passOpen, "blit needs a command buffer outside passes");
    QUENCH_ASSERT(blit.source.texture && blit.destination.texture, "blit without a source or destination");
    m_device->blit(m_native, blit);
}

TextureRef CommandBuffer::acquireSwapchainTexture(SDL_Window* window) {
    QUENCH_ASSERT(m_native && !m_passOpen, "acquireSwapchainTexture needs a command buffer outside passes");
    TextureRef ref = m_device->acquireSwapchainTexture(m_native, window);
    if (ref) m_swapchainAcquired = true;
    return ref;
}

void CommandBuffer::pushDebugGroup(const char* name) { m_device->pushDebugGroup(m_native, name); }

void CommandBuffer::popDebugGroup() { m_device->popDebugGroup(m_native); }

void CommandBuffer::insertDebugLabel(const char* text) { m_device->insertDebugLabel(m_native, text); }

void CommandBuffer::beginTiming(const char* name) {
    QUENCH_ASSERT(m_native && !m_passOpen, "beginTiming needs a command buffer outside passes");
    beginPassZone(m_native, name);
    if (m_timed) m_device->beginTiming(m_native, name);
}

void CommandBuffer::endTiming() {
    QUENCH_ASSERT(m_native && !m_passOpen, "endTiming needs a command buffer outside passes");
    endPassZones(m_native, false);
    if (m_timed) m_device->endTiming(m_native);
}

Result<void> CommandBuffer::submit() {
    QUENCH_ASSERT(m_native, "submit on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "submit with an open pass");
    void* native = std::exchange(m_native, nullptr);
    endPassZones(native, true);
    m_swapchainAcquired = false;
    return m_device->submit(native);
}

Result<Fence> CommandBuffer::submitWithFence() {
    QUENCH_ASSERT(m_native, "submit on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "submit with an open pass");
    void* native = std::exchange(m_native, nullptr);
    endPassZones(native, true);
    m_swapchainAcquired = false;
    Result<void*> fence = m_device->submitWithFence(native);
    if (!fence) return Error{fence.error()};
    Fence result;
    Access::adopt(result, m_device, *fence);
    m_device->live.fetch_add(1, std::memory_order_relaxed);
    return result;
}

void CommandBuffer::cancel() {
    QUENCH_ASSERT(m_native, "cancel on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "cancel with an open pass");
    QUENCH_ASSERT(!m_swapchainAcquired, "a command buffer that acquired a swapchain texture must be submitted");
    endPassZones(m_native, true);
    m_device->cancel(std::exchange(m_native, nullptr));
}

// ------------------------------------------------------------------ RenderPass

RenderPass::RenderPass(RenderPass&& other) noexcept
    : m_commands(std::exchange(other.m_commands, nullptr)), m_native(std::exchange(other.m_native, nullptr)),
      m_targets(other.m_targets), m_pipelineBound(other.m_pipelineBound) {}

RenderPass& RenderPass::operator=(RenderPass&& other) noexcept {
    if (this != &other) {
        end();
        m_commands = std::exchange(other.m_commands, nullptr);
        m_native = std::exchange(other.m_native, nullptr);
        m_targets = other.m_targets;
        m_pipelineBound = other.m_pipelineBound;
    }
    return *this;
}

void RenderPass::end() {
    if (!m_native) return;
    m_commands->m_device->endRenderPass(m_commands->m_native, std::exchange(m_native, nullptr));
    m_commands->m_passOpen = false;
    m_commands = nullptr;
}

void RenderPass::bindPipeline(const GraphicsPipeline& pipeline) {
    QUENCH_ASSERT(m_native, "bindPipeline outside a render pass");
    QUENCH_ASSERT(pipeline, "bindPipeline with an empty pipeline");
    [[maybe_unused]] const detail::PipelineInfo& p = Access::info(pipeline);
    QUENCH_ASSERT(p.colorTargets == m_targets.colorTargets && p.colorFormats == m_targets.colorFormats,
                  "pipeline color formats differ from the pass targets");
    QUENCH_ASSERT(p.depthFormat == m_targets.depthFormat, "pipeline depth format differs from the pass target");
    QUENCH_ASSERT(p.sampleCount == m_targets.sampleCount, "pipeline sample count differs from the pass targets");
    m_commands->m_device->bindPipeline(m_native, static_cast<detail::PipelineSlot*>(Access::native(pipeline))->native);
    m_pipelineBound = true;
}

void RenderPass::setViewport(const Viewport& viewport) { m_commands->m_device->setViewport(m_native, viewport); }

void RenderPass::setScissor(const Rect& scissor) { m_commands->m_device->setScissor(m_native, scissor); }

void RenderPass::setBlendConstants(const Color& color) { m_commands->m_device->setBlendConstants(m_native, color); }

void RenderPass::bindVertexBuffers(std::uint32_t firstSlot, std::span<const BufferBinding> buffers) {
    QUENCH_ASSERT(buffers.size() <= 16, "at most 16 vertex buffers");
    m_commands->m_device->bindVertexBuffers(m_native, firstSlot, buffers);
}

void RenderPass::bindIndexBuffer(const BufferBinding& buffer, IndexFormat format) {
    m_commands->m_device->bindIndexBuffer(m_native, buffer, format);
}

void RenderPass::bindSamplers(ShaderStage stage, std::uint32_t firstSlot,
                              std::span<const TextureSamplerBinding> bindings) {
    QUENCH_ASSERT(bindings.size() <= 16, "at most 16 samplers per stage");
    for ([[maybe_unused]] const TextureSamplerBinding& binding : bindings)
        QUENCH_ASSERT(binding.texture && binding.sampler, "sampler binding without texture or sampler");
    m_commands->m_device->bindSamplers(m_native, stage, firstSlot, bindings);
}

void RenderPass::bindStorageBuffers(ShaderStage stage, std::uint32_t firstSlot, std::span<const BufferRef> buffers) {
    QUENCH_ASSERT(buffers.size() <= 8, "at most 8 storage buffers per stage");
    m_commands->m_device->bindStorageBuffers(m_native, stage, firstSlot, buffers);
}

void RenderPass::bindStorageTextures(ShaderStage stage, std::uint32_t firstSlot, std::span<const TextureRef> textures) {
    QUENCH_ASSERT(textures.size() <= 8, "at most 8 storage textures per stage");
    m_commands->m_device->bindStorageTextures(m_native, stage, firstSlot, textures);
}

void RenderPass::bindAccelerationStructure(ShaderStage stage, std::uint32_t slot,
                                           const AccelerationStructure& topLevel) {
    QUENCH_ASSERT(m_native, "bindAccelerationStructure outside a render pass");
    QUENCH_ASSERT(stage != ShaderStage::Compute, "a render pass binds vertex or fragment resources");
    QUENCH_ASSERT(slot < 2, "acceleration structure slots are 0..1");
    QUENCH_ASSERT(topLevel && topLevel.desc().level == AccelerationStructureLevel::Top,
                  "shaders trace rays against a top level");
    m_commands->m_device->bindAccelerationStructure(m_native, stage, slot, Access::native(topLevel));
}

void RenderPass::pushUniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size) {
    QUENCH_ASSERT(m_native, "pushUniforms outside a render pass");
    QUENCH_ASSERT(slot < 4, "uniform slots are 0..3");
    m_commands->m_device->pushUniforms(m_commands->m_native, stage, slot, data, size);
}

void RenderPass::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                      std::uint32_t firstInstance) {
    QUENCH_ASSERT(m_pipelineBound, "draw without a pipeline");
    m_commands->m_device->draw(m_native, vertexCount, instanceCount, firstVertex, firstInstance);
}

void RenderPass::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                             std::int32_t vertexOffset, std::uint32_t firstInstance) {
    QUENCH_ASSERT(m_pipelineBound, "draw without a pipeline");
    m_commands->m_device->drawIndexed(m_native, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void RenderPass::drawIndirect(const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) {
    QUENCH_ASSERT(m_pipelineBound, "draw without a pipeline");
    QUENCH_ASSERT(m_commands->m_device->caps.indirectDraw, "indirect draws need Caps::indirectDraw");
    m_commands->m_device->drawIndirect(m_native, buffer, offset, drawCount);
}

void RenderPass::drawIndexedIndirect(const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) {
    QUENCH_ASSERT(m_pipelineBound, "draw without a pipeline");
    QUENCH_ASSERT(m_commands->m_device->caps.indirectDraw, "indirect draws need Caps::indirectDraw");
    m_commands->m_device->drawIndexedIndirect(m_native, buffer, offset, drawCount);
}

// ------------------------------------------------------------------ ComputePass

ComputePass::ComputePass(ComputePass&& other) noexcept
    : m_commands(std::exchange(other.m_commands, nullptr)), m_native(std::exchange(other.m_native, nullptr)),
      m_pipelineBound(other.m_pipelineBound) {}

ComputePass& ComputePass::operator=(ComputePass&& other) noexcept {
    if (this != &other) {
        end();
        m_commands = std::exchange(other.m_commands, nullptr);
        m_native = std::exchange(other.m_native, nullptr);
        m_pipelineBound = other.m_pipelineBound;
    }
    return *this;
}

void ComputePass::end() {
    if (!m_native) return;
    m_commands->m_device->endComputePass(m_commands->m_native, std::exchange(m_native, nullptr));
    m_commands->m_passOpen = false;
    m_commands = nullptr;
}

void ComputePass::bindPipeline(const ComputePipeline& pipeline) {
    QUENCH_ASSERT(m_native, "bindPipeline outside a compute pass");
    QUENCH_ASSERT(pipeline, "bindPipeline with an empty pipeline");
    m_commands->m_device->bindComputePipeline(m_native,
                                              static_cast<detail::PipelineSlot*>(Access::native(pipeline))->native);
    m_pipelineBound = true;
}

void ComputePass::bindStorageBuffers(std::uint32_t firstSlot, std::span<const BufferRef> buffers) {
    QUENCH_ASSERT(m_native, "bindStorageBuffers outside a compute pass");
    QUENCH_ASSERT(buffers.size() <= 8, "at most 8 storage buffers");
    m_commands->m_device->bindComputeStorageBuffers(m_native, firstSlot, buffers);
}

void ComputePass::bindAccelerationStructure(std::uint32_t slot, const AccelerationStructure& topLevel) {
    QUENCH_ASSERT(m_native, "bindAccelerationStructure outside a compute pass");
    QUENCH_ASSERT(slot < 2, "acceleration structure slots are 0..1");
    QUENCH_ASSERT(topLevel && topLevel.desc().level == AccelerationStructureLevel::Top,
                  "shaders trace rays against a top level");
    m_commands->m_device->bindAccelerationStructure(m_native, ShaderStage::Compute, slot, Access::native(topLevel));
}

void ComputePass::pushUniforms(std::uint32_t slot, const void* data, std::uint32_t size) {
    QUENCH_ASSERT(m_native, "pushUniforms outside a compute pass");
    QUENCH_ASSERT(slot < 4, "uniform slots are 0..3");
    m_commands->m_device->pushUniforms(m_commands->m_native, ShaderStage::Compute, slot, data, size);
}

void ComputePass::dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) {
    QUENCH_ASSERT(m_pipelineBound, "dispatch without a pipeline");
    m_commands->m_device->dispatch(m_native, groupsX, groupsY, groupsZ);
}

// ------------------------------------------------------------------ CopyPass

CopyPass::CopyPass(CopyPass&& other) noexcept
    : m_commands(std::exchange(other.m_commands, nullptr)), m_native(std::exchange(other.m_native, nullptr)) {}

CopyPass& CopyPass::operator=(CopyPass&& other) noexcept {
    if (this != &other) {
        end();
        m_commands = std::exchange(other.m_commands, nullptr);
        m_native = std::exchange(other.m_native, nullptr);
    }
    return *this;
}

void CopyPass::end() {
    if (!m_native) return;
    m_commands->m_device->endCopyPass(m_commands->m_native, std::exchange(m_native, nullptr));
    m_commands->m_passOpen = false;
    m_commands = nullptr;
}

void CopyPass::upload(const TransferBuffer& source, std::uint32_t sourceOffset, const BufferRef& destination,
                      std::uint32_t destinationOffset, std::uint32_t size, bool cycle) {
    QUENCH_ASSERT(m_native, "upload outside a copy pass");
    QUENCH_ASSERT(source.desc().direction == TransferDirection::Upload, "upload from a download buffer");
    m_commands->m_device->uploadToBuffer(m_native, Access::native(source), sourceOffset, destination, destinationOffset,
                                         size, cycle);
}

void CopyPass::upload(const TransferBuffer& source, std::uint32_t sourceOffset, const TextureRegion& destination,
                      bool cycle) {
    QUENCH_ASSERT(m_native, "upload outside a copy pass");
    QUENCH_ASSERT(source.desc().direction == TransferDirection::Upload, "upload from a download buffer");
    m_commands->m_device->uploadToTexture(m_native, Access::native(source), sourceOffset, destination, cycle);
}

void CopyPass::download(const TextureRegion& source, const TransferBuffer& destination,
                        std::uint32_t destinationOffset) {
    QUENCH_ASSERT(m_native, "download outside a copy pass");
    QUENCH_ASSERT(destination.desc().direction == TransferDirection::Download, "download into an upload buffer");
    m_commands->m_device->downloadFromTexture(m_native, source, Access::native(destination), destinationOffset);
}

void CopyPass::download(const BufferRef& source, std::uint32_t sourceOffset, std::uint32_t size,
                        const TransferBuffer& destination, std::uint32_t destinationOffset) {
    QUENCH_ASSERT(m_native, "download outside a copy pass");
    QUENCH_ASSERT(destination.desc().direction == TransferDirection::Download, "download into an upload buffer");
    m_commands->m_device->downloadFromBuffer(m_native, source, sourceOffset, size, Access::native(destination),
                                             destinationOffset);
}

void CopyPass::copy(const BufferRef& source, std::uint32_t sourceOffset, const BufferRef& destination,
                    std::uint32_t destinationOffset, std::uint32_t size, bool cycle) {
    QUENCH_ASSERT(m_native, "copy outside a copy pass");
    m_commands->m_device->copyBuffer(m_native, source, sourceOffset, destination, destinationOffset, size, cycle);
}

void CopyPass::copy(const TextureRegion& source, const TextureRegion& destination, bool cycle) {
    QUENCH_ASSERT(m_native, "copy outside a copy pass");
    m_commands->m_device->copyTexture(m_native, source, destination, cycle);
}

// ------------------------------------------------------------------ AccelerationStructurePass

AccelerationStructurePass::AccelerationStructurePass(AccelerationStructurePass&& other) noexcept
    : m_commands(std::exchange(other.m_commands, nullptr)), m_native(std::exchange(other.m_native, nullptr)) {}

AccelerationStructurePass& AccelerationStructurePass::operator=(AccelerationStructurePass&& other) noexcept {
    if (this != &other) {
        end();
        m_commands = std::exchange(other.m_commands, nullptr);
        m_native = std::exchange(other.m_native, nullptr);
    }
    return *this;
}

void AccelerationStructurePass::end() {
    if (!m_native) return;
    m_commands->m_device->endAccelerationStructurePass(m_commands->m_native, std::exchange(m_native, nullptr));
    m_commands->m_passOpen = false;
    m_commands = nullptr;
}

namespace {

[[maybe_unused]] bool fits(const AccelerationStructureDesc& desc, const RayGeometry& geometry) {
    return geometry.vertices && geometry.indices && geometry.vertexCount <= desc.vertexCount &&
           geometry.triangleCount <= desc.triangleCount && geometry.vertexStride >= 12 && geometry.indexOffset % 4 == 0;
}

[[maybe_unused]] bool validInstances(std::span<const RayInstance> instances) {
    for (const RayInstance& instance : instances)
        if (!instance.bottomLevel || !*instance.bottomLevel ||
            instance.bottomLevel->desc().level != AccelerationStructureLevel::Bottom || instance.id >= (1u << 24))
            return false;
    return true;
}

} // namespace

void AccelerationStructurePass::build(const AccelerationStructure& bottomLevel, const RayGeometry& geometry) {
    QUENCH_ASSERT(m_native, "build outside an acceleration structure pass");
    QUENCH_ASSERT(bottomLevel && bottomLevel.desc().level == AccelerationStructureLevel::Bottom,
                  "build(geometry) needs a bottom level");
    QUENCH_ASSERT(bottomLevel.desc().compactedSize == 0, "a compacted bottom level is filled by copyAndCompact");
    QUENCH_ASSERT(fits(bottomLevel.desc(), geometry), "geometry larger than the bottom level or incomplete");
    m_commands->m_device->buildBottomLevel(m_native, Access::native(bottomLevel), geometry, false);
}

void AccelerationStructurePass::refit(const AccelerationStructure& bottomLevel, const RayGeometry& geometry) {
    QUENCH_ASSERT(m_native, "refit outside an acceleration structure pass");
    QUENCH_ASSERT(bottomLevel && bottomLevel.desc().level == AccelerationStructureLevel::Bottom &&
                      bottomLevel.desc().allowRefit,
                  "refit needs a bottom level created with allowRefit");
    QUENCH_ASSERT(fits(bottomLevel.desc(), geometry), "geometry larger than the bottom level or incomplete");
    m_commands->m_device->buildBottomLevel(m_native, Access::native(bottomLevel), geometry, true);
}

void AccelerationStructurePass::build(const AccelerationStructure& topLevel, std::span<const RayInstance> instances) {
    QUENCH_ASSERT(m_native, "build outside an acceleration structure pass");
    QUENCH_ASSERT(topLevel && topLevel.desc().level == AccelerationStructureLevel::Top,
                  "build(instances) needs a top level");
    QUENCH_ASSERT(instances.size() <= topLevel.desc().instanceCapacity, "more instances than the top level holds");
    QUENCH_ASSERT(validInstances(instances), "a top-level instance needs a bottom level and a 24-bit id");
    m_commands->m_device->buildTopLevel(m_native, Access::native(topLevel), instances, false);
}

void AccelerationStructurePass::refit(const AccelerationStructure& topLevel, std::span<const RayInstance> instances) {
    QUENCH_ASSERT(m_native, "refit outside an acceleration structure pass");
    QUENCH_ASSERT(topLevel && topLevel.desc().level == AccelerationStructureLevel::Top,
                  "refit(instances) needs a top level");
    QUENCH_ASSERT(instances.size() <= topLevel.desc().instanceCapacity, "more instances than the top level holds");
    QUENCH_ASSERT(validInstances(instances), "a top-level instance needs a bottom level and a 24-bit id");
    m_commands->m_device->buildTopLevel(m_native, Access::native(topLevel), instances, topLevel.desc().allowRefit);
}

void AccelerationStructurePass::queryCompactedSize(const AccelerationStructure& bottomLevel) {
    QUENCH_ASSERT(m_native, "queryCompactedSize outside an acceleration structure pass");
    QUENCH_ASSERT(bottomLevel && bottomLevel.desc().level == AccelerationStructureLevel::Bottom,
                  "queryCompactedSize needs a bottom level");
    m_commands->m_device->queryCompactedSize(m_native, Access::native(bottomLevel));
}

void AccelerationStructurePass::copyAndCompact(const AccelerationStructure& source,
                                               const AccelerationStructure& destination) {
    QUENCH_ASSERT(m_native, "copyAndCompact outside an acceleration structure pass");
    QUENCH_ASSERT(source && source.desc().level == AccelerationStructureLevel::Bottom && destination &&
                      destination.desc().level == AccelerationStructureLevel::Bottom,
                  "copyAndCompact copies bottom levels");
    QUENCH_ASSERT(destination.desc().compactedSize > 0,
                  "copyAndCompact into a bottom level created with AccelerationStructureDesc::compactedSize");
    m_commands->m_device->copyAndCompact(m_native, Access::native(source), Access::native(destination));
}

} // namespace quench::gpu
