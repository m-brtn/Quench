// The native Metal backend: command buffers, passes, bindings, draws, copies, blits and timestamps. Bindings are
// recorded and applied at the next draw or dispatch, with the slot layout of SDL GPU's Metal backend.

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#include "gpu/metal/MetalDevice.h"

#include "gpu/shaders/BlitUniforms.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace quench::gpu::detail {

namespace {

MetalCommandBuffer& commandsOf(void* native) { return *static_cast<MetalCommandBuffer*>(native); }
MetalTexture& textureOf(const TextureRef& ref) { return *static_cast<MetalTexture*>(ref.native); }
id<MTLBuffer> bufferOf(const BufferRef& ref) { return static_cast<MetalBuffer*>(ref.native)->handle; }
MetalTransferBuffer& transferOf(void* native) { return *static_cast<MetalTransferBuffer*>(native); }

std::size_t stageIndex(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex: return 0;
    case ShaderStage::Fragment: return 1;
    case ShaderStage::Compute: return 2;
    }
    return 0;
}

MTLLoadAction loadAction(LoadOp op) {
    switch (op) {
    case LoadOp::Load: return MTLLoadActionLoad;
    case LoadOp::Clear: return MTLLoadActionClear;
    case LoadOp::DontCare: return MTLLoadActionDontCare;
    }
    return MTLLoadActionDontCare;
}

MTLStoreAction storeAction(StoreOp op) {
    switch (op) {
    case StoreOp::Store: return MTLStoreActionStore;
    case StoreOp::DontCare: return MTLStoreActionDontCare;
    case StoreOp::Resolve: return MTLStoreActionMultisampleResolve;
    case StoreOp::ResolveAndStore: return MTLStoreActionStoreAndMultisampleResolve;
    }
    return MTLStoreActionStore;
}

// Bytes of one row of blocks and of one tightly packed slice of a region.
struct Pitch {
    NSUInteger row;
    NSUInteger image;
};

Pitch pitchOf(TextureFormat format, std::uint32_t width, std::uint32_t height) {
    const FormatInfo& info = formatInfo(format);
    const NSUInteger blocksX = (width + info.blockWidth - 1) / info.blockWidth;
    const NSUInteger blocksY = (height + info.blockHeight - 1) / info.blockHeight;
    return {blocksX * info.blockBytes, blocksX * blocksY * info.blockBytes};
}

constexpr std::array<NSUInteger, metal::kMaxStorage> kZeroOffsets{};

} // namespace

void MetalCommandBuffer::resetPassState() {
    pass = MetalPass::None;
    render = nil;
    compute = nil;
    blit = nil;
    graphicsPipeline = nullptr;
    vertexBuffers.fill(nil);
    vertexOffsets.fill(0);
    vertexBufferCount = 0;
    vertexBuffersDirty = false;
    indexBuffer = nil;
    indexOffset = 0;
    for (std::size_t s = 0; s < 2; ++s) {
        textures[s].fill(nil);
        samplers[s].fill(nil);
        storageBuffers[s].fill(nil);
        storageTextures[s].fill(nil);
    }
    samplersDirty = {};
    storageBuffersDirty = {};
    storageTexturesDirty = {};
    accel = nil;
    accelBottomBuilt = false;
    scratchOffset = 0;
    for (auto& stage : accelerationStructures) stage.fill(nullptr);
    accelerationStructuresDirty = {};
    computePipeline = nullptr;
    computeBuffers.fill(nil);
    computeWrites.fill(nil);
    computeBuffersDirty = false;
}

// ------------------------------------------------------------------ command buffers

void MetalDevice::reclaim() {
    for (std::size_t i = 0; i < m_live.size();) {
        MetalCommandBuffer& c = *m_live[i];
        const MTLCommandBufferStatus status = c.submitted ? c.handle.status : MTLCommandBufferStatusNotEnqueued;
        const bool done =
            c.cancelled || status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError;
        if (!done) {
            ++i;
            continue;
        }
        if (status == MTLCommandBufferStatusError)
            log::error("gpu", "Metal command buffer failed: {}",
                       c.handle.error ? c.handle.error.localizedDescription.UTF8String : "unknown error");
        for (id<MTLBuffer> chunk : c.uniformChunks) m_uniformPool.push_back(chunk);
        c.uniformChunks.clear();
        for (id<MTLBuffer> buffer : c.instanceBuffers) m_instancePool.push_back(buffer);
        c.instanceBuffers.clear();
        c.handle = nil;
        c.timing.reset();
        c.presents.clear();
        c.resetPassState();
        m_free.push_back(std::move(m_live[i]));
        m_live[i] = std::move(m_live.back());
        m_live.pop_back();
    }
}

std::uint64_t MetalDevice::finishedBelow() const {
    std::uint64_t oldest = m_nextSerial;
    for (const std::unique_ptr<MetalCommandBuffer>& c : m_live) oldest = std::min(oldest, c->serial);
    return oldest;
}

void MetalDevice::useTransfer(MetalCommandBuffer& commands, MetalTransferBuffer& buffer) {
    buffer.current().lastUse = std::max(buffer.current().lastUse, commands.serial);
}

id<MTLBuffer> MetalDevice::uniformChunk() {
    if (!m_uniformPool.empty()) {
        id<MTLBuffer> chunk = m_uniformPool.back();
        m_uniformPool.pop_back();
        return chunk;
    }
    return [m_device newBufferWithLength:metal::kUniformChunkSize
                                 options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
}

void* MetalDevice::acquireCommandBuffer() {
    @autoreleasepool {
        reclaim();
        std::unique_ptr<MetalCommandBuffer> c;
        if (m_free.empty()) {
            c = std::make_unique<MetalCommandBuffer>();
        } else {
            c = std::move(m_free.back());
            m_free.pop_back();
        }
        c->handle = [m_queue commandBuffer];
        if (!c->handle) {
            log::error("gpu", "cannot create a Metal command buffer");
            m_free.push_back(std::move(c));
            return nullptr;
        }
        c->serial = m_nextSerial++;
        c->submitted = false;
        c->cancelled = false;
        c->uniformOffset = metal::kUniformChunkSize;
        for (auto& stage : c->uniforms) stage.fill({});
        c->resetPassState();
        MetalCommandBuffer* raw = c.get();
        m_live.push_back(std::move(c));
        return raw;
    }
}

std::pair<id<MTLBuffer>, std::uint32_t> MetalDevice::uniformSpace(MetalCommandBuffer& c, std::uint32_t size) {
    QUENCH_ASSERT(size <= metal::kUniformChunkSize, "uniform block larger than 32 KB");
    const std::uint32_t aligned = (size + metal::kUniformAlignment - 1) & ~(metal::kUniformAlignment - 1);
    if (c.uniformChunks.empty() || c.uniformOffset + aligned > metal::kUniformChunkSize) {
        id<MTLBuffer> chunk = uniformChunk();
        if (!chunk) {
            log::error("gpu", "cannot allocate uniform memory");
            return {nil, 0};
        }
        c.uniformChunks.push_back(chunk);
        c.uniformOffset = 0;
    }
    const std::uint32_t offset = c.uniformOffset;
    c.uniformOffset += aligned;
    return {c.uniformChunks.back(), offset};
}

void MetalDevice::pushUniforms(void* commands, ShaderStage stage, std::uint32_t slot, const void* data,
                               std::uint32_t size) {
    MetalCommandBuffer& c = commandsOf(commands);
    QUENCH_ASSERT(slot < metal::kUniformSlots, "uniform slot out of range");
    auto [chunk, offset] = uniformSpace(c, size);
    if (!chunk) return;
    std::memcpy(static_cast<std::byte*>(chunk.contents) + offset, data, size);
    MetalUniformBinding& binding = c.uniforms[stageIndex(stage)][slot];
    binding.buffer = chunk;
    binding.offset = offset;
    binding.dirty = true;
}

TextureRef MetalDevice::acquireSwapchainTexture(void* commands, SDL_Window* window) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        MetalWindow* w = findWindow(window);
        if (!w) {
            log::error("gpu", "swapchain texture of a window the Metal device has not claimed");
            return {};
        }
        // At most kFramesInFlight frames queued per window, as SDL GPU allows.
        if (id<MTLCommandBuffer> previous = w->inFlight[w->frame]) {
            [previous waitUntilCompleted];
            w->inFlight[w->frame] = nil;
        }
        const CGSize size = w->layer.drawableSize;
        if (size.width < 1.0 || size.height < 1.0) return {};
        w->drawable = [w->layer nextDrawable];
        if (!w->drawable) return {};
        w->texture.handle = w->drawable.texture;
        c.presents.push_back(w);
        TextureRef ref;
        ref.native = &w->texture;
        ref.desc.format = w->format;
        ref.desc.usage = TextureUsage::ColorTarget;
        ref.desc.width = std::uint32_t(w->texture.handle.width);
        ref.desc.height = std::uint32_t(w->texture.handle.height);
        return ref;
    }
}

void MetalDevice::pushDebugGroup(void* commands, const char* name) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        NSString* text = @(name);
        if (c.render)
            [c.render pushDebugGroup:text];
        else if (c.blit)
            [c.blit pushDebugGroup:text];
        else if (c.compute)
            [c.compute pushDebugGroup:text];
        else
            [c.handle pushDebugGroup:text];
    }
}

void MetalDevice::popDebugGroup(void* commands) {
    MetalCommandBuffer& c = commandsOf(commands);
    if (c.render)
        [c.render popDebugGroup];
    else if (c.blit)
        [c.blit popDebugGroup];
    else if (c.compute)
        [c.compute popDebugGroup];
    else
        [c.handle popDebugGroup];
}

void MetalDevice::insertDebugLabel(void* commands, const char* text) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        NSString* label = @(text);
        if (c.render) {
            [c.render insertDebugSignpost:label];
        } else if (c.blit) {
            [c.blit insertDebugSignpost:label];
        } else if (c.compute) {
            [c.compute insertDebugSignpost:label];
        } else {
            [c.handle pushDebugGroup:label];
            [c.handle popDebugGroup];
        }
    }
}

Result<void*> MetalDevice::commit(MetalCommandBuffer& c, bool fence) {
    @autoreleasepool {
        for (MetalWindow* w : c.presents) {
            [c.handle presentDrawable:w->drawable];
            w->drawable = nil;
            w->texture.handle = nil;
            w->inFlight[w->frame] = c.handle;
            w->frame = (w->frame + 1) % metal::kFramesInFlight;
        }
        c.presents.clear();
        if (c.timing) {
            // Ranges still open at the submit end with the last pass.
            for (std::size_t index : c.timing->open) {
                c.timing->ranges[index].endEncoder = c.timing->encoders.size();
                c.timing->ranges[index].closed = true;
            }
            c.timing->open.clear();
        }
        [c.handle commit];
        c.submitted = true;
        void* result = fence ? new MetalFence{c.handle, c.timing} : nullptr;
        reclaim();
        return result;
    }
}

Result<void> MetalDevice::submit(void* commands) {
    Result<void*> committed = commit(commandsOf(commands), false);
    if (!committed) return Error{committed.error()};
    return {};
}

Result<void*> MetalDevice::submitWithFence(void* commands) { return commit(commandsOf(commands), true); }

void MetalDevice::cancel(void* commands) {
    MetalCommandBuffer& c = commandsOf(commands);
    c.cancelled = true;
    c.presents.clear();
    reclaim();
}

// ------------------------------------------------------------------ timestamps

void MetalDevice::beginTiming(void* commands, const char* name) {
    MetalCommandBuffer& c = commandsOf(commands);
    if (!c.timing) {
        c.timing = std::make_shared<MetalTiming>();
        c.timing->device = this;
    }
    MetalTiming& t = *c.timing;
    t.ranges.push_back({.name = name,
                        .depth = std::uint32_t(t.open.size()),
                        .firstEncoder = t.encoders.size(),
                        .endEncoder = t.encoders.size()});
    t.open.push_back(t.ranges.size() - 1);
}

void MetalDevice::endTiming(void* commands) {
    MetalCommandBuffer& c = commandsOf(commands);
    QUENCH_ASSERT(c.timing && !c.timing->open.empty(), "endTiming without beginTiming");
    if (!c.timing || c.timing->open.empty()) return;
    MetalTiming::Range& range = c.timing->ranges[c.timing->open.back()];
    range.endEncoder = c.timing->encoders.size();
    range.closed = true;
    c.timing->open.pop_back();
}

std::int64_t MetalDevice::timingSamples(MetalCommandBuffer& c, std::uint32_t count) {
    if (!c.timing || c.timing->open.empty()) return -1;
    MetalTiming& t = *c.timing;
    if (!t.samples) {
        if (!m_samplePool.empty()) {
            t.samples = m_samplePool.back();
            m_samplePool.pop_back();
        } else {
            MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
            d.counterSet = m_timestampSet;
            d.storageMode = MTLStorageModeShared;
            d.sampleCount = metal::kTimestampSamples;
            NSError* error = nil;
            t.samples = [m_device newCounterSampleBufferWithDescriptor:d error:&error];
            if (!t.samples) {
                static bool logged = false;
                if (!logged)
                    log::error("gpu", "cannot create a counter sample buffer: {}",
                               error ? error.localizedDescription.UTF8String : "unknown error");
                logged = true;
            }
        }
    }
    if (!t.samples || t.used + count > metal::kTimestampSamples) {
        // Not sampled: the open ranges cannot be measured completely.
        for (std::size_t index : t.open) t.ranges[index].complete = false;
        t.encoders.push_back({0, 0});
        return -1;
    }
    const std::uint32_t first = t.used;
    t.used += count;
    t.encoders.push_back({first, count});
    return first;
}

// ------------------------------------------------------------------ render passes

void* MetalDevice::beginRenderPass(void* commands, std::span<const ColorTarget> colors, const DepthTarget* depth) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
        std::uint32_t width = ~0u;
        std::uint32_t height = ~0u;
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const ColorTarget& in = colors[i];
            MTLRenderPassColorAttachmentDescriptor* a = d.colorAttachments[i];
            a.texture = textureOf(in.texture).handle;
            a.level = in.mipLevel;
            if (in.texture.desc.type == TextureType::Texture3D)
                a.depthPlane = in.layer;
            else
                a.slice = in.layer;
            a.clearColor = MTLClearColorMake(in.clear.r, in.clear.g, in.clear.b, in.clear.a);
            a.loadAction = loadAction(in.load);
            a.storeAction = storeAction(in.store);
            if (in.store == StoreOp::Resolve || in.store == StoreOp::ResolveAndStore) {
                a.resolveTexture = textureOf(in.resolveTexture).handle;
                a.resolveSlice = in.resolveLayer;
                a.resolveLevel = in.resolveMipLevel;
            }
            width = std::min(width, in.texture.desc.width >> in.mipLevel);
            height = std::min(height, in.texture.desc.height >> in.mipLevel);
        }
        if (depth) {
            id<MTLTexture> texture = textureOf(depth->texture).handle;
            d.depthAttachment.texture = texture;
            d.depthAttachment.level = depth->mipLevel;
            d.depthAttachment.slice = depth->layer;
            d.depthAttachment.loadAction = loadAction(depth->load);
            d.depthAttachment.storeAction = storeAction(depth->store);
            d.depthAttachment.clearDepth = depth->clearDepth;
            if (formatInfo(depth->texture.desc.format).stencil) {
                d.stencilAttachment.texture = texture;
                d.stencilAttachment.level = depth->mipLevel;
                d.stencilAttachment.slice = depth->layer;
                d.stencilAttachment.loadAction = loadAction(depth->stencilLoad);
                d.stencilAttachment.storeAction = storeAction(depth->stencilStore);
                d.stencilAttachment.clearStencil = depth->clearStencil;
            }
            width = std::min(width, depth->texture.desc.width >> depth->mipLevel);
            height = std::min(height, depth->texture.desc.height >> depth->mipLevel);
        }
        if (const std::int64_t first = timingSamples(c, 4); first >= 0) {
            MTLRenderPassSampleBufferAttachmentDescriptor* s = d.sampleBufferAttachments[0];
            s.sampleBuffer = c.timing->samples;
            s.startOfVertexSampleIndex = NSUInteger(first);
            s.endOfVertexSampleIndex = NSUInteger(first + 1);
            s.startOfFragmentSampleIndex = NSUInteger(first + 2);
            s.endOfFragmentSampleIndex = NSUInteger(first + 3);
        }
        c.render = [c.handle renderCommandEncoderWithDescriptor:d];
        if (!c.render) {
            log::error("gpu", "cannot begin a Metal render pass");
            return nullptr;
        }
        c.pass = MetalPass::Render;
        // SDL GPU's defaults: the whole (smallest) target, blend constants 1, stencil reference 0.
        [c.render setViewport:MTLViewport{0.0, 0.0, double(width), double(height), 0.0, 1.0}];
        [c.render setScissorRect:MTLScissorRect{0, 0, width, height}];
        [c.render setBlendColorRed:1.0f green:1.0f blue:1.0f alpha:1.0f];
        [c.render setStencilReferenceValue:0];
        return &c;
    }
}

void MetalDevice::endRenderPass(void*, void* pass) {
    MetalCommandBuffer& c = commandsOf(pass);
    [c.render endEncoding];
    c.resetPassState();
}

void MetalDevice::bindPipeline(void* pass, void* pipeline) {
    MetalCommandBuffer& c = commandsOf(pass);
    const MetalGraphicsPipeline& p = *static_cast<MetalGraphicsPipeline*>(pipeline);
    c.graphicsPipeline = &p;
    [c.render setRenderPipelineState:p.state];
    [c.render setTriangleFillMode:p.fill];
    [c.render setCullMode:p.cull];
    [c.render setFrontFacingWinding:p.winding];
#if !QUENCH_PLATFORM_IOS_SIMULATOR
    [c.render setDepthClipMode:p.depthClip];
#endif
    [c.render setDepthBias:p.depthBias slopeScale:p.depthBiasSlope clamp:p.depthBiasClamp];
    if (p.depthStencil) [c.render setDepthStencilState:p.depthStencil];
    c.samplersDirty = {true, true};
    c.storageBuffersDirty = {true, true};
    c.storageTexturesDirty = {true, true};
    c.accelerationStructuresDirty[0] = c.accelerationStructuresDirty[1] = true;
    for (std::size_t s = 0; s < 2; ++s)
        for (MetalUniformBinding& binding : c.uniforms[s]) binding.dirty = true;
}

void MetalDevice::setViewport(void* pass, const Viewport& v) {
    [commandsOf(pass).render setViewport:MTLViewport{v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth}];
}

void MetalDevice::setScissor(void* pass, const Rect& r) {
    [commandsOf(pass).render
        setScissorRect:MTLScissorRect{NSUInteger(r.x), NSUInteger(r.y), NSUInteger(r.width), NSUInteger(r.height)}];
}

void MetalDevice::setBlendConstants(void* pass, const Color& color) {
    [commandsOf(pass).render setBlendColorRed:color.r green:color.g blue:color.b alpha:color.a];
}

void MetalDevice::bindVertexBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferBinding> buffers) {
    MetalCommandBuffer& c = commandsOf(pass);
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        c.vertexBuffers[firstSlot + i] = bufferOf(buffers[i].buffer);
        c.vertexOffsets[firstSlot + i] = buffers[i].offset;
    }
    c.vertexBufferCount = std::max(c.vertexBufferCount, firstSlot + std::uint32_t(buffers.size()));
    c.vertexBuffersDirty = true;
}

void MetalDevice::bindIndexBuffer(void* pass, const BufferBinding& buffer, IndexFormat format) {
    MetalCommandBuffer& c = commandsOf(pass);
    c.indexBuffer = bufferOf(buffer.buffer);
    c.indexOffset = buffer.offset;
    c.indexType = format == IndexFormat::Uint32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
}

void MetalDevice::bindSamplers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                               std::span<const TextureSamplerBinding> bindings) {
    MetalCommandBuffer& c = commandsOf(pass);
    const std::size_t s = stageIndex(stage);
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        c.textures[s][firstSlot + i] = textureOf(bindings[i].texture).handle;
        c.samplers[s][firstSlot + i] = static_cast<MetalSampler*>(bindings[i].sampler.native)->handle;
    }
    c.samplersDirty[s] = true;
}

void MetalDevice::bindStorageBuffers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                     std::span<const BufferRef> buffers) {
    MetalCommandBuffer& c = commandsOf(pass);
    const std::size_t s = stageIndex(stage);
    for (std::size_t i = 0; i < buffers.size(); ++i) c.storageBuffers[s][firstSlot + i] = bufferOf(buffers[i]);
    c.storageBuffersDirty[s] = true;
}

void MetalDevice::bindStorageTextures(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                      std::span<const TextureRef> textures) {
    MetalCommandBuffer& c = commandsOf(pass);
    const std::size_t s = stageIndex(stage);
    for (std::size_t i = 0; i < textures.size(); ++i)
        c.storageTextures[s][firstSlot + i] = textureOf(textures[i]).handle;
    c.storageTexturesDirty[s] = true;
}

void MetalDevice::flushGraphics(MetalCommandBuffer& c) {
    id<MTLRenderCommandEncoder> e = c.render;
    const MetalGraphicsPipeline& p = *c.graphicsPipeline;
    if (c.vertexBuffersDirty) {
        [e setVertexBuffers:c.vertexBuffers.data()
                    offsets:c.vertexOffsets.data()
                  withRange:NSMakeRange(metal::kFirstVertexBufferSlot, c.vertexBufferCount)];
        c.vertexBuffersDirty = false;
    }
    for (std::size_t s = 0; s < 2; ++s) {
        const MetalStageCounts& n = s == 0 ? p.vertex : p.fragment;
        const bool vertex = s == 0;
        if (c.samplersDirty[s] && n.samplers > 0) {
            const NSRange range = NSMakeRange(0, n.samplers);
            if (vertex) {
                [e setVertexTextures:c.textures[s].data() withRange:range];
                [e setVertexSamplerStates:c.samplers[s].data() withRange:range];
            } else {
                [e setFragmentTextures:c.textures[s].data() withRange:range];
                [e setFragmentSamplerStates:c.samplers[s].data() withRange:range];
            }
        }
        c.samplersDirty[s] = false;
        if (c.storageTexturesDirty[s] && n.storageTextures > 0) {
            const NSRange range = NSMakeRange(n.samplers, n.storageTextures);
            if (vertex)
                [e setVertexTextures:c.storageTextures[s].data() withRange:range];
            else
                [e setFragmentTextures:c.storageTextures[s].data() withRange:range];
        }
        c.storageTexturesDirty[s] = false;
        if (c.storageBuffersDirty[s] && n.storageBuffers > 0) {
            const NSRange range = NSMakeRange(n.uniformBuffers, n.storageBuffers);
            if (vertex)
                [e setVertexBuffers:c.storageBuffers[s].data() offsets:kZeroOffsets.data() withRange:range];
            else
                [e setFragmentBuffers:c.storageBuffers[s].data() offsets:kZeroOffsets.data() withRange:range];
        }
        c.storageBuffersDirty[s] = false;
        if (c.accelerationStructuresDirty[s] && n.accelerationStructures > 0) {
            const MTLRenderStages stages = vertex ? MTLRenderStageVertex : MTLRenderStageFragment;
            for (std::uint32_t i = 0; i < n.accelerationStructures && i < metal::kMaxAccelerationStructures; ++i) {
                const MetalAccelerationStructure* as = c.accelerationStructures[s][i];
                if (!as) continue;
                const NSUInteger index = n.uniformBuffers + n.storageBuffers + i;
                if (vertex)
                    [e setVertexAccelerationStructure:as->handle atBufferIndex:index];
                else
                    [e setFragmentAccelerationStructure:as->handle atBufferIndex:index];
                if (!as->residency.empty())
                    [e useResources:as->residency.data()
                              count:as->residency.size()
                              usage:MTLResourceUsageRead
                             stages:stages];
            }
        }
        c.accelerationStructuresDirty[s] = false;
        for (std::uint32_t i = 0; i < n.uniformBuffers; ++i) {
            MetalUniformBinding& b = c.uniforms[s][i];
            if (!b.dirty) continue;
            id<MTLBuffer> buffer = b.buffer ? b.buffer : m_zeroUniforms;
            if (vertex)
                [e setVertexBuffer:buffer offset:b.offset atIndex:i];
            else
                [e setFragmentBuffer:buffer offset:b.offset atIndex:i];
            b.dirty = false;
        }
    }
}

void MetalDevice::draw(void* pass, std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                       std::uint32_t firstInstance) {
    MetalCommandBuffer& c = commandsOf(pass);
    flushGraphics(c);
    [c.render drawPrimitives:c.graphicsPipeline->primitive
                 vertexStart:firstVertex
                 vertexCount:vertexCount
               instanceCount:instanceCount
                baseInstance:firstInstance];
}

void MetalDevice::drawIndexed(void* pass, std::uint32_t indexCount, std::uint32_t instanceCount,
                              std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance) {
    MetalCommandBuffer& c = commandsOf(pass);
    flushGraphics(c);
    const NSUInteger indexSize = c.indexType == MTLIndexTypeUInt32 ? 4 : 2;
    [c.render drawIndexedPrimitives:c.graphicsPipeline->primitive
                         indexCount:indexCount
                          indexType:c.indexType
                        indexBuffer:c.indexBuffer
                  indexBufferOffset:c.indexOffset + firstIndex * indexSize
                      instanceCount:instanceCount
                         baseVertex:vertexOffset
                       baseInstance:firstInstance];
}

void MetalDevice::drawIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) {
    MetalCommandBuffer& c = commandsOf(pass);
    flushGraphics(c);
    id<MTLBuffer> indirect = bufferOf(buffer);
    // 16-byte commands (vertex count, instance count, first vertex, first instance).
    for (std::uint32_t i = 0; i < drawCount; ++i)
        [c.render drawPrimitives:c.graphicsPipeline->primitive
                  indirectBuffer:indirect
            indirectBufferOffset:offset + NSUInteger(i) * 16];
}

void MetalDevice::drawIndexedIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset,
                                      std::uint32_t drawCount) {
    MetalCommandBuffer& c = commandsOf(pass);
    flushGraphics(c);
    id<MTLBuffer> indirect = bufferOf(buffer);
    // 20-byte commands (index count, instance count, first index, vertex offset, first instance).
    for (std::uint32_t i = 0; i < drawCount; ++i)
        [c.render drawIndexedPrimitives:c.graphicsPipeline->primitive
                              indexType:c.indexType
                            indexBuffer:c.indexBuffer
                      indexBufferOffset:c.indexOffset
                         indirectBuffer:indirect
                   indirectBufferOffset:offset + NSUInteger(i) * 20];
}

// ------------------------------------------------------------------ compute passes

void* MetalDevice::beginComputePass(void* commands, std::span<const StorageBufferWrite> writes) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        if (const std::int64_t first = timingSamples(c, 2); first >= 0) {
            MTLComputePassDescriptor* d = [MTLComputePassDescriptor computePassDescriptor];
            d.dispatchType = MTLDispatchTypeSerial;
            d.sampleBufferAttachments[0].sampleBuffer = c.timing->samples;
            d.sampleBufferAttachments[0].startOfEncoderSampleIndex = NSUInteger(first);
            d.sampleBufferAttachments[0].endOfEncoderSampleIndex = NSUInteger(first + 1);
            c.compute = [c.handle computeCommandEncoderWithDescriptor:d];
        } else {
            c.compute = [c.handle computeCommandEncoder];
        }
        if (!c.compute) {
            log::error("gpu", "cannot begin a Metal compute pass");
            return nullptr;
        }
        c.pass = MetalPass::Compute;
        QUENCH_ASSERT(writes.size() <= metal::kMaxStorage, "too many compute storage writes");
        for (std::size_t i = 0; i < writes.size(); ++i) c.computeWrites[i] = bufferOf(writes[i].buffer);
        return &c;
    }
}

void MetalDevice::endComputePass(void*, void* pass) {
    MetalCommandBuffer& c = commandsOf(pass);
    [c.compute endEncoding];
    c.resetPassState();
}

void MetalDevice::bindComputePipeline(void* pass, void* pipeline) {
    MetalCommandBuffer& c = commandsOf(pass);
    const MetalComputePipeline& p = *static_cast<MetalComputePipeline*>(pipeline);
    c.computePipeline = &p;
    [c.compute setComputePipelineState:p.state];
    if (p.readWriteStorageBuffers > 0)
        [c.compute setBuffers:c.computeWrites.data()
                      offsets:kZeroOffsets.data()
                    withRange:NSMakeRange(p.uniformBuffers + p.storageBuffers, p.readWriteStorageBuffers)];
    c.computeBuffersDirty = true;
    c.accelerationStructuresDirty[2] = true;
    for (MetalUniformBinding& binding : c.uniforms[2]) binding.dirty = true;
}

void MetalDevice::bindComputeStorageBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferRef> buffers) {
    MetalCommandBuffer& c = commandsOf(pass);
    for (std::size_t i = 0; i < buffers.size(); ++i) c.computeBuffers[firstSlot + i] = bufferOf(buffers[i]);
    c.computeBuffersDirty = true;
}

void MetalDevice::flushCompute(MetalCommandBuffer& c) {
    const MetalComputePipeline& p = *c.computePipeline;
    if (c.computeBuffersDirty && p.storageBuffers > 0)
        [c.compute setBuffers:c.computeBuffers.data()
                      offsets:kZeroOffsets.data()
                    withRange:NSMakeRange(p.uniformBuffers, p.storageBuffers)];
    c.computeBuffersDirty = false;
    if (c.accelerationStructuresDirty[2]) {
        for (std::uint32_t i = 0; i < p.accelerationStructures && i < metal::kMaxAccelerationStructures; ++i) {
            const MetalAccelerationStructure* as = c.accelerationStructures[2][i];
            if (!as) continue;
            [c.compute setAccelerationStructure:as->handle
                                  atBufferIndex:p.uniformBuffers + p.storageBuffers + p.readWriteStorageBuffers + i];
            if (!as->residency.empty())
                [c.compute useResources:as->residency.data() count:as->residency.size() usage:MTLResourceUsageRead];
        }
        c.accelerationStructuresDirty[2] = false;
    }
    for (std::uint32_t i = 0; i < p.uniformBuffers; ++i) {
        MetalUniformBinding& b = c.uniforms[2][i];
        if (!b.dirty) continue;
        [c.compute setBuffer:(b.buffer ? b.buffer : m_zeroUniforms) offset:b.offset atIndex:i];
        b.dirty = false;
    }
}

void MetalDevice::dispatch(void* pass, std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) {
    MetalCommandBuffer& c = commandsOf(pass);
    flushCompute(c);
    [c.compute dispatchThreadgroups:MTLSizeMake(groupsX, groupsY, groupsZ)
              threadsPerThreadgroup:c.computePipeline->threads];
}

// ------------------------------------------------------------------ acceleration structures

bool MetalDevice::beginAccelerationEncoder(MetalCommandBuffer& c) {
    if (const std::int64_t first = timingSamples(c, 2); first >= 0) {
        MTLAccelerationStructurePassDescriptor* d =
            [MTLAccelerationStructurePassDescriptor accelerationStructurePassDescriptor];
        d.sampleBufferAttachments[0].sampleBuffer = c.timing->samples;
        d.sampleBufferAttachments[0].startOfEncoderSampleIndex = NSUInteger(first);
        d.sampleBufferAttachments[0].endOfEncoderSampleIndex = NSUInteger(first + 1);
        c.accel = [c.handle accelerationStructureCommandEncoderWithDescriptor:d];
    } else {
        c.accel = [c.handle accelerationStructureCommandEncoder];
    }
    c.accelBottomBuilt = false;
    c.scratchOffset = 0;
    if (!c.accel) log::error("gpu", "cannot begin a Metal acceleration structure pass");
    return c.accel != nil;
}

void* MetalDevice::beginAccelerationStructurePass(void* commands) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        if (!beginAccelerationEncoder(c)) return nullptr;
        c.pass = MetalPass::AccelerationStructure;
        return &c;
    }
}

bool MetalDevice::afterBottomLevelBuilds(MetalCommandBuffer& c) {
    if (!c.accelBottomBuilt) return true;
    [c.accel endEncoding];
    return beginAccelerationEncoder(c);
}

void MetalDevice::endAccelerationStructurePass(void*, void* pass) {
    MetalCommandBuffer& c = commandsOf(pass);
    [c.accel endEncoding];
    c.resetPassState();
}

NSUInteger MetalDevice::scratch(MetalCommandBuffer& c, NSUInteger size) {
    size = std::max<NSUInteger>((size + metal::kScratchAlignment - 1) & ~(metal::kScratchAlignment - 1),
                                metal::kScratchAlignment);
    if (!m_scratch || c.scratchOffset + size > m_scratch.length) {
        // The builds already in the encoder keep the old buffer.
        const NSUInteger length = std::max<NSUInteger>(size, m_scratch ? m_scratch.length * 2 : 1 << 20);
        m_scratch = [m_device newBufferWithLength:length options:MTLResourceStorageModePrivate];
        if (m_debug) m_scratch.label = @"acceleration structure scratch";
        c.scratchOffset = 0;
    }
    const NSUInteger offset = c.scratchOffset;
    c.scratchOffset += size;
    return offset;
}

id<MTLBuffer> MetalDevice::instanceBuffer(MetalCommandBuffer& c, NSUInteger size) {
    id<MTLBuffer> buffer = nil;
    for (std::size_t i = 0; i < m_instancePool.size(); ++i)
        if (m_instancePool[i].length >= size) {
            buffer = m_instancePool[i];
            m_instancePool[i] = m_instancePool.back();
            m_instancePool.pop_back();
            break;
        }
    if (!buffer) {
        NSUInteger length = 4096;
        while (length < size) length *= 2;
        buffer = [m_device newBufferWithLength:length
                                       options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        if (!buffer) return nil;
    }
    c.instanceBuffers.push_back(buffer);
    return buffer;
}

void MetalDevice::buildBottomLevel(void* pass, void* structure, const RayGeometry& geometry, bool refit) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(pass);
        MetalAccelerationStructure& as = *static_cast<MetalAccelerationStructure*>(structure);
        MTLPrimitiveAccelerationStructureDescriptor* d =
            metal::bottomLevelDescriptor(&geometry, geometry.triangleCount, as.allowRefit);
        const NSUInteger offset = scratch(c, refit ? as.refitScratch : as.buildScratch);
        if (refit)
            [c.accel refitAccelerationStructure:as.handle
                                     descriptor:d
                                    destination:nil
                                  scratchBuffer:m_scratch
                            scratchBufferOffset:offset];
        else
            [c.accel buildAccelerationStructure:as.handle
                                     descriptor:d
                                  scratchBuffer:m_scratch
                            scratchBufferOffset:offset];
        c.accelBottomBuilt = true;
    }
}

void MetalDevice::buildTopLevel(void* pass, void* structure, std::span<const RayInstance> instances, bool refit) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(pass);
        MetalAccelerationStructure& as = *static_cast<MetalAccelerationStructure*>(structure);
        if (!afterBottomLevelBuilds(c)) return;

        // The distinct bottom levels, in first-use order; instances refer to them by index.
        NSMutableArray<id<MTLAccelerationStructure>>* bottomLevels = [NSMutableArray array];
        std::vector<const void*> natives;
        std::vector<std::uint32_t> indices(instances.size());
        for (std::size_t i = 0; i < instances.size(); ++i) {
            const void* native = Access::native(*instances[i].bottomLevel);
            auto found = std::find(natives.begin(), natives.end(), native);
            if (found == natives.end()) {
                natives.push_back(native);
                [bottomLevels addObject:static_cast<const MetalAccelerationStructure*>(native)->handle];
                found = natives.end() - 1;
            }
            indices[i] = std::uint32_t(found - natives.begin());
        }
        // A refit keeps each instance's bottom level from the last build; with others, or before a build, it builds.
        if (refit) {
            refit =
                as.bottomLevels && as.bottomLevels.count == bottomLevels.count && as.instanceBottomLevels == indices;
            for (NSUInteger i = 0; refit && i < bottomLevels.count; ++i) refit = as.bottomLevels[i] == bottomLevels[i];
        }

        const NSUInteger bytes =
            std::max<NSUInteger>(instances.size(), 1) * sizeof(MTLAccelerationStructureUserIDInstanceDescriptor);
        id<MTLBuffer> buffer = instanceBuffer(c, bytes);
        if (!buffer) {
            log::error("gpu", "cannot allocate {} bytes of instance descriptors", bytes);
            return;
        }
        auto* out = static_cast<MTLAccelerationStructureUserIDInstanceDescriptor*>(buffer.contents);
        for (std::size_t i = 0; i < instances.size(); ++i) {
            const RayInstance& in = instances[i];
            MTLAccelerationStructureUserIDInstanceDescriptor& o = out[i];
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 3; ++row)
                    o.transformationMatrix.columns[column].elements[row] = in.transform[std::size_t(row * 4 + column)];
            // Metal's default front faces (clockwise in its left-handed terms, judged in object space) are the
            // engine's counter-clockwise ones, also through mirroring transforms; the winding option turns them around.
            o.options = MTLAccelerationStructureInstanceOptionNone;
            if (any(in.flags & RayInstanceFlags::Opaque)) o.options |= MTLAccelerationStructureInstanceOptionOpaque;
            if (any(in.flags & RayInstanceFlags::NonOpaque))
                o.options |= MTLAccelerationStructureInstanceOptionNonOpaque;
            o.mask = in.mask;
            o.intersectionFunctionTableOffset = 0;
            o.accelerationStructureIndex = indices[i];
            o.userID = in.id;
        }

        MTLInstanceAccelerationStructureDescriptor* d = [MTLInstanceAccelerationStructureDescriptor descriptor];
        d.instancedAccelerationStructures = bottomLevels;
        d.instanceCount = instances.size();
        d.instanceDescriptorBuffer = buffer;
        d.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
        d.usage = as.allowRefit ? MTLAccelerationStructureUsageRefit : MTLAccelerationStructureUsageNone;
        if (refit) {
            const NSUInteger offset = scratch(c, as.refitScratch);
            [c.accel refitAccelerationStructure:as.handle
                                     descriptor:d
                                    destination:nil
                                  scratchBuffer:m_scratch
                            scratchBufferOffset:offset];
            return;
        }
        const NSUInteger offset = scratch(c, as.buildScratch);
        [c.accel buildAccelerationStructure:as.handle descriptor:d scratchBuffer:m_scratch scratchBufferOffset:offset];

        as.bottomLevels = bottomLevels;
        as.instanceBottomLevels = std::move(indices);
        as.residency.clear();
        for (id<MTLAccelerationStructure> bottom in bottomLevels) as.residency.push_back(bottom);
    }
}

void MetalDevice::queryCompactedSize(void* pass, void* structure) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(pass);
        MetalAccelerationStructure& as = *static_cast<MetalAccelerationStructure*>(structure);
        if (!as.sizePage) {
            if (m_sizeSlots.empty()) {
                id<MTLBuffer> page = [m_device newBufferWithLength:metal::kSizePageBytes
                                                           options:MTLResourceStorageModeShared];
                if (!page) {
                    log::error("gpu", "cannot allocate {} bytes for compacted sizes", metal::kSizePageBytes);
                    return;
                }
                if (m_debug) page.label = @"compacted acceleration structure sizes";
                for (NSUInteger offset = metal::kSizePageBytes; offset > 0; offset -= sizeof(std::uint32_t))
                    m_sizeSlots.push_back({page, offset - sizeof(std::uint32_t)});
            }
            as.sizePage = m_sizeSlots.back().page;
            as.sizeOffset = m_sizeSlots.back().offset;
            m_sizeSlots.pop_back();
        }
        if (!afterBottomLevelBuilds(c)) return;
        [c.accel writeCompactedAccelerationStructureSize:as.handle toBuffer:as.sizePage offset:as.sizeOffset];
        auto query = std::make_shared<MetalSizeQuery>();
        query->commands = c.handle;
        as.sizeQuery = query;
        [c.handle addCompletedHandler:^(id<MTLCommandBuffer> commands) {
          if (commands.status == MTLCommandBufferStatusCompleted)
              query->completed.store(true, std::memory_order_release);
        }];
    }
}

void MetalDevice::copyAndCompact(void* pass, void* source, void* destination) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(pass);
        if (!afterBottomLevelBuilds(c)) return;
        [c.accel copyAndCompactAccelerationStructure:static_cast<MetalAccelerationStructure*>(source)->handle
                             toAccelerationStructure:static_cast<MetalAccelerationStructure*>(destination)->handle];
        c.accelBottomBuilt = true; // a top level built after it in the pass uses the copy
    }
}

void MetalDevice::bindAccelerationStructure(void* pass, ShaderStage stage, std::uint32_t slot, void* structure) {
    MetalCommandBuffer& c = commandsOf(pass);
    const std::size_t s = stageIndex(stage);
    c.accelerationStructures[s][slot] = static_cast<const MetalAccelerationStructure*>(structure);
    c.accelerationStructuresDirty[s] = true;
}

// ------------------------------------------------------------------ upscaling

void MetalDevice::upscale(void* commands, void* upscaler, const UpscaleDesc& desc) {
#if QUENCH_PLATFORM_IOS_SIMULATOR
    (void)commands;
    (void)upscaler;
    (void)desc;
#else
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        MetalUpscaler& u = *static_cast<MetalUpscaler*>(upscaler);
        // MetalFX encodes passes of its own: in a timed range, blit encoders before and after them take the start
        // and the end timestamp, each at its own start. Each copies a texel (an empty encoder is dropped, its
        // samples never written): the first one of the input after the passes that drew it, the last one of the
        // output once MetalFX wrote it. (After the temporal scaler, an end-of-encoder sample was not written.)
        const std::int64_t first = timingSamples(c, 2);
        if (first >= 0 && !m_markerBuffer) {
            m_markerBuffer = [m_device newBufferWithLength:64 options:MTLResourceStorageModePrivate];
            if (m_debug) m_markerBuffer.label = @"upscale timestamp texels";
        }
        const auto marker = [&](bool start, const TextureRef& texture) {
            MTLBlitPassDescriptor* d = [MTLBlitPassDescriptor blitPassDescriptor];
            d.sampleBufferAttachments[0].sampleBuffer = c.timing->samples;
            d.sampleBufferAttachments[0].startOfEncoderSampleIndex = NSUInteger(start ? first : first + 1);
            d.sampleBufferAttachments[0].endOfEncoderSampleIndex = MTLCounterDontSample;
            id<MTLBlitCommandEncoder> blit = [c.handle blitCommandEncoderWithDescriptor:d];
            const NSUInteger texel = NSUInteger(levelByteSize(texture.desc.format, 1, 1, 1));
            [blit copyFromTexture:textureOf(texture).handle
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(1, 1, 1)
                                toBuffer:m_markerBuffer
                       destinationOffset:start ? 0 : 32
                  destinationBytesPerRow:texel
                destinationBytesPerImage:texel];
            [blit endEncoding];
        };
        if (u.exposure && u.exposureValue != desc.exposure) {
            const _Float16 value = _Float16(desc.exposure);
            auto [buffer, offset] = uniformSpace(c, sizeof(value));
            if (buffer) {
                std::memcpy(static_cast<std::byte*>(buffer.contents) + offset, &value, sizeof(value));
                id<MTLBlitCommandEncoder> blit = [c.handle blitCommandEncoder];
                [blit copyFromBuffer:buffer
                           sourceOffset:offset
                      sourceBytesPerRow:sizeof(value)
                    sourceBytesPerImage:sizeof(value)
                             sourceSize:MTLSizeMake(1, 1, 1)
                              toTexture:u.exposure
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
                u.exposureValue = desc.exposure;
            }
        }
        if (first >= 0) marker(true, desc.color);
        if (u.spatial) {
            u.spatial.colorTexture = textureOf(desc.color).handle;
            u.spatial.outputTexture = textureOf(desc.output).handle;
            u.spatial.inputContentWidth = desc.color.desc.width;
            u.spatial.inputContentHeight = desc.color.desc.height;
            [u.spatial encodeToCommandBuffer:c.handle];
        } else if (u.temporal) {
            u.temporal.colorTexture = textureOf(desc.color).handle;
            u.temporal.depthTexture = textureOf(desc.depth).handle;
            u.temporal.motionTexture = textureOf(desc.motion).handle;
            u.temporal.outputTexture = textureOf(desc.output).handle;
            u.temporal.inputContentWidth = desc.color.desc.width;
            u.temporal.inputContentHeight = desc.color.desc.height;
            u.temporal.jitterOffsetX = desc.jitterX;
            u.temporal.jitterOffsetY = desc.jitterY;
            u.temporal.motionVectorScaleX = desc.motionScaleX;
            u.temporal.motionVectorScaleY = desc.motionScaleY;
            u.temporal.exposureTexture = u.exposure;
            u.temporal.reset = desc.reset;
            [u.temporal encodeToCommandBuffer:c.handle];
        } else if (u.denoised) {
            if (@available(macOS 26.0, iOS 26.0, *)) {
                id<MTLFXTemporalDenoisedScaler> d = u.denoised;
                d.colorTexture = textureOf(desc.color).handle;
                d.depthTexture = textureOf(desc.depth).handle;
                d.motionTexture = textureOf(desc.motion).handle;
                d.diffuseAlbedoTexture = textureOf(desc.diffuseAlbedo).handle;
                d.specularAlbedoTexture = textureOf(desc.specularAlbedo).handle;
                d.normalTexture = textureOf(desc.normal).handle;
                d.roughnessTexture = textureOf(desc.roughness).handle;
                d.outputTexture = textureOf(desc.output).handle;
                d.exposureTexture = u.exposure;
                d.jitterOffsetX = desc.jitterX;
                d.jitterOffsetY = desc.jitterY;
                d.motionVectorScaleX = desc.motionScaleX;
                d.motionVectorScaleY = desc.motionScaleY;
                simd_float4x4 worldToView, viewToClip;
                static_assert(sizeof(worldToView) == sizeof(desc.worldToView));
                std::memcpy(&worldToView, desc.worldToView.data(), sizeof(worldToView));
                std::memcpy(&viewToClip, desc.viewToClip.data(), sizeof(viewToClip));
                d.worldToViewMatrix = worldToView;
                d.viewToClipMatrix = viewToClip;
                d.shouldResetHistory = desc.reset;
                [d encodeToCommandBuffer:c.handle];
            }
        }
        if (first >= 0) marker(false, desc.output);
    }
#endif
}

// ------------------------------------------------------------------ copy passes

void* MetalDevice::beginCopyPass(void* commands) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        if (const std::int64_t first = timingSamples(c, 2); first >= 0) {
            MTLBlitPassDescriptor* d = [MTLBlitPassDescriptor blitPassDescriptor];
            d.sampleBufferAttachments[0].sampleBuffer = c.timing->samples;
            d.sampleBufferAttachments[0].startOfEncoderSampleIndex = NSUInteger(first);
            d.sampleBufferAttachments[0].endOfEncoderSampleIndex = NSUInteger(first + 1);
            c.blit = [c.handle blitCommandEncoderWithDescriptor:d];
        } else {
            c.blit = [c.handle blitCommandEncoder];
        }
        if (!c.blit) {
            log::error("gpu", "cannot begin a Metal copy pass");
            return nullptr;
        }
        c.pass = MetalPass::Copy;
        return &c;
    }
}

void MetalDevice::endCopyPass(void*, void* pass) {
    MetalCommandBuffer& c = commandsOf(pass);
    [c.blit endEncoding];
    c.resetPassState();
}

void MetalDevice::uploadToBuffer(void* pass, void* transferBuffer, std::uint32_t sourceOffset,
                                 const BufferRef& destination, std::uint32_t destinationOffset, std::uint32_t size,
                                 bool) {
    MetalCommandBuffer& c = commandsOf(pass);
    MetalTransferBuffer& transfer = transferOf(transferBuffer);
    useTransfer(c, transfer);
    [c.blit copyFromBuffer:transfer.current().handle
              sourceOffset:sourceOffset
                  toBuffer:bufferOf(destination)
         destinationOffset:destinationOffset
                      size:size];
}

void MetalDevice::uploadToTexture(void* pass, void* transferBuffer, std::uint32_t sourceOffset,
                                  const TextureRegion& destination, bool) {
    MetalCommandBuffer& c = commandsOf(pass);
    MetalTransferBuffer& transfer = transferOf(transferBuffer);
    useTransfer(c, transfer);
    const std::uint32_t width = regionWidth(destination);
    const std::uint32_t height = regionHeight(destination);
    const Pitch pitch = pitchOf(destination.texture.desc.format, width, height);
    [c.blit copyFromBuffer:transfer.current().handle
               sourceOffset:sourceOffset
          sourceBytesPerRow:pitch.row
        sourceBytesPerImage:pitch.image
                 sourceSize:MTLSizeMake(width, height, destination.depth)
                  toTexture:textureOf(destination.texture).handle
           destinationSlice:destination.layer
           destinationLevel:destination.mipLevel
          destinationOrigin:MTLOriginMake(destination.x, destination.y, destination.z)];
}

void MetalDevice::downloadFromTexture(void* pass, const TextureRegion& source, void* transferBuffer,
                                      std::uint32_t destinationOffset) {
    MetalCommandBuffer& c = commandsOf(pass);
    MetalTransferBuffer& transfer = transferOf(transferBuffer);
    useTransfer(c, transfer);
    const std::uint32_t width = regionWidth(source);
    const std::uint32_t height = regionHeight(source);
    const Pitch pitch = pitchOf(source.texture.desc.format, width, height);
    [c.blit copyFromTexture:textureOf(source.texture).handle
                     sourceSlice:source.layer
                     sourceLevel:source.mipLevel
                    sourceOrigin:MTLOriginMake(source.x, source.y, source.z)
                      sourceSize:MTLSizeMake(width, height, source.depth)
                        toBuffer:transfer.current().handle
               destinationOffset:destinationOffset
          destinationBytesPerRow:pitch.row
        destinationBytesPerImage:pitch.image];
}

void MetalDevice::downloadFromBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset,
                                     std::uint32_t size, void* transferBuffer, std::uint32_t destinationOffset) {
    MetalCommandBuffer& c = commandsOf(pass);
    MetalTransferBuffer& transfer = transferOf(transferBuffer);
    useTransfer(c, transfer);
    [c.blit copyFromBuffer:bufferOf(source)
              sourceOffset:sourceOffset
                  toBuffer:transfer.current().handle
         destinationOffset:destinationOffset
                      size:size];
}

void MetalDevice::copyBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset,
                             const BufferRef& destination, std::uint32_t destinationOffset, std::uint32_t size, bool) {
    [commandsOf(pass).blit copyFromBuffer:bufferOf(source)
                             sourceOffset:sourceOffset
                                 toBuffer:bufferOf(destination)
                        destinationOffset:destinationOffset
                                     size:size];
}

void MetalDevice::copyTexture(void* pass, const TextureRegion& source, const TextureRegion& destination, bool) {
    [commandsOf(pass).blit copyFromTexture:textureOf(source.texture).handle
                               sourceSlice:source.layer
                               sourceLevel:source.mipLevel
                              sourceOrigin:MTLOriginMake(source.x, source.y, source.z)
                                sourceSize:MTLSizeMake(regionWidth(source), regionHeight(source), source.depth)
                                 toTexture:textureOf(destination.texture).handle
                          destinationSlice:destination.layer
                          destinationLevel:destination.mipLevel
                         destinationOrigin:MTLOriginMake(destination.x, destination.y, destination.z)];
}

// ------------------------------------------------------------------ mipmaps and blits

void MetalDevice::generateMipmaps(void* commands, const TextureRef& texture) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        id<MTLBlitCommandEncoder> encoder = nil;
        if (const std::int64_t first = timingSamples(c, 2); first >= 0) {
            MTLBlitPassDescriptor* d = [MTLBlitPassDescriptor blitPassDescriptor];
            d.sampleBufferAttachments[0].sampleBuffer = c.timing->samples;
            d.sampleBufferAttachments[0].startOfEncoderSampleIndex = NSUInteger(first);
            d.sampleBufferAttachments[0].endOfEncoderSampleIndex = NSUInteger(first + 1);
            encoder = [c.handle blitCommandEncoderWithDescriptor:d];
        } else {
            encoder = [c.handle blitCommandEncoder];
        }
        [encoder generateMipmapsForTexture:textureOf(texture).handle];
        [encoder endEncoding];
    }
}

Result<id<MTLRenderPipelineState>> MetalDevice::blitPipeline(TextureFormat destination, bool array) {
    for (const BlitPipeline& p : m_blitPipelines)
        if (p.format == destination && p.array == array) return p.state;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = m_blitVertex;
    d.fragmentFunction = array ? m_blit2dArray : m_blit2d;
    d.colorAttachments[0].pixelFormat = metal::pixelFormat(destination);
    if (m_debug) d.label = @"blit";
    NSError* error = nil;
    id<MTLRenderPipelineState> state = [m_device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!state)
        return Error{std::format("blit pipeline for {}: {}", formatInfo(destination).name,
                                 error ? error.localizedDescription.UTF8String : "unknown error")};
    m_blitPipelines.push_back({destination, array, state});
    return state;
}

void MetalDevice::blit(void* commands, const BlitDesc& blit) {
    @autoreleasepool {
        MetalCommandBuffer& c = commandsOf(commands);
        const TextureRegion& src = blit.source;
        const TextureRegion& dst = blit.destination;
        const TextureType type = src.texture.desc.type;
        if (type != TextureType::Texture2D && type != TextureType::Texture2DArray) {
            log::error("gpu", "blit from a 3D or cube texture: not supported by the native Metal backend");
            return;
        }
        Result<id<MTLRenderPipelineState>> pipeline =
            blitPipeline(dst.texture.desc.format, type == TextureType::Texture2DArray);
        if (!pipeline) {
            log::error("gpu", "{}", pipeline.error());
            return;
        }

        MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassColorAttachmentDescriptor* a = d.colorAttachments[0];
        a.texture = textureOf(dst.texture).handle;
        a.level = dst.mipLevel;
        if (dst.texture.desc.type == TextureType::Texture3D)
            a.depthPlane = dst.layer;
        else
            a.slice = dst.layer;
        a.loadAction = loadAction(blit.load);
        a.storeAction = MTLStoreActionStore;
        a.clearColor = MTLClearColorMake(blit.clear.r, blit.clear.g, blit.clear.b, blit.clear.a);
        if (const std::int64_t first = timingSamples(c, 4); first >= 0) {
            MTLRenderPassSampleBufferAttachmentDescriptor* s = d.sampleBufferAttachments[0];
            s.sampleBuffer = c.timing->samples;
            s.startOfVertexSampleIndex = NSUInteger(first);
            s.endOfVertexSampleIndex = NSUInteger(first + 1);
            s.startOfFragmentSampleIndex = NSUInteger(first + 2);
            s.endOfFragmentSampleIndex = NSUInteger(first + 3);
        }
        id<MTLRenderCommandEncoder> encoder = [c.handle renderCommandEncoderWithDescriptor:d];
        if (!encoder) {
            log::error("gpu", "cannot begin the blit pass");
            return;
        }

        const double sourceWidth = double(src.texture.desc.width >> src.mipLevel);
        const double sourceHeight = double(src.texture.desc.height >> src.mipLevel);
        shader::BlitUniforms uniforms{};
        uniforms.region = {float(src.x / sourceWidth), float(src.y / sourceHeight),
                           float(regionWidth(src) / sourceWidth), float(regionHeight(src) / sourceHeight)};
        uniforms.source = {float(src.mipLevel), float(src.layer), 0.0f, 0.0f};
        [encoder setRenderPipelineState:*pipeline];
        [encoder setViewport:MTLViewport{double(dst.x), double(dst.y), double(regionWidth(dst)),
                                         double(regionHeight(dst)), 0.0, 1.0}];
        [encoder setFragmentTexture:textureOf(src.texture).handle atIndex:0];
        [encoder setFragmentSamplerState:(blit.filter == Filter::Linear ? m_blitLinear : m_blitNearest) atIndex:0];
        [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
    }
}

} // namespace quench::gpu::detail
