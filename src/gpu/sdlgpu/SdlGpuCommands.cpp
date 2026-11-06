#include "gpu/sdlgpu/SdlGpu.h"

#include <quench/core/Log.h>

#include <SDL3/SDL_error.h>

#include <algorithm>
#include <array>
#include <string>

namespace quench::gpu::detail {
namespace {

SDL_GPUCommandBuffer* nativeCommands(void* native) { return static_cast<SDL_GPUCommandBuffer*>(native); }

SDL_GPURenderPass* nativePass(void* native) { return static_cast<SDL_GPURenderPass*>(native); }

SDL_GPUCopyPass* nativeCopy(void* native) { return static_cast<SDL_GPUCopyPass*>(native); }

SDL_GPUComputePass* nativeCompute(void* native) { return static_cast<SDL_GPUComputePass*>(native); }

SDL_GPUTransferBuffer* nativeTransfer(void* native) { return static_cast<SDL_GPUTransferBuffer*>(native); }

} // namespace

// ------------------------------------------------------------------ command buffers

void* SdlGpuDevice::acquireCommandBuffer() {
    SDL_GPUCommandBuffer* native = SDL_AcquireGPUCommandBuffer(m_device);
    if (!native) log::error("gpu", "SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
    return native;
}

void* SdlGpuDevice::beginRenderPass(void* commands, std::span<const ColorTarget> colors, const DepthTarget* depth) {
    std::array<SDL_GPUColorTargetInfo, 4> infos{};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const ColorTarget& in = colors[i];
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
        info.cycle_resolve_texture = in.cycleResolve;
    }
    SDL_GPUDepthStencilTargetInfo depthInfo{};
    if (depth) {
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
    }
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(nativeCommands(commands), infos.data(),
                                                     std::uint32_t(colors.size()), depth ? &depthInfo : nullptr);
    if (!pass) log::error("gpu", "SDL_BeginGPURenderPass: {}", SDL_GetError());
    return pass;
}

void* SdlGpuDevice::beginCopyPass(void* commands) {
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(nativeCommands(commands));
    if (!pass) log::error("gpu", "SDL_BeginGPUCopyPass: {}", SDL_GetError());
    return pass;
}

void* SdlGpuDevice::beginComputePass(void* commands, std::span<const StorageBufferWrite> writes) {
    std::array<SDL_GPUStorageBufferReadWriteBinding, 8> bindings{};
    for (std::size_t i = 0; i < writes.size(); ++i) {
        bindings[i].buffer = sdl::buffer(writes[i].buffer);
        bindings[i].cycle = writes[i].cycle;
    }
    SDL_GPUComputePass* pass =
        SDL_BeginGPUComputePass(nativeCommands(commands), nullptr, 0, bindings.data(), std::uint32_t(writes.size()));
    if (!pass) log::error("gpu", "SDL_BeginGPUComputePass: {}", SDL_GetError());
    return pass;
}

void SdlGpuDevice::endRenderPass(void*, void* pass) { SDL_EndGPURenderPass(nativePass(pass)); }

void SdlGpuDevice::endCopyPass(void*, void* pass) { SDL_EndGPUCopyPass(nativeCopy(pass)); }

void SdlGpuDevice::endComputePass(void*, void* pass) { SDL_EndGPUComputePass(nativeCompute(pass)); }

void SdlGpuDevice::pushUniforms(void* commands, ShaderStage stage, std::uint32_t slot, const void* data,
                                std::uint32_t size) {
    if (stage == ShaderStage::Vertex)
        SDL_PushGPUVertexUniformData(nativeCommands(commands), slot, data, size);
    else if (stage == ShaderStage::Fragment)
        SDL_PushGPUFragmentUniformData(nativeCommands(commands), slot, data, size);
    else
        SDL_PushGPUComputeUniformData(nativeCommands(commands), slot, data, size);
}

void SdlGpuDevice::generateMipmaps(void* commands, const TextureRef& texture) {
    const TextureDesc& desc = texture.desc;
    // SDL's Vulkan driver halves both sides per level without stopping at 1 (SDL 3.4.18), so the last levels of a
    // non-square texture would be blits to an empty region: the levels are blitted here, each side at least 1.
    if (caps.driver == "vulkan" && desc.width != desc.height && desc.type != TextureType::Texture3D) {
        const std::uint32_t layers = desc.type == TextureType::Texture2D ? 1u : desc.layers;
        for (std::uint32_t level = 1; level < desc.mipLevels; ++level)
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                SDL_GPUBlitInfo info{};
                info.source = {sdl::texture(texture),
                               level - 1,
                               layer,
                               0,
                               0,
                               std::max(desc.width >> (level - 1), 1u),
                               std::max(desc.height >> (level - 1), 1u)};
                info.destination = {sdl::texture(texture),
                                    level,
                                    layer,
                                    0,
                                    0,
                                    std::max(desc.width >> level, 1u),
                                    std::max(desc.height >> level, 1u)};
                info.load_op = SDL_GPU_LOADOP_DONT_CARE;
                info.filter = SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(nativeCommands(commands), &info);
            }
        return;
    }
    SDL_GenerateMipmapsForGPUTexture(nativeCommands(commands), sdl::texture(texture));
}

void SdlGpuDevice::blit(void* commands, const BlitDesc& blit) {
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
    SDL_BlitGPUTexture(nativeCommands(commands), &info);
}

TextureRef SdlGpuDevice::acquireSwapchainTexture(void* commands, SDL_Window* window) {
    SDL_GPUTexture* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(nativeCommands(commands), window, &texture, &width, &height)) {
        log::error("gpu", "SDL_WaitAndAcquireGPUSwapchainTexture: {}", SDL_GetError());
        return {};
    }
    if (!texture) return {};
    TextureRef ref;
    ref.native = texture;
    ref.desc.format = sdl::fromSdl(SDL_GetGPUSwapchainTextureFormat(m_device, window));
    ref.desc.usage = TextureUsage::ColorTarget;
    ref.desc.width = width;
    ref.desc.height = height;
    return ref;
}

void SdlGpuDevice::pushDebugGroup(void* commands, const char* name) {
    SDL_PushGPUDebugGroup(nativeCommands(commands), name);
}

void SdlGpuDevice::popDebugGroup(void* commands) { SDL_PopGPUDebugGroup(nativeCommands(commands)); }

void SdlGpuDevice::insertDebugLabel(void* commands, const char* text) {
    SDL_InsertGPUDebugLabel(nativeCommands(commands), text);
}

Result<void> SdlGpuDevice::submit(void* commands) {
    if (!SDL_SubmitGPUCommandBuffer(nativeCommands(commands)))
        return Error{std::string("SDL_SubmitGPUCommandBuffer: ") + SDL_GetError()};
    return {};
}

Result<void*> SdlGpuDevice::submitWithFence(void* commands) {
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(nativeCommands(commands));
    if (!fence) return Error{std::string("SDL_SubmitGPUCommandBufferAndAcquireFence: ") + SDL_GetError()};
    return static_cast<void*>(fence);
}

void SdlGpuDevice::cancel(void* commands) { SDL_CancelGPUCommandBuffer(nativeCommands(commands)); }

// ------------------------------------------------------------------ render passes

void SdlGpuDevice::bindPipeline(void* pass, void* pipeline) {
    SDL_BindGPUGraphicsPipeline(nativePass(pass), static_cast<SDL_GPUGraphicsPipeline*>(pipeline));
}

void SdlGpuDevice::setViewport(void* pass, const Viewport& viewport) {
    const SDL_GPUViewport v{viewport.x,      viewport.y,        viewport.width,
                            viewport.height, viewport.minDepth, viewport.maxDepth};
    SDL_SetGPUViewport(nativePass(pass), &v);
}

void SdlGpuDevice::setScissor(void* pass, const Rect& scissor) {
    const SDL_Rect r{scissor.x, scissor.y, scissor.width, scissor.height};
    SDL_SetGPUScissor(nativePass(pass), &r);
}

void SdlGpuDevice::setBlendConstants(void* pass, const Color& color) {
    SDL_SetGPUBlendConstants(nativePass(pass), sdl::toSdl(color));
}

void SdlGpuDevice::bindVertexBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferBinding> buffers) {
    std::array<SDL_GPUBufferBinding, 16> bindings{};
    for (std::size_t i = 0; i < buffers.size(); ++i) bindings[i] = {sdl::buffer(buffers[i].buffer), buffers[i].offset};
    SDL_BindGPUVertexBuffers(nativePass(pass), firstSlot, bindings.data(), std::uint32_t(buffers.size()));
}

void SdlGpuDevice::bindIndexBuffer(void* pass, const BufferBinding& buffer, IndexFormat format) {
    const SDL_GPUBufferBinding binding{sdl::buffer(buffer.buffer), buffer.offset};
    SDL_BindGPUIndexBuffer(nativePass(pass), &binding,
                           format == IndexFormat::Uint16 ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                                         : SDL_GPU_INDEXELEMENTSIZE_32BIT);
}

void SdlGpuDevice::bindSamplers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                std::span<const TextureSamplerBinding> bindings) {
    std::array<SDL_GPUTextureSamplerBinding, 16> native{};
    for (std::size_t i = 0; i < bindings.size(); ++i)
        native[i] = {sdl::texture(bindings[i].texture), sdl::sampler(bindings[i].sampler)};
    if (stage == ShaderStage::Vertex)
        SDL_BindGPUVertexSamplers(nativePass(pass), firstSlot, native.data(), std::uint32_t(bindings.size()));
    else
        SDL_BindGPUFragmentSamplers(nativePass(pass), firstSlot, native.data(), std::uint32_t(bindings.size()));
}

void SdlGpuDevice::bindStorageBuffers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                      std::span<const BufferRef> buffers) {
    std::array<SDL_GPUBuffer*, 8> native{};
    for (std::size_t i = 0; i < buffers.size(); ++i) native[i] = sdl::buffer(buffers[i]);
    if (stage == ShaderStage::Vertex)
        SDL_BindGPUVertexStorageBuffers(nativePass(pass), firstSlot, native.data(), std::uint32_t(buffers.size()));
    else
        SDL_BindGPUFragmentStorageBuffers(nativePass(pass), firstSlot, native.data(), std::uint32_t(buffers.size()));
}

void SdlGpuDevice::bindStorageTextures(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                       std::span<const TextureRef> textures) {
    std::array<SDL_GPUTexture*, 8> native{};
    for (std::size_t i = 0; i < textures.size(); ++i) native[i] = sdl::texture(textures[i]);
    if (stage == ShaderStage::Vertex)
        SDL_BindGPUVertexStorageTextures(nativePass(pass), firstSlot, native.data(), std::uint32_t(textures.size()));
    else
        SDL_BindGPUFragmentStorageTextures(nativePass(pass), firstSlot, native.data(), std::uint32_t(textures.size()));
}

void SdlGpuDevice::draw(void* pass, std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                        std::uint32_t firstInstance) {
    SDL_DrawGPUPrimitives(nativePass(pass), vertexCount, instanceCount, firstVertex, firstInstance);
}

void SdlGpuDevice::drawIndexed(void* pass, std::uint32_t indexCount, std::uint32_t instanceCount,
                               std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance) {
    SDL_DrawGPUIndexedPrimitives(nativePass(pass), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void SdlGpuDevice::drawIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) {
    SDL_DrawGPUPrimitivesIndirect(nativePass(pass), sdl::buffer(buffer), offset, drawCount);
}

void SdlGpuDevice::drawIndexedIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset,
                                       std::uint32_t drawCount) {
    SDL_DrawGPUIndexedPrimitivesIndirect(nativePass(pass), sdl::buffer(buffer), offset, drawCount);
}

// ------------------------------------------------------------------ compute passes

void SdlGpuDevice::bindComputePipeline(void* pass, void* pipeline) {
    SDL_BindGPUComputePipeline(nativeCompute(pass), static_cast<SDL_GPUComputePipeline*>(pipeline));
}

void SdlGpuDevice::bindComputeStorageBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferRef> buffers) {
    std::array<SDL_GPUBuffer*, 8> native{};
    for (std::size_t i = 0; i < buffers.size(); ++i) native[i] = sdl::buffer(buffers[i]);
    SDL_BindGPUComputeStorageBuffers(nativeCompute(pass), firstSlot, native.data(), std::uint32_t(buffers.size()));
}

void SdlGpuDevice::dispatch(void* pass, std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) {
    SDL_DispatchGPUCompute(nativeCompute(pass), groupsX, groupsY, groupsZ);
}

// ------------------------------------------------------------------ copy passes

void SdlGpuDevice::uploadToBuffer(void* pass, void* transferBuffer, std::uint32_t sourceOffset,
                                  const BufferRef& destination, std::uint32_t destinationOffset, std::uint32_t size,
                                  bool cycle) {
    const SDL_GPUTransferBufferLocation from{nativeTransfer(transferBuffer), sourceOffset};
    const SDL_GPUBufferRegion to{sdl::buffer(destination), destinationOffset, size};
    SDL_UploadToGPUBuffer(nativeCopy(pass), &from, &to, cycle);
}

void SdlGpuDevice::uploadToTexture(void* pass, void* transferBuffer, std::uint32_t sourceOffset,
                                   const TextureRegion& destination, bool cycle) {
    const SDL_GPUTextureTransferInfo from{nativeTransfer(transferBuffer), sourceOffset, 0, 0};
    const SDL_GPUTextureRegion to = sdl::toSdlRegion(destination);
    SDL_UploadToGPUTexture(nativeCopy(pass), &from, &to, cycle);
}

void SdlGpuDevice::downloadFromTexture(void* pass, const TextureRegion& source, void* transferBuffer,
                                       std::uint32_t destinationOffset) {
    const SDL_GPUTextureRegion from = sdl::toSdlRegion(source);
    const SDL_GPUTextureTransferInfo to{nativeTransfer(transferBuffer), destinationOffset, 0, 0};
    SDL_DownloadFromGPUTexture(nativeCopy(pass), &from, &to);
}

void SdlGpuDevice::downloadFromBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset,
                                      std::uint32_t size, void* transferBuffer, std::uint32_t destinationOffset) {
    const SDL_GPUBufferRegion from{sdl::buffer(source), sourceOffset, size};
    const SDL_GPUTransferBufferLocation to{nativeTransfer(transferBuffer), destinationOffset};
    SDL_DownloadFromGPUBuffer(nativeCopy(pass), &from, &to);
}

void SdlGpuDevice::copyBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset,
                              const BufferRef& destination, std::uint32_t destinationOffset, std::uint32_t size,
                              bool cycle) {
    const SDL_GPUBufferLocation from{sdl::buffer(source), sourceOffset};
    const SDL_GPUBufferLocation to{sdl::buffer(destination), destinationOffset};
    SDL_CopyGPUBufferToBuffer(nativeCopy(pass), &from, &to, size, cycle);
}

void SdlGpuDevice::copyTexture(void* pass, const TextureRegion& source, const TextureRegion& destination, bool cycle) {
    const SDL_GPUTextureRegion region = sdl::toSdlRegion(source);
    const SDL_GPUTextureLocation from{region.texture, region.mip_level, region.layer, region.x, region.y, region.z};
    const SDL_GPUTextureLocation to{sdl::texture(destination.texture),
                                    destination.mipLevel,
                                    destination.layer,
                                    destination.x,
                                    destination.y,
                                    destination.z};
    SDL_CopyGPUTextureToTexture(nativeCopy(pass), &from, &to, region.w, region.h, region.d, cycle);
}

} // namespace quench::gpu::detail
