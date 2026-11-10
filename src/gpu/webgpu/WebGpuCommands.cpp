#include "gpu/webgpu/WebGpu.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <cstring>

namespace quench::gpu::detail {

namespace {

using wg::sv;

WgCommands& cmd(void* native) { return *static_cast<WgCommands*>(native); }

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// A texture region as WebGPU copies it: compressed formats in whole blocks, rows of blocks.
struct CopyShape {
    WGPUTexelCopyTextureInfo texture = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUExtent3D extent{};
    std::uint32_t rowBytes = 0; // of a tightly packed row
    std::uint32_t rows = 0;     // per image
    std::uint32_t images = 1;   // depth slices
};

CopyShape shapeOf(const TextureRegion& region) {
    const WgTexture& texture = *wg::texture(region.texture);
    const FormatInfo& info = formatInfo(texture.desc.format);
    const bool volume = texture.desc.type == TextureType::Texture3D;
    const std::uint32_t blocksX = (regionWidth(region) + info.blockWidth - 1) / info.blockWidth;
    const std::uint32_t blocksY = (regionHeight(region) + info.blockHeight - 1) / info.blockHeight;
    CopyShape shape;
    shape.texture.texture = texture.texture;
    shape.texture.mipLevel = region.mipLevel;
    shape.texture.origin = {region.x, region.y, volume ? region.z : region.layer};
    shape.texture.aspect = info.depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
    shape.images = volume ? std::max(region.depth, 1u) : 1u;
    shape.extent = {blocksX * info.blockWidth, blocksY * info.blockHeight, shape.images};
    shape.rowBytes = blocksX * info.blockBytes;
    shape.rows = blocksY;
    return shape;
}

// WebGPU wants rows at a multiple of 256 bytes when a copy has more than one.
std::uint32_t pitchOf(const CopyShape& shape) {
    return shape.rows * shape.images > 1 ? alignUp(shape.rowBytes, 256) : shape.rowBytes;
}

// The blit: a triangle over the viewport (the destination's rectangle) that samples a part of the source level.
constexpr std::string_view kBlitShader = R"(
struct Region { offsetScale : vec4f }
@group(0) @binding(0) var source : texture_2d<f32>;
@group(0) @binding(1) var sourceSampler : sampler;
@group(0) @binding(2) var<uniform> region : Region;
struct Out { @builtin(position) position : vec4f, @location(0) uv : vec2f }
@vertex fn vs(@builtin(vertex_index) index : u32) -> Out {
    let p = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    var out : Out;
    out.position = vec4f(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = vec2f(p.x, 1.0 - p.y) * region.offsetScale.zw + region.offsetScale.xy;
    return out;
}
@fragment fn fs(in : Out) -> @location(0) vec4f {
    return textureSampleLevel(source, sourceSampler, in.uv, 0.0);
}
)";

} // namespace

// ------------------------------------------------------------------ command buffers

void* WebGpuDevice::acquireCommandBuffer() {
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(m_device, nullptr);
    if (!encoder) {
        log::error("gpu", "cannot create a WebGPU command encoder");
        return nullptr;
    }
    auto* c = new WgCommands;
    c->encoder = encoder;
    ++m_openCommands;
    return c;
}

void WebGpuDevice::resetPassState(WgCommands& c) {
    c.pipeline = nullptr;
    c.pipelineDirty = true;
    c.boundRender = nullptr;
    c.boundCompute = nullptr;
    c.boundGroups = {};
    c.stages = {};
    c.writes = {};
    c.writesDirty = true;
    for (WgUniformState& u : c.uniforms) u.dirty = true;
}

void* WebGpuDevice::beginRenderPass(void* commands, std::span<const ColorTarget> colors, const DepthTarget* depth) {
    WgCommands& c = cmd(commands);
    std::array<WGPURenderPassColorAttachment, 4> attachments;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        const ColorTarget& in = colors[i];
        WGPURenderPassColorAttachment& a = attachments[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        std::uint32_t slice = WGPU_DEPTH_SLICE_UNDEFINED;
        a.view = targetView(in.texture, in.mipLevel, in.layer, &slice);
        a.depthSlice = slice;
        a.loadOp = in.load == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
        a.clearValue = {in.clear.r, in.clear.g, in.clear.b, in.clear.a};
        a.storeOp = in.store == StoreOp::Store || in.store == StoreOp::ResolveAndStore ? WGPUStoreOp_Store
                                                                                       : WGPUStoreOp_Discard;
        if ((in.store == StoreOp::Resolve || in.store == StoreOp::ResolveAndStore) && in.resolveTexture)
            a.resolveTarget = targetView(in.resolveTexture, in.resolveMipLevel, in.resolveLayer, nullptr);
    }
    WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    if (depth) {
        depthAttachment.view = targetView(depth->texture, depth->mipLevel, depth->layer, nullptr);
        depthAttachment.depthLoadOp = depth->load == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
        depthAttachment.depthStoreOp = depth->store == StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
        depthAttachment.depthClearValue = std::clamp(depth->clearDepth, 0.0f, 1.0f);
        if (formatInfo(depth->texture.desc.format).stencil) {
            depthAttachment.stencilLoadOp = depth->stencilLoad == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
            depthAttachment.stencilStoreOp =
                depth->stencilStore == StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
            depthAttachment.stencilClearValue = depth->clearStencil;
        }
    }
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.colorAttachmentCount = colors.size();
    desc.colorAttachments = attachments.data();
    desc.depthStencilAttachment = depth ? &depthAttachment : nullptr;
    c.render = wgpuCommandEncoderBeginRenderPass(c.encoder, &desc);
    if (!c.render) {
        log::error("gpu", "cannot begin a WebGPU render pass");
        return nullptr;
    }
    const TextureRef& first = !colors.empty() ? colors[0].texture : depth->texture;
    const std::uint32_t mip = !colors.empty() ? colors[0].mipLevel : depth->mipLevel;
    c.targetWidth = mipExtent(first.desc.width, mip);
    c.targetHeight = mipExtent(first.desc.height, mip);
    resetPassState(c);
    return &c;
}

void* WebGpuDevice::beginCopyPass(void* commands) { return commands; }

void* WebGpuDevice::beginComputePass(void* commands, std::span<const StorageBufferWrite> writes) {
    WgCommands& c = cmd(commands);
    c.computePass = wgpuCommandEncoderBeginComputePass(c.encoder, nullptr);
    if (!c.computePass) {
        log::error("gpu", "cannot begin a WebGPU compute pass");
        return nullptr;
    }
    resetPassState(c);
    for (std::size_t i = 0; i < writes.size() && i < kWgStorageSlots; ++i) c.writes[i] = wg::buffer(writes[i].buffer);
    return &c;
}

void WebGpuDevice::endDebugGroups(WgCommands& c) {
    for (; c.pendingPops > 0; --c.pendingPops) wgpuCommandEncoderPopDebugGroup(c.encoder);
}

void WebGpuDevice::endRenderPass(void* commands, void*) {
    WgCommands& c = cmd(commands);
    wgpuRenderPassEncoderEnd(c.render);
    wgpuRenderPassEncoderRelease(c.render);
    c.render = nullptr;
    endDebugGroups(c);
}

void WebGpuDevice::endCopyPass(void*, void*) {}

void WebGpuDevice::endComputePass(void* commands, void*) {
    WgCommands& c = cmd(commands);
    wgpuComputePassEncoderEnd(c.computePass);
    wgpuComputePassEncoderRelease(c.computePass);
    c.computePass = nullptr;
    endDebugGroups(c);
}

void WebGpuDevice::pushDebugGroup(void* commands, const char* name) {
    WgCommands& c = cmd(commands);
    // The encoder takes no commands while one of its passes is open: such a group is left out.
    const bool forwarded = !c.render && !c.computePass;
    if (forwarded) wgpuCommandEncoderPushDebugGroup(c.encoder, sv(name));
    c.debugGroups.push_back(forwarded);
}

void WebGpuDevice::popDebugGroup(void* commands) {
    WgCommands& c = cmd(commands);
    if (c.debugGroups.empty()) return;
    const bool forwarded = c.debugGroups.back();
    c.debugGroups.pop_back();
    if (!forwarded) return;
    if (c.render || c.computePass)
        ++c.pendingPops;
    else
        wgpuCommandEncoderPopDebugGroup(c.encoder);
}

void WebGpuDevice::insertDebugLabel(void* commands, const char* text) {
    WgCommands& c = cmd(commands);
    if (!c.render && !c.computePass) wgpuCommandEncoderInsertDebugMarker(c.encoder, sv(text));
}

// ------------------------------------------------------------------ submission

void WebGpuDevice::closeCommands(WgCommands* c) {
    for (WGPUBuffer page : c->pages) m_freePages.push_back(page);
    for (WGPUBuffer staging : c->staging) {
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
    }
    for (const std::shared_ptr<WgRead>& read : c->reads) read->done = true; // never submitted: nothing to read
    wgpuCommandEncoderRelease(c->encoder);
    delete c;
    QUENCH_ASSERT(m_openCommands > 0, "a command buffer closed twice");
    if (--m_openCommands == 0) {
        for (WGPUTexture texture : m_deadTextures) {
            wgpuTextureDestroy(texture);
            wgpuTextureRelease(texture);
        }
        for (WGPUBuffer buffer : m_deadBuffers) {
            wgpuBufferDestroy(buffer);
            wgpuBufferRelease(buffer);
        }
        m_deadTextures.clear();
        m_deadBuffers.clear();
    }
    if (++m_submissions % 64 == 0) trimBindGroups();
}

// Bind groups of resources that are gone (or no longer drawn with) are dropped by age.
void WebGpuDevice::trimBindGroups() {
    constexpr std::uint64_t kUnusedSubmissions = 240;
    for (auto it = m_bindGroups.begin(); it != m_bindGroups.end();) {
        if (it->second.lastUsed + kUnusedSubmissions < m_submissions) {
            wgpuBindGroupRelease(it->second.group);
            it = m_bindGroups.erase(it);
        } else {
            ++it;
        }
    }
}

Result<void> WebGpuDevice::finish(WgCommands* c, std::shared_ptr<std::uint32_t>* pending) {
    if (c->render || c->computePass) {
        closeCommands(c);
        return Error{"a command buffer was submitted with a pass open"};
    }
    flushPage(*c);
    // Groups left open would make the encoder invalid.
    for (const bool forwarded : c->debugGroups)
        if (forwarded) wgpuCommandEncoderPopDebugGroup(c->encoder);
    WGPUCommandBuffer buffer = wgpuCommandEncoderFinish(c->encoder, nullptr);
    if (!buffer) {
        closeCommands(c);
        return Error{"cannot finish a WebGPU command encoder"};
    }
    wgpuQueueSubmit(m_queue, 1, &buffer);
    wgpuCommandBufferRelease(buffer);
    if (pending) {
        *pending = std::make_shared<std::uint32_t>(1u + std::uint32_t(c->reads.size()));
        WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* user, void*) {
            auto* count = static_cast<std::shared_ptr<std::uint32_t>*>(user);
            --**count;
            delete count;
        };
        info.userdata1 = new std::shared_ptr<std::uint32_t>(*pending); // the fence may be released first
        wgpuQueueOnSubmittedWorkDone(m_queue, info);
    }
    // The downloads are mapped now and read when the browser resolves the map, between frames.
    for (const std::shared_ptr<WgRead>& read : c->reads) {
        read->submitted = true;
        if (pending) read->pending = *pending;
        WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* user, void*) {
            auto* held = static_cast<std::shared_ptr<WgRead>*>(user);
            WgRead& r = **held;
            const void* bytes = status == WGPUMapAsyncStatus_Success
                                    ? wgpuBufferGetConstMappedRange(r.buffer, 0, std::size_t(r.bufferSize))
                                    : nullptr;
            if (bytes) {
                r.bytes.resize(std::size_t(r.rowBytes) * r.rows);
                for (std::uint32_t row = 0; row < r.rows; ++row)
                    std::memcpy(r.bytes.data() + std::size_t(row) * r.rowBytes,
                                static_cast<const std::byte*>(bytes) + std::size_t(row) * r.pitch, r.rowBytes);
                r.ok = true;
            } else if (status == WGPUMapAsyncStatus_Error) {
                // Not the map a closing device aborts or cancels: that is how a pending download ends at the quit.
                log::error("gpu", "cannot map a download of {} bytes", r.bufferSize);
            }
            if (status == WGPUMapAsyncStatus_Success) wgpuBufferUnmap(r.buffer);
            wgpuBufferRelease(r.buffer);
            r.buffer = nullptr;
            r.done = true;
            if (r.pending) --*r.pending;
            r.pending.reset();
            delete held;
        };
        info.userdata1 = new std::shared_ptr<WgRead>(read); // the transfer buffer may be released first
        wgpuBufferMapAsync(read->buffer, WGPUMapMode_Read, 0, std::size_t(read->bufferSize), info);
    }
    c->reads.clear();
    closeCommands(c);
    return {};
}

Result<void> WebGpuDevice::submit(void* commands) { return finish(&cmd(commands), nullptr); }

Result<void*> WebGpuDevice::submitWithFence(void* commands) {
    auto fence = std::make_unique<WgFence>();
    if (Result<void> finished = finish(&cmd(commands), &fence->pending); !finished) return Error{finished.error()};
    return static_cast<void*>(fence.release());
}

void WebGpuDevice::cancel(void* commands) {
    WgCommands& c = cmd(commands);
    if (c.render) {
        wgpuRenderPassEncoderEnd(c.render);
        wgpuRenderPassEncoderRelease(c.render);
    }
    if (c.computePass) {
        wgpuComputePassEncoderEnd(c.computePass);
        wgpuComputePassEncoderRelease(c.computePass);
    }
    closeCommands(&c);
}

// ------------------------------------------------------------------ uniforms

void WebGpuDevice::pushUniforms(void* commands, ShaderStage stage, std::uint32_t slot, const void* data,
                                std::uint32_t size) {
    WgCommands& c = cmd(commands);
    if (slot >= kWgUniformSlots) return;
    if (size > kWgUniformWindow) {
        log::error("gpu", "a uniform block of {} bytes is larger than the {} this backend binds", size,
                   kWgUniformWindow);
        size = kWgUniformWindow;
    }
    WgUniformState& u = c.uniforms[std::size_t(stage)];
    std::vector<std::byte>& bytes = u.data[slot];
    // The same block again keeps its place in the page.
    if (u.page[slot] != ~0u && bytes.size() == size && std::memcmp(bytes.data(), data, size) == 0) return;
    const auto* first = static_cast<const std::byte*>(data);
    bytes.assign(first, first + size);
    u.page[slot] = ~0u;
    u.dirty = true;
}

void WebGpuDevice::flushPage(WgCommands& c) {
    if (!c.pages.empty() && c.pageUsed > 0)
        wgpuQueueWriteBuffer(m_queue, c.pages.back(), 0, c.pageData.data(), c.pageUsed);
}

void WebGpuDevice::newPage(WgCommands& c) {
    flushPage(c);
    WGPUBuffer page = nullptr;
    if (!m_freePages.empty()) {
        page = m_freePages.back();
        m_freePages.pop_back();
    } else {
        WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
        d.label = sv("quench uniforms");
        d.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        d.size = kWgUniformPageSize + kWgUniformWindow; // a binding's window fits at every offset
        page = wgpuDeviceCreateBuffer(m_device, &d);
    }
    c.pages.push_back(page);
    c.pageData.resize(kWgUniformPageSize);
    c.pageUsed = 0;
}

WGPUBuffer WebGpuDevice::placeUniforms(WgCommands& c, const WgStage& stage, ShaderStage which,
                                       std::array<std::uint32_t, kWgUniformSlots>& offsets) {
    WgUniformState& u = c.uniforms[std::size_t(which)];
    const std::uint32_t alignment = std::max(m_limits.minUniformBufferOffsetAlignment, 16u);
    const auto room = [&](std::uint32_t slot) {
        return alignUp(std::max(std::uint32_t(u.data[slot].size()), 16u), alignment);
    };
    // One bind group binds one page: the stage's blocks stay where they are while they are all in the same one.
    std::uint32_t page = ~0u;
    bool together = true;
    for (const std::uint32_t slot : stage.uniforms) {
        if (u.page[slot] == ~0u || (page != ~0u && u.page[slot] != page)) together = false;
        page = u.page[slot];
    }
    if (!together) {
        std::uint32_t current = c.pages.empty() ? ~0u : std::uint32_t(c.pages.size() - 1);
        std::uint32_t needed = 0;
        for (const std::uint32_t slot : stage.uniforms)
            if (current == ~0u || u.page[slot] != current) needed += room(slot);
        if (current == ~0u || c.pageUsed + needed > kWgUniformPageSize) {
            newPage(c);
            current = std::uint32_t(c.pages.size() - 1);
        }
        for (const std::uint32_t slot : stage.uniforms) {
            if (u.page[slot] == current) continue;
            if (!u.data[slot].empty())
                std::memcpy(c.pageData.data() + c.pageUsed, u.data[slot].data(), u.data[slot].size());
            u.offset[slot] = c.pageUsed;
            u.page[slot] = current;
            c.pageUsed += room(slot);
        }
        page = current;
    }
    for (std::size_t i = 0; i < stage.uniforms.size(); ++i) offsets[i] = u.offset[stage.uniforms[i]];
    return c.pages[page];
}

// ------------------------------------------------------------------ bind groups

WGPUBindGroup WebGpuDevice::cachedGroup(std::vector<std::uint64_t>& key, WGPUBindGroupLayout layout,
                                        std::span<const WGPUBindGroupEntry> entries) {
    const std::string text(reinterpret_cast<const char*>(key.data()), key.size() * sizeof(std::uint64_t));
    if (const auto it = m_bindGroups.find(text); it != m_bindGroups.end()) {
        it->second.lastUsed = m_submissions;
        return it->second.group;
    }
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = layout;
    d.entryCount = entries.size();
    d.entries = entries.data();
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(m_device, &d);
    m_bindGroups[text] = {group, m_submissions};
    return group;
}

std::uint32_t WebGpuDevice::unfilterableSlots(const WgStage& stage, const WgStageState& state) {
    std::uint32_t mask = 0;
    for (const WgBinding& b : stage.resources) {
        if (b.kind != WgBindingKind::Texture || b.sampleType != WGPUTextureSampleType_Float) continue;
        const std::uint32_t slot = b.binding / 2;
        const WgTexture* texture = slot < kWgTextureSlots ? state.textures[slot] : nullptr;
        if (texture && !texture->filterable) mask |= 1u << slot;
    }
    return mask;
}

WGPUBindGroup WebGpuDevice::resourceGroup(const WgStage& stage, WGPUBindGroupLayout layout, WgStageState& state,
                                          std::uint32_t unfilterable) {
    std::array<WGPUBindGroupEntry, 2 * kWgTextureSlots + kWgStorageSlots> entries;
    std::size_t count = 0;
    std::vector<std::uint64_t> key;
    key.reserve(stage.resources.size() + 1);
    key.push_back(std::uint64_t(reinterpret_cast<std::uintptr_t>(layout)));
    std::uint32_t storageSlot = 0;
    for (const WgBinding& b : stage.resources) {
        if (count == entries.size()) break;
        WGPUBindGroupEntry& e = entries[count++] = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = b.binding;
        const std::uint32_t slot = b.binding / 2;
        if (b.kind == WgBindingKind::Texture) {
            WgTexture* texture = slot < kWgTextureSlots ? state.textures[slot] : nullptr;
            if (!texture) texture = dummyTexture(b.dimension); // a slot the game left empty
            e.textureView = sampledView(*texture, b.dimension);
            key.push_back(texture->id);
        } else if (b.kind == WgBindingKind::Sampler) {
            const WgSampler* sampler = slot < kWgTextureSlots ? state.samplers[slot] : nullptr;
            const bool nearest = (unfilterable >> slot) & 1u;
            e.sampler = sampler   ? (nearest ? sampler->nearest : sampler->sampler)
                        : nearest ? m_blitSamplers[0]
                                  : m_defaultSampler;
            key.push_back((sampler ? sampler->id : 0) << 1 | std::uint64_t(nearest));
        } else {
            const WgBuffer* buffer = storageSlot < kWgStorageSlots ? state.storage[storageSlot] : nullptr;
            ++storageSlot;
            e.buffer = buffer ? buffer->buffer : m_dummyStorage;
            e.size = WGPU_WHOLE_SIZE;
            key.push_back(buffer ? buffer->id : 0);
        }
    }
    return cachedGroup(key, layout, {entries.data(), count});
}

WGPUBindGroup WebGpuDevice::uniformGroup(WGPUBindGroupLayout layout, const WgStage& stage, WGPUBuffer page) {
    std::array<WGPUBindGroupEntry, kWgUniformSlots> entries;
    for (std::size_t i = 0; i < stage.uniforms.size(); ++i) {
        WGPUBindGroupEntry& e = entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = stage.uniforms[i];
        e.buffer = page;
        e.size = kWgUniformWindow;
    }
    std::vector<std::uint64_t> key{std::uint64_t(reinterpret_cast<std::uintptr_t>(layout)),
                                   std::uint64_t(reinterpret_cast<std::uintptr_t>(page))};
    return cachedGroup(key, layout, {entries.data(), stage.uniforms.size()});
}

void WebGpuDevice::setGroup(WgCommands& c, std::uint32_t index, WGPUBindGroup group,
                            std::span<const std::uint32_t> offsets) {
    std::array<std::uint32_t, kWgUniformSlots> padded{};
    std::copy(offsets.begin(), offsets.end(), padded.begin());
    if (c.boundGroups[index] == group && c.boundOffsets[index] == padded) return;
    c.boundGroups[index] = group;
    c.boundOffsets[index] = padded;
    if (c.render)
        wgpuRenderPassEncoderSetBindGroup(c.render, index, group, offsets.size(), offsets.data());
    else
        wgpuComputePassEncoderSetBindGroup(c.computePass, index, group, offsets.size(), offsets.data());
}

void WebGpuDevice::bindUniforms(WgCommands& c, std::uint32_t index, const WgStage& stage, ShaderStage which,
                                WGPUBindGroupLayout layout, bool pipelineChanged) {
    WgUniformState& u = c.uniforms[std::size_t(which)];
    if (!pipelineChanged && !u.dirty) return;
    u.dirty = false;
    std::array<std::uint32_t, kWgUniformSlots> offsets{};
    // A stage without uniforms binds the (shared) empty group.
    WGPUBuffer page = stage.uniforms.empty() ? nullptr : placeUniforms(c, stage, which, offsets);
    setGroup(c, index, uniformGroup(layout, stage, page), {offsets.data(), stage.uniforms.size()});
}

bool WebGpuDevice::prepareDraw(WgCommands& c) {
    WgPipeline* p = c.pipeline;
    if (!p || !c.render || p->compute) return false;
    const bool pipelineChanged = c.pipelineDirty;
    for (std::size_t i = 0; i < 2; ++i)
        if (pipelineChanged || c.stages[i].resourcesDirty)
            c.unfilterable[i] = unfilterableSlots(p->stages[i], c.stages[i]);
    const WgPipeline::Variant* v = variant(*p, c.unfilterable);
    if (!v) return false;
    if (v->pipeline != c.boundRender) {
        wgpuRenderPassEncoderSetPipeline(c.render, v->pipeline);
        c.boundRender = v->pipeline;
    }
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (pipelineChanged || c.stages[i].resourcesDirty) {
            setGroup(c, i * 2, resourceGroup(p->stages[i], v->resourceLayouts[i], c.stages[i], c.unfilterable[i]), {});
            c.stages[i].resourcesDirty = false;
        }
        bindUniforms(c, i * 2 + 1, p->stages[i], i == 0 ? ShaderStage::Vertex : ShaderStage::Fragment,
                     p->uniformLayouts[i], pipelineChanged);
    }
    c.pipelineDirty = false;
    return true;
}

bool WebGpuDevice::prepareDispatch(WgCommands& c) {
    WgPipeline* p = c.pipeline;
    if (!p || !c.computePass || !p->compute) return false;
    const bool pipelineChanged = c.pipelineDirty;
    const WgStage& stage = p->stages[0];
    if (p->computePipeline != c.boundCompute) {
        wgpuComputePassEncoderSetPipeline(c.computePass, p->computePipeline);
        c.boundCompute = p->computePipeline;
    }
    if (pipelineChanged || c.stages[0].resourcesDirty) {
        setGroup(c, 0, resourceGroup(stage, p->readLayout, c.stages[0], 0), {});
        c.stages[0].resourcesDirty = false;
    }
    if (pipelineChanged || c.writesDirty) {
        std::array<WGPUBindGroupEntry, kWgStorageSlots> entries;
        std::size_t count = 0;
        std::vector<std::uint64_t> key{std::uint64_t(reinterpret_cast<std::uintptr_t>(p->writeLayout))};
        for (const WgBinding& b : stage.writes) {
            if (count == entries.size()) break;
            const WgBuffer* buffer = b.binding < kWgStorageSlots ? c.writes[b.binding] : nullptr;
            if (!buffer) {
                log::error("gpu", "compute pipeline '{}' writes storage buffer {}, which the pass does not have",
                           p->name, b.binding);
                return false;
            }
            WGPUBindGroupEntry& e = entries[count++] = WGPU_BIND_GROUP_ENTRY_INIT;
            e.binding = b.binding;
            e.buffer = buffer->buffer;
            e.size = WGPU_WHOLE_SIZE;
            key.push_back(buffer->id);
        }
        setGroup(c, 1, cachedGroup(key, p->writeLayout, {entries.data(), count}), {});
        c.writesDirty = false;
    }
    bindUniforms(c, 2, stage, ShaderStage::Compute, p->uniformLayouts[0], pipelineChanged);
    c.pipelineDirty = false;
    return true;
}

// ------------------------------------------------------------------ render passes

void WebGpuDevice::bindPipeline(void* pass, void* pipeline) {
    WgCommands& c = cmd(pass);
    if (c.pipeline == pipeline) return;
    c.pipeline = static_cast<WgPipeline*>(pipeline);
    c.pipelineDirty = true;
}

void WebGpuDevice::setViewport(void* pass, const Viewport& viewport) {
    WgCommands& c = cmd(pass);
    // WebGPU takes a viewport inside the target only.
    const float width = float(c.targetWidth), height = float(c.targetHeight);
    const float x0 = std::clamp(viewport.x, 0.0f, width), y0 = std::clamp(viewport.y, 0.0f, height);
    const float x1 = std::clamp(viewport.x + viewport.width, x0, width);
    const float y1 = std::clamp(viewport.y + viewport.height, y0, height);
    const float minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
    wgpuRenderPassEncoderSetViewport(c.render, x0, y0, x1 - x0, y1 - y0, minDepth,
                                     std::clamp(viewport.maxDepth, minDepth, 1.0f));
}

void WebGpuDevice::setScissor(void* pass, const Rect& scissor) {
    WgCommands& c = cmd(pass);
    const std::int64_t width = c.targetWidth, height = c.targetHeight;
    const std::int64_t x0 = std::clamp<std::int64_t>(scissor.x, 0, width);
    const std::int64_t y0 = std::clamp<std::int64_t>(scissor.y, 0, height);
    const std::int64_t x1 = std::clamp<std::int64_t>(std::int64_t(scissor.x) + scissor.width, x0, width);
    const std::int64_t y1 = std::clamp<std::int64_t>(std::int64_t(scissor.y) + scissor.height, y0, height);
    wgpuRenderPassEncoderSetScissorRect(c.render, std::uint32_t(x0), std::uint32_t(y0), std::uint32_t(x1 - x0),
                                        std::uint32_t(y1 - y0));
}

void WebGpuDevice::setBlendConstants(void* pass, const Color& color) {
    const WGPUColor constant{color.r, color.g, color.b, color.a};
    wgpuRenderPassEncoderSetBlendConstant(cmd(pass).render, &constant);
}

void WebGpuDevice::bindVertexBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferBinding> buffers) {
    WgCommands& c = cmd(pass);
    for (std::size_t i = 0; i < buffers.size(); ++i)
        wgpuRenderPassEncoderSetVertexBuffer(c.render, firstSlot + std::uint32_t(i),
                                             wg::buffer(buffers[i].buffer)->buffer, buffers[i].offset, WGPU_WHOLE_SIZE);
}

void WebGpuDevice::bindIndexBuffer(void* pass, const BufferBinding& buffer, IndexFormat format) {
    wgpuRenderPassEncoderSetIndexBuffer(cmd(pass).render, wg::buffer(buffer.buffer)->buffer,
                                        format == IndexFormat::Uint16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
                                        buffer.offset, WGPU_WHOLE_SIZE);
}

void WebGpuDevice::bindSamplers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                std::span<const TextureSamplerBinding> bindings) {
    WgStageState& state = cmd(pass).stages[stage == ShaderStage::Fragment ? 1 : 0];
    for (std::size_t i = 0; i < bindings.size() && firstSlot + i < kWgTextureSlots; ++i) {
        WgTexture* texture = wg::texture(bindings[i].texture);
        WgSampler* sampler = wg::sampler(bindings[i].sampler);
        if (state.textures[firstSlot + i] == texture && state.samplers[firstSlot + i] == sampler) continue;
        state.textures[firstSlot + i] = texture;
        state.samplers[firstSlot + i] = sampler;
        state.resourcesDirty = true;
    }
}

void WebGpuDevice::bindStorageBuffers(void* pass, ShaderStage stage, std::uint32_t firstSlot,
                                      std::span<const BufferRef> buffers) {
    WgStageState& state = cmd(pass).stages[stage == ShaderStage::Fragment ? 1 : 0];
    for (std::size_t i = 0; i < buffers.size() && firstSlot + i < kWgStorageSlots; ++i) {
        WgBuffer* buffer = wg::buffer(buffers[i]);
        if (state.storage[firstSlot + i] == buffer) continue;
        state.storage[firstSlot + i] = buffer;
        state.resourcesDirty = true;
    }
}

void WebGpuDevice::bindStorageTextures(void*, ShaderStage, std::uint32_t, std::span<const TextureRef>) {
    static bool logged = false;
    if (!logged) log::error("gpu", "storage textures are not supported by the WebGPU backend");
    logged = true;
}

void WebGpuDevice::draw(void* pass, std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex,
                        std::uint32_t firstInstance) {
    WgCommands& c = cmd(pass);
    if (prepareDraw(c)) wgpuRenderPassEncoderDraw(c.render, vertexCount, instanceCount, firstVertex, firstInstance);
}

void WebGpuDevice::drawIndexed(void* pass, std::uint32_t indexCount, std::uint32_t instanceCount,
                               std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance) {
    WgCommands& c = cmd(pass);
    if (prepareDraw(c))
        wgpuRenderPassEncoderDrawIndexed(c.render, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

// WebGPU draws one indirect command per call; the commands have Vulkan's layout (16 and 20 bytes).
void WebGpuDevice::drawIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset, std::uint32_t drawCount) {
    WgCommands& c = cmd(pass);
    if (!prepareDraw(c)) return;
    for (std::uint32_t i = 0; i < drawCount; ++i)
        wgpuRenderPassEncoderDrawIndirect(c.render, wg::buffer(buffer)->buffer, offset + std::uint64_t(i) * 16);
}

void WebGpuDevice::drawIndexedIndirect(void* pass, const BufferRef& buffer, std::uint32_t offset,
                                       std::uint32_t drawCount) {
    WgCommands& c = cmd(pass);
    if (!prepareDraw(c)) return;
    for (std::uint32_t i = 0; i < drawCount; ++i)
        wgpuRenderPassEncoderDrawIndexedIndirect(c.render, wg::buffer(buffer)->buffer, offset + std::uint64_t(i) * 20);
}

// ------------------------------------------------------------------ compute passes

void WebGpuDevice::bindComputePipeline(void* pass, void* pipeline) { bindPipeline(pass, pipeline); }

void WebGpuDevice::bindComputeStorageBuffers(void* pass, std::uint32_t firstSlot, std::span<const BufferRef> buffers) {
    bindStorageBuffers(pass, ShaderStage::Vertex, firstSlot, buffers);
}

void WebGpuDevice::dispatch(void* pass, std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) {
    WgCommands& c = cmd(pass);
    if (prepareDispatch(c)) wgpuComputePassEncoderDispatchWorkgroups(c.computePass, groupsX, groupsY, groupsZ);
}

// ------------------------------------------------------------------ copy passes

WGPUBuffer WebGpuDevice::stagingBuffer(WgCommands& c, const std::byte* data, std::uint64_t size) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.usage = WGPUBufferUsage_CopySrc;
    d.size = size;
    d.mappedAtCreation = true;
    WGPUBuffer buffer = wgpuDeviceCreateBuffer(m_device, &d);
    if (!buffer) {
        log::error("gpu", "cannot create a staging buffer of {} bytes", size);
        return nullptr;
    }
    wgpuBufferWriteMappedRange(buffer, 0, data, std::size_t(size));
    wgpuBufferUnmap(buffer);
    c.staging.push_back(buffer);
    return buffer;
}

namespace {

// WebGPU copies buffers in multiples of 4 bytes, at offsets that are multiples of 4.
bool copyRange(const char* what, const WgBuffer& buffer, std::uint32_t offset, std::uint32_t size,
               std::uint32_t& padded) {
    padded = std::min((size + 3u) & ~3u, buffer.size - std::min(buffer.size, offset));
    if (offset % 4 == 0 && padded >= size) return true;
    static bool logged = false;
    if (!logged) log::error("gpu", "{} of {} bytes at offset {}: WebGPU copies at multiples of 4", what, size, offset);
    logged = true;
    return false;
}

} // namespace

void WebGpuDevice::uploadToBuffer(void* pass, void* transferBuffer, std::uint32_t sourceOffset,
                                  const BufferRef& destination, std::uint32_t destinationOffset, std::uint32_t size,
                                  bool) {
    WgCommands& c = cmd(pass);
    const WgTransfer& transfer = *static_cast<WgTransfer*>(transferBuffer);
    const WgBuffer& buffer = *wg::buffer(destination);
    std::uint32_t padded = 0;
    if (size == 0 || std::uint64_t(sourceOffset) + size > transfer.data.size() ||
        !copyRange("a buffer upload", buffer, destinationOffset, size, padded))
        return;
    const std::byte* source = transfer.data.data() + sourceOffset;
    if (padded != size) {
        // WebGPU has no way to write fewer: what follows in the buffer must be free or uploaded afterwards (16-bit
        // indices of an odd count are the usual case).
        static bool logged = false;
        if (!logged && destinationOffset + padded < destination.desc.size) {
            log::debug("gpu",
                       "a buffer upload of {} bytes at offset {} inside a buffer of {}: the bytes up to the "
                       "next multiple of 4 are zeroed (logged once)",
                       size, destinationOffset, destination.desc.size);
            logged = true;
        }
        m_scratch.assign(padded, std::byte{0});
        std::memcpy(m_scratch.data(), source, size);
        source = m_scratch.data();
    }
    if (WGPUBuffer staging = stagingBuffer(c, source, padded))
        wgpuCommandEncoderCopyBufferToBuffer(c.encoder, staging, 0, buffer.buffer, destinationOffset, padded);
}

void WebGpuDevice::uploadToTexture(void* pass, void* transferBuffer, std::uint32_t sourceOffset,
                                   const TextureRegion& destination, bool) {
    WgCommands& c = cmd(pass);
    const WgTransfer& transfer = *static_cast<WgTransfer*>(transferBuffer);
    const CopyShape shape = shapeOf(destination);
    const std::uint32_t rows = shape.rows * shape.images, pitch = pitchOf(shape);
    const std::uint64_t packed = std::uint64_t(shape.rowBytes) * rows;
    if (packed == 0 || sourceOffset + packed > transfer.data.size()) {
        log::error("gpu", "a texture upload of {} bytes from a transfer buffer of {}", packed, transfer.data.size());
        return;
    }
    const std::byte* source = transfer.data.data() + sourceOffset;
    std::uint64_t size = std::uint64_t(pitch) * rows;
    if (pitch != shape.rowBytes || size % 4 != 0) {
        size = (size + 3) & ~std::uint64_t(3);
        m_scratch.assign(std::size_t(size), std::byte{0});
        for (std::uint32_t row = 0; row < rows; ++row)
            std::memcpy(m_scratch.data() + std::size_t(row) * pitch, source + std::size_t(row) * shape.rowBytes,
                        shape.rowBytes);
        source = m_scratch.data();
    }
    WGPUBuffer staging = stagingBuffer(c, source, size);
    if (!staging) return;
    WGPUTexelCopyBufferInfo from = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    from.buffer = staging;
    if (rows > 1) { // one row is copied without a pitch (it would have to be a multiple of 256 too)
        from.layout.bytesPerRow = pitch;
        from.layout.rowsPerImage = shape.rows;
    }
    wgpuCommandEncoderCopyBufferToTexture(c.encoder, &from, &shape.texture, &shape.extent);
}

void WebGpuDevice::downloadFromTexture(void* pass, const TextureRegion& source, void* transferBuffer,
                                       std::uint32_t destinationOffset) {
    WgCommands& c = cmd(pass);
    WgTransfer& transfer = *static_cast<WgTransfer*>(transferBuffer);
    const CopyShape shape = shapeOf(source);
    auto read = std::make_shared<WgRead>();
    read->offset = destinationOffset;
    read->rowBytes = shape.rowBytes;
    read->rows = shape.rows * shape.images;
    read->pitch = pitchOf(shape);
    read->bufferSize = (std::uint64_t(read->pitch) * read->rows + 3) & ~std::uint64_t(3);
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    d.size = read->bufferSize;
    read->buffer = wgpuDeviceCreateBuffer(m_device, &d);
    if (!read->buffer) return;
    WGPUTexelCopyBufferInfo to = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    to.buffer = read->buffer;
    if (read->rows > 1) {
        to.layout.bytesPerRow = read->pitch;
        to.layout.rowsPerImage = shape.rows;
    }
    wgpuCommandEncoderCopyTextureToBuffer(c.encoder, &shape.texture, &to, &shape.extent);
    c.reads.push_back(read);
    transfer.reads.push_back(std::move(read));
}

void WebGpuDevice::downloadFromBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset,
                                      std::uint32_t size, void* transferBuffer, std::uint32_t destinationOffset) {
    WgCommands& c = cmd(pass);
    WgTransfer& transfer = *static_cast<WgTransfer*>(transferBuffer);
    std::uint32_t padded = 0;
    if (size == 0 || !copyRange("a buffer download", *wg::buffer(source), sourceOffset, size, padded)) return;
    auto read = std::make_shared<WgRead>();
    read->offset = destinationOffset;
    read->rowBytes = size;
    read->pitch = size;
    read->rows = 1;
    read->bufferSize = padded;
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    d.size = padded;
    read->buffer = wgpuDeviceCreateBuffer(m_device, &d);
    if (!read->buffer) return;
    wgpuCommandEncoderCopyBufferToBuffer(c.encoder, wg::buffer(source)->buffer, sourceOffset, read->buffer, 0, padded);
    c.reads.push_back(read);
    transfer.reads.push_back(std::move(read));
}

void WebGpuDevice::copyBuffer(void* pass, const BufferRef& source, std::uint32_t sourceOffset,
                              const BufferRef& destination, std::uint32_t destinationOffset, std::uint32_t size, bool) {
    WgCommands& c = cmd(pass);
    std::uint32_t fromSize = 0, toSize = 0;
    if (size == 0 || !copyRange("a buffer copy", *wg::buffer(source), sourceOffset, size, fromSize) ||
        !copyRange("a buffer copy", *wg::buffer(destination), destinationOffset, size, toSize))
        return;
    wgpuCommandEncoderCopyBufferToBuffer(c.encoder, wg::buffer(source)->buffer, sourceOffset,
                                         wg::buffer(destination)->buffer, destinationOffset,
                                         std::min(fromSize, toSize));
}

void WebGpuDevice::copyTexture(void* pass, const TextureRegion& source, const TextureRegion& destination, bool) {
    WgCommands& c = cmd(pass);
    const CopyShape from = shapeOf(source);
    TextureRegion sized = destination; // the destination takes the source's size
    sized.width = regionWidth(source);
    sized.height = regionHeight(source);
    sized.depth = source.depth;
    CopyShape to = shapeOf(sized);
    // A layer of an array into a slice of a 3D texture (and back) is one image either way.
    WGPUExtent3D extent = from.extent;
    extent.depthOrArrayLayers = std::min(from.extent.depthOrArrayLayers, std::max(to.extent.depthOrArrayLayers, 1u));
    wgpuCommandEncoderCopyTextureToTexture(c.encoder, &from.texture, &to.texture, &extent);
}

// ------------------------------------------------------------------ blits

WGPURenderPipeline WebGpuDevice::blitPipeline(WGPUTextureFormat format, bool unfilterable) {
    const std::uint32_t key = std::uint32_t(format) | std::uint32_t(unfilterable) << 31;
    if (const auto it = m_blitPipelines.find(key); it != m_blitPipelines.end()) return it->second;
    if (!m_blitModule) {
        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
        wgsl.code = sv(kBlitShader);
        WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        d.nextInChain = &wgsl.chain;
        d.label = sv("quench blit");
        m_blitModule = wgpuDeviceCreateShaderModule(m_device, &d);
    }
    WGPUBindGroupLayout& layout = m_blitLayouts[unfilterable ? 1 : 0];
    if (!layout) {
        std::array<WGPUBindGroupLayoutEntry, 3> entries;
        entries.fill(WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT);
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Fragment;
        entries[0].texture.sampleType =
            unfilterable ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
        entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].sampler.type = unfilterable ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Vertex;
        entries[2].buffer.type = WGPUBufferBindingType_Uniform;
        entries[2].buffer.hasDynamicOffset = true;
        entries[2].buffer.minBindingSize = 16;
        WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        d.entryCount = entries.size();
        d.entries = entries.data();
        layout = wgpuDeviceCreateBindGroupLayout(m_device, &d);
    }
    WGPUPipelineLayoutDescriptor layoutDesc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layoutDesc.bindGroupLayoutCount = 1;
    layoutDesc.bindGroupLayouts = &layout;
    WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(m_device, &layoutDesc);
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = m_blitModule;
    fragment.entryPoint = sv("fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.label = sv("quench blit");
    d.layout = pipelineLayout;
    d.vertex.module = m_blitModule;
    d.vertex.entryPoint = sv("vs");
    d.fragment = &fragment;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(m_device, &d);
    wgpuPipelineLayoutRelease(pipelineLayout);
    return m_blitPipelines[key] = pipeline;
}

void WebGpuDevice::blitPass(WgCommands& c, WgTexture& source, std::uint32_t sourceMip, std::uint32_t sourceLayer,
                            const std::array<float, 4>& sourceRegion, const TextureRef& destination, std::uint32_t mip,
                            std::uint32_t layer, const Rect& rect, LoadOp load, const Color& clear, Filter filter) {
    const WgTexture& target = *wg::texture(destination);
    if (source.desc.type == TextureType::Texture3D || source.desc.sampleCount > 1 ||
        formatInfo(target.desc.format).depth) {
        static bool logged = false;
        if (!logged) log::error("gpu", "the WebGPU backend blits single-sample 2D color levels only");
        logged = true;
        return;
    }
    const bool unfilterable = !source.filterable;
    WGPURenderPipeline pipeline = blitPipeline(target.format, unfilterable);
    if (!pipeline) return;

    // The region goes into the command buffer's uniform page.
    if (c.pages.empty() || c.pageUsed + 256 > kWgUniformPageSize) newPage(c);
    const std::uint32_t offset = c.pageUsed;
    std::memcpy(c.pageData.data() + offset, sourceRegion.data(), sizeof(float) * 4);
    c.pageUsed += alignUp(16, std::max(m_limits.minUniformBufferOffsetAlignment, 16u));

    std::array<WGPUBindGroupEntry, 3> entries;
    entries.fill(WGPU_BIND_GROUP_ENTRY_INIT);
    entries[0].binding = 0;
    entries[0].textureView = view(source, WGPUTextureViewDimension_2D, sourceMip, 1, sourceLayer, 1, true);
    entries[1].binding = 1;
    entries[1].sampler = m_blitSamplers[!unfilterable && filter == Filter::Linear ? 1 : 0];
    entries[2].binding = 2;
    entries[2].buffer = c.pages.back();
    entries[2].size = 16;
    WGPUBindGroupDescriptor groupDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    groupDesc.layout = m_blitLayouts[unfilterable ? 1 : 0];
    groupDesc.entryCount = entries.size();
    groupDesc.entries = entries.data();
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(m_device, &groupDesc);

    WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    std::uint32_t slice = WGPU_DEPTH_SLICE_UNDEFINED;
    attachment.view = targetView(destination, mip, layer, &slice);
    attachment.depthSlice = slice;
    attachment.loadOp = load == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
    attachment.clearValue = {clear.r, clear.g, clear.b, clear.a};
    attachment.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &attachment;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(c.encoder, &passDesc);
    const std::int32_t width = std::int32_t(mipExtent(target.desc.width, mip));
    const std::int32_t height = std::int32_t(mipExtent(target.desc.height, mip));
    const std::int32_t x0 = std::clamp(rect.x, 0, width), y0 = std::clamp(rect.y, 0, height);
    const std::int32_t x1 = std::clamp(rect.x + rect.width, x0, width);
    const std::int32_t y1 = std::clamp(rect.y + rect.height, y0, height);
    if (x1 > x0 && y1 > y0) {
        // The viewport is the whole rectangle (the source maps onto it), the scissor what is left of it in the target.
        wgpuRenderPassEncoderSetViewport(pass, float(x0), float(y0), float(x1 - x0), float(y1 - y0), 0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(pass, std::uint32_t(x0), std::uint32_t(y0), std::uint32_t(x1 - x0),
                                            std::uint32_t(y1 - y0));
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 1, &offset);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

void WebGpuDevice::blit(void* commands, const BlitDesc& blit) {
    WgCommands& c = cmd(commands);
    WgTexture& source = *wg::texture(blit.source.texture);
    const float width = float(mipExtent(source.desc.width, blit.source.mipLevel));
    const float height = float(mipExtent(source.desc.height, blit.source.mipLevel));
    const std::array<float, 4> region{float(blit.source.x) / width, float(blit.source.y) / height,
                                      float(regionWidth(blit.source)) / width,
                                      float(regionHeight(blit.source)) / height};
    const Rect rect{std::int32_t(blit.destination.x), std::int32_t(blit.destination.y),
                    std::int32_t(regionWidth(blit.destination)), std::int32_t(regionHeight(blit.destination))};
    blitPass(c, source, blit.source.mipLevel, blit.source.layer, region, blit.destination.texture,
             blit.destination.mipLevel, blit.destination.layer, rect, blit.load, blit.clear, blit.filter);
}

void WebGpuDevice::generateMipmaps(void* commands, const TextureRef& texture) {
    WgCommands& c = cmd(commands);
    WgTexture& t = *wg::texture(texture);
    const TextureDesc& desc = t.desc;
    const std::uint32_t layers = desc.type == TextureType::Texture2D ? 1u : desc.layers;
    for (std::uint32_t level = 1; level < desc.mipLevels; ++level)
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            const Rect rect{0, 0, std::int32_t(mipExtent(desc.width, level)),
                            std::int32_t(mipExtent(desc.height, level))};
            blitPass(c, t, level - 1, layer, {0.0f, 0.0f, 1.0f, 1.0f}, texture, level, layer, rect, LoadOp::DontCare,
                     {}, Filter::Linear);
        }
}

} // namespace quench::gpu::detail
