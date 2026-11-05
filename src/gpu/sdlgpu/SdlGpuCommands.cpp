#include "gpu/sdlgpu/SdlGpu.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <SDL3/SDL_error.h>

#include <array>
#include <utility>

namespace quench::gpu {
namespace {

using detail::Access;

SDL_GPUCommandBuffer* nativeCommands(void* native) { return static_cast<SDL_GPUCommandBuffer*>(native); }

SDL_GPURenderPass* nativePass(void* native) { return static_cast<SDL_GPURenderPass*>(native); }

SDL_GPUCopyPass* nativeCopy(void* native) { return static_cast<SDL_GPUCopyPass*>(native); }

SDL_GPUTransferBuffer* nativeTransfer(const TransferBuffer& buffer) {
    return static_cast<SDL_GPUTransferBuffer*>(Access::native(buffer));
}

void pushUniformData(void* commands, ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size) {
    QUENCH_ASSERT(slot < 4, "uniform slots are 0..3");
    if (stage == ShaderStage::Vertex)
        SDL_PushGPUVertexUniformData(nativeCommands(commands), slot, data, size);
    else
        SDL_PushGPUFragmentUniformData(nativeCommands(commands), slot, data, size);
}

} // namespace

// ------------------------------------------------------------------ CommandBuffer

CommandBuffer Device::acquireCommandBuffer() {
    CommandBuffer commands;
    SDL_GPUCommandBuffer* native = SDL_AcquireGPUCommandBuffer(m_impl->device);
    if (!native) {
        log::error("gpu", "SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
        return commands;
    }
    commands.m_device = m_impl.get();
    commands.m_native = native;
    return commands;
}

CommandBuffer::~CommandBuffer() {
    if (!m_native) return;
    QUENCH_ASSERT(!m_passOpen, "command buffer destroyed with an open pass");
    if (m_swapchainAcquired) {
        // SDL cannot cancel a command buffer that acquired a swapchain texture; presenting is the lesser harm.
        QUENCH_ASSERT(false, "a command buffer that acquired a swapchain texture must be submitted");
        SDL_SubmitGPUCommandBuffer(nativeCommands(m_native));
    } else {
        SDL_CancelGPUCommandBuffer(nativeCommands(m_native));
    }
}

CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
    : m_device(std::exchange(other.m_device, nullptr)), m_native(std::exchange(other.m_native, nullptr)),
      m_passOpen(std::exchange(other.m_passOpen, false)),
      m_swapchainAcquired(std::exchange(other.m_swapchainAcquired, false)) {
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
    }
    return *this;
}

RenderPass CommandBuffer::beginRenderPass(std::span<const ColorTarget> colors, const DepthTarget* depth) {
    QUENCH_ASSERT(m_native, "beginRenderPass on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "a pass is already open");
    QUENCH_ASSERT(colors.size() <= 4, "at most 4 color targets");
    QUENCH_ASSERT(!colors.empty() || depth, "a render pass needs a target");
    RenderPass pass;
    std::array<SDL_GPUColorTargetInfo, 4> infos{};
    std::uint32_t samples = 0;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const ColorTarget& in = colors[i];
        QUENCH_ASSERT(in.texture, "color target without a texture");
        QUENCH_ASSERT(samples == 0 || samples == in.texture.desc.sampleCount, "targets differ in sample count");
        samples = in.texture.desc.sampleCount;
        SDL_GPUColorTargetInfo& info = infos[i];
        info.texture = sdl::texture(in.texture);
        info.mip_level = in.mipLevel;
        info.layer_or_depth_plane = in.layer;
        info.clear_color = sdl::toSdl(in.clear);
        info.load_op = sdl::toSdl(in.load);
        info.store_op = sdl::toSdl(in.store);
        info.resolve_texture = sdl::texture(in.resolveTexture);
        info.resolve_mip_level = in.resolveMipLevel;
        info.resolve_layer = in.resolveLayer;
        info.cycle = in.cycle;
        pass.m_targets.colorFormats[i] = in.texture.desc.format;
    }
    pass.m_targets.colorTargets = std::uint32_t(colors.size());
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    if (depth) {
        QUENCH_ASSERT(depth->texture, "depth target without a texture");
        QUENCH_ASSERT(depth->mipLevel < 256 && depth->layer < 256, "depth target level and layer are 0..255");
        QUENCH_ASSERT(samples == 0 || samples == depth->texture.desc.sampleCount, "targets differ in sample count");
        samples = depth->texture.desc.sampleCount;
        depthInfo.texture = sdl::texture(depth->texture);
        depthInfo.clear_depth = depth->clearDepth;
        depthInfo.load_op = sdl::toSdl(depth->load);
        depthInfo.store_op = sdl::toSdl(depth->store);
        depthInfo.stencil_load_op = sdl::toSdl(depth->stencilLoad);
        depthInfo.stencil_store_op = sdl::toSdl(depth->stencilStore);
        depthInfo.cycle = depth->cycle;
        depthInfo.clear_stencil = depth->clearStencil;
        depthInfo.mip_level = std::uint8_t(depth->mipLevel);
        depthInfo.layer = std::uint8_t(depth->layer);
        pass.m_targets.depthFormat = depth->texture.desc.format;
    }
    pass.m_targets.sampleCount = samples;
    pass.m_native = SDL_BeginGPURenderPass(nativeCommands(m_native), infos.data(), std::uint32_t(colors.size()),
                                           depth ? &depthInfo : nullptr);
    if (!pass.m_native) {
        log::error("gpu", "SDL_BeginGPURenderPass: {}", SDL_GetError());
        return pass;
    }
    pass.m_commands = this;
    m_passOpen = true;
    return pass;
}

CopyPass CommandBuffer::beginCopyPass() {
    QUENCH_ASSERT(m_native, "beginCopyPass on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "a pass is already open");
    CopyPass pass;
    pass.m_native = SDL_BeginGPUCopyPass(nativeCommands(m_native));
    if (!pass.m_native) {
        log::error("gpu", "SDL_BeginGPUCopyPass: {}", SDL_GetError());
        return pass;
    }
    pass.m_commands = this;
    m_passOpen = true;
    return pass;
}

void CommandBuffer::pushUniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size) {
    QUENCH_ASSERT(m_native, "pushUniforms on an empty command buffer");
    pushUniformData(m_native, stage, slot, data, size);
}

void CommandBuffer::generateMipmaps(const TextureRef& texture) {
    QUENCH_ASSERT(m_native && !m_passOpen, "generateMipmaps needs a command buffer outside passes");
    QUENCH_ASSERT(texture.desc.mipLevels > 1, "generateMipmaps on a texture with one level");
    QUENCH_ASSERT(any(texture.desc.usage & TextureUsage::Sampled) &&
                      any(texture.desc.usage & TextureUsage::ColorTarget),
                  "generateMipmaps needs Sampled and ColorTarget usage");
    SDL_GenerateMipmapsForGPUTexture(nativeCommands(m_native), sdl::texture(texture));
}

void CommandBuffer::blit(const BlitDesc& blit) {
    QUENCH_ASSERT(m_native && !m_passOpen, "blit needs a command buffer outside passes");
    const SDL_GPUTextureRegion src = sdl::toSdlRegion(blit.source);
    const SDL_GPUTextureRegion dst = sdl::toSdlRegion(blit.destination);
    SDL_GPUBlitInfo info{};
    info.source = {src.texture, src.mip_level, src.layer, src.x, src.y, src.w, src.h};
    info.destination = {dst.texture, dst.mip_level, dst.layer, dst.x, dst.y, dst.w, dst.h};
    info.load_op = sdl::toSdl(blit.load);
    info.clear_color = sdl::toSdl(blit.clear);
    info.flip_mode = SDL_FLIP_NONE;
    info.filter = sdl::toSdl(blit.filter);
    info.cycle = blit.cycle;
    SDL_BlitGPUTexture(nativeCommands(m_native), &info);
}

TextureRef CommandBuffer::acquireSwapchainTexture(SDL_Window* window) {
    QUENCH_ASSERT(m_native && !m_passOpen, "acquireSwapchainTexture needs a command buffer outside passes");
    SDL_GPUTexture* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(nativeCommands(m_native), window, &texture, &width, &height)) {
        log::error("gpu", "SDL_WaitAndAcquireGPUSwapchainTexture: {}", SDL_GetError());
        return {};
    }
    if (!texture) return {};
    m_swapchainAcquired = true;
    TextureRef ref;
    ref.native = texture;
    ref.desc.format = sdl::fromSdl(SDL_GetGPUSwapchainTextureFormat(m_device->device, window));
    ref.desc.usage = TextureUsage::ColorTarget;
    ref.desc.width = width;
    ref.desc.height = height;
    return ref;
}

void CommandBuffer::pushDebugGroup(const char* name) { SDL_PushGPUDebugGroup(nativeCommands(m_native), name); }

void CommandBuffer::popDebugGroup() { SDL_PopGPUDebugGroup(nativeCommands(m_native)); }

void CommandBuffer::insertDebugLabel(const char* text) { SDL_InsertGPUDebugLabel(nativeCommands(m_native), text); }

Result<void> CommandBuffer::submit() {
    QUENCH_ASSERT(m_native, "submit on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "submit with an open pass");
    SDL_GPUCommandBuffer* native = nativeCommands(std::exchange(m_native, nullptr));
    m_swapchainAcquired = false;
    if (!SDL_SubmitGPUCommandBuffer(native)) return Error{std::string("SDL_SubmitGPUCommandBuffer: ") + SDL_GetError()};
    return {};
}

Result<Fence> CommandBuffer::submitWithFence() {
    QUENCH_ASSERT(m_native, "submit on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "submit with an open pass");
    SDL_GPUCommandBuffer* native = nativeCommands(std::exchange(m_native, nullptr));
    m_swapchainAcquired = false;
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(native);
    if (!fence) return Error{std::string("SDL_SubmitGPUCommandBufferAndAcquireFence: ") + SDL_GetError()};
    Fence result;
    Access::adopt(result, m_device, fence);
    m_device->live.fetch_add(1, std::memory_order_relaxed);
    return result;
}

void CommandBuffer::cancel() {
    QUENCH_ASSERT(m_native, "cancel on an empty command buffer");
    QUENCH_ASSERT(!m_passOpen, "cancel with an open pass");
    QUENCH_ASSERT(!m_swapchainAcquired, "a command buffer that acquired a swapchain texture must be submitted");
    SDL_CancelGPUCommandBuffer(nativeCommands(std::exchange(m_native, nullptr)));
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
    SDL_EndGPURenderPass(nativePass(std::exchange(m_native, nullptr)));
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
    SDL_BindGPUGraphicsPipeline(nativePass(m_native), static_cast<SDL_GPUGraphicsPipeline*>(Access::native(pipeline)));
    m_pipelineBound = true;
}

void RenderPass::setViewport(const Viewport& viewport) {
    const SDL_GPUViewport v{viewport.x,      viewport.y,        viewport.width,
                            viewport.height, viewport.minDepth, viewport.maxDepth};
    SDL_SetGPUViewport(nativePass(m_native), &v);
}

void RenderPass::setScissor(const Rect& scissor) {
    const SDL_Rect r{scissor.x, scissor.y, scissor.width, scissor.height};
    SDL_SetGPUScissor(nativePass(m_native), &r);
}

void RenderPass::setBlendConstants(const Color& color) {
    SDL_SetGPUBlendConstants(nativePass(m_native), sdl::toSdl(color));
}

void RenderPass::bindVertexBuffers(std::uint32_t firstSlot, std::span<const BufferBinding> buffers) {
    QUENCH_ASSERT(buffers.size() <= 16, "at most 16 vertex buffers");
    std::array<SDL_GPUBufferBinding, 16> bindings{};
    for (std::size_t i = 0; i < buffers.size(); ++i) bindings[i] = {sdl::buffer(buffers[i].buffer), buffers[i].offset};
    SDL_BindGPUVertexBuffers(nativePass(m_native), firstSlot, bindings.data(), std::uint32_t(buffers.size()));
}

void RenderPass::bindIndexBuffer(const BufferBinding& buffer, IndexFormat format) {
    const SDL_GPUBufferBinding binding{sdl::buffer(buffer.buffer), buffer.offset};
    SDL_BindGPUIndexBuffer(nativePass(m_native), &binding,
                           format == IndexFormat::Uint16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                                         : SDL_GPU_INDEXELEMENTSIZE_32BIT);
}

void RenderPass::bindSamplers(ShaderStage stage, std::uint32_t firstSlot,
                              std::span<const TextureSamplerBinding> bindings) {
    QUENCH_ASSERT(bindings.size() <= 16, "at most 16 samplers per stage");
    std::array<SDL_GPUTextureSamplerBinding, 16> native{};
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        QUENCH_ASSERT(bindings[i].texture && bindings[i].sampler, "sampler binding without texture or sampler");
        native[i] = {sdl::texture(bindings[i].texture), sdl::sampler(bindings[i].sampler)};
    }
    if (stage == ShaderStage::Vertex)
        SDL_BindGPUVertexSamplers(nativePass(m_native), firstSlot, native.data(), std::uint32_t(bindings.size()));
    else
        SDL_BindGPUFragmentSamplers(nativePass(m_native), firstSlot, native.data(), std::uint32_t(bindings.size()));
}

void RenderPass::bindStorageBuffers(ShaderStage stage, std::uint32_t firstSlot, std::span<const BufferRef> buffers) {
    QUENCH_ASSERT(buffers.size() <= 8, "at most 8 storage buffers per stage");
    std::array<SDL_GPUBuffer*, 8> native{};
    for (std::size_t i = 0; i < buffers.size(); ++i) native[i] = sdl::buffer(buffers[i]);
    if (stage == ShaderStage::Vertex)
        SDL_BindGPUVertexStorageBuffers(nativePass(m_native), firstSlot, native.data(), std::uint32_t(buffers.size()));
    else
        SDL_BindGPUFragmentStorageBuffers(nativePass(m_native), firstSlot, native.data(),
                                          std::uint32_t(buffers.size()));
}

void RenderPass::bindStorageTextures(ShaderStage stage, std::uint32_t firstSlot, std::span<const TextureRef> textures) {
    QUENCH_ASSERT(textures.size() <= 8, "at most 8 storage textures per stage");
    std::array<SDL_GPUTexture*, 8> native{};
    for (std::size_t i = 0; i < textures.size(); ++i) native[i] = sdl::texture(textures[i]);
    if (stage == ShaderStage::Vertex)
        SDL_BindGPUVertexStorageTextures(nativePass(m_native), firstSlot, native.data(),
                                         std::uint32_t(textures.size()));
    else
        SDL_BindGPUFragmentStorageTextures(nativePass(m_native), firstSlot, native.data(),
                                           std::uint32_t(textures.size()));
}

void RenderPass::pushUniforms(ShaderStage stage, std::uint32_t slot, const void* data, std::uint32_t size) {
    QUENCH_ASSERT(m_native, "pushUniforms outside a render pass");
    pushUniformData(m_commands->m_native, stage, slot, data, size);
}

void RenderPass::draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                      std::uint32_t firstInstance) {
    QUENCH_ASSERT(m_pipelineBound, "draw without a pipeline");
    SDL_DrawGPUPrimitives(nativePass(m_native), vertexCount, instanceCount, firstVertex, firstInstance);
}

void RenderPass::drawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                             std::int32_t vertexOffset, std::uint32_t firstInstance) {
    QUENCH_ASSERT(m_pipelineBound, "draw without a pipeline");
    SDL_DrawGPUIndexedPrimitives(nativePass(m_native), indexCount, instanceCount, firstIndex, vertexOffset,
                                 firstInstance);
}

void RenderPass::drawIndirect(const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) {
    QUENCH_ASSERT(m_pipelineBound, "draw without a pipeline");
    QUENCH_ASSERT(m_commands->m_device->caps.indirectDraw, "indirect draws need Caps::indirectDraw");
    SDL_DrawGPUPrimitivesIndirect(nativePass(m_native), sdl::buffer(buffer), offset, drawCount);
}

void RenderPass::drawIndexedIndirect(const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) {
    QUENCH_ASSERT(m_pipelineBound, "draw without a pipeline");
    QUENCH_ASSERT(m_commands->m_device->caps.indirectDraw, "indirect draws need Caps::indirectDraw");
    SDL_DrawGPUIndexedPrimitivesIndirect(nativePass(m_native), sdl::buffer(buffer), offset, drawCount);
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
    SDL_EndGPUCopyPass(nativeCopy(std::exchange(m_native, nullptr)));
    m_commands->m_passOpen = false;
    m_commands = nullptr;
}

void CopyPass::upload(const TransferBuffer& source, std::uint32_t sourceOffset, const BufferRef& destination,
                      std::uint32_t destinationOffset, std::uint32_t size, bool cycle) {
    QUENCH_ASSERT(m_native, "upload outside a copy pass");
    QUENCH_ASSERT(source.desc().direction == TransferDirection::Upload, "upload from a download buffer");
    const SDL_GPUTransferBufferLocation from{nativeTransfer(source), sourceOffset};
    const SDL_GPUBufferRegion to{sdl::buffer(destination), destinationOffset, size};
    SDL_UploadToGPUBuffer(nativeCopy(m_native), &from, &to, cycle);
}

void CopyPass::upload(const TransferBuffer& source, std::uint32_t sourceOffset, const TextureRegion& destination,
                      bool cycle) {
    QUENCH_ASSERT(m_native, "upload outside a copy pass");
    QUENCH_ASSERT(source.desc().direction == TransferDirection::Upload, "upload from a download buffer");
    const SDL_GPUTextureTransferInfo from{nativeTransfer(source), sourceOffset, 0, 0};
    const SDL_GPUTextureRegion to = sdl::toSdlRegion(destination);
    SDL_UploadToGPUTexture(nativeCopy(m_native), &from, &to, cycle);
}

void CopyPass::download(const TextureRegion& source, const TransferBuffer& destination,
                        std::uint32_t destinationOffset) {
    QUENCH_ASSERT(m_native, "download outside a copy pass");
    QUENCH_ASSERT(destination.desc().direction == TransferDirection::Download, "download into an upload buffer");
    const SDL_GPUTextureRegion from = sdl::toSdlRegion(source);
    const SDL_GPUTextureTransferInfo to{nativeTransfer(destination), destinationOffset, 0, 0};
    SDL_DownloadFromGPUTexture(nativeCopy(m_native), &from, &to);
}

void CopyPass::download(const BufferRef& source, std::uint32_t sourceOffset, std::uint32_t size,
                        const TransferBuffer& destination, std::uint32_t destinationOffset) {
    QUENCH_ASSERT(m_native, "download outside a copy pass");
    QUENCH_ASSERT(destination.desc().direction == TransferDirection::Download, "download into an upload buffer");
    const SDL_GPUBufferRegion from{sdl::buffer(source), sourceOffset, size};
    const SDL_GPUTransferBufferLocation to{nativeTransfer(destination), destinationOffset};
    SDL_DownloadFromGPUBuffer(nativeCopy(m_native), &from, &to);
}

void CopyPass::copy(const BufferRef& source, std::uint32_t sourceOffset, const BufferRef& destination,
                    std::uint32_t destinationOffset, std::uint32_t size, bool cycle) {
    QUENCH_ASSERT(m_native, "copy outside a copy pass");
    const SDL_GPUBufferLocation from{sdl::buffer(source), sourceOffset};
    const SDL_GPUBufferLocation to{sdl::buffer(destination), destinationOffset};
    SDL_CopyGPUBufferToBuffer(nativeCopy(m_native), &from, &to, size, cycle);
}

void CopyPass::copy(const TextureRegion& source, const TextureRegion& destination, bool cycle) {
    QUENCH_ASSERT(m_native, "copy outside a copy pass");
    const SDL_GPUTextureRegion region = sdl::toSdlRegion(source);
    const SDL_GPUTextureLocation from{region.texture, region.mip_level, region.layer, region.x, region.y, region.z};
    const SDL_GPUTextureLocation to{sdl::texture(destination.texture),
                                    destination.mipLevel,
                                    destination.layer,
                                    destination.x,
                                    destination.y,
                                    destination.z};
    SDL_CopyGPUTextureToTexture(nativeCopy(m_native), &from, &to, region.w, region.h, region.d, cycle);
}

} // namespace quench::gpu
