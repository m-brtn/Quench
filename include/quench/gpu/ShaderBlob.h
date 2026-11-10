#pragma once

// Compiled shaders embedded at build time by quench_add_shaders() (cmake/QuenchShaders.cmake): one table per shader
// set, one row per entry point. A format that was not built for this platform has data == nullptr.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace quench {

enum class ShaderStage : std::uint8_t { Vertex, Fragment, Compute };

struct ShaderCode {
    const std::uint8_t* data;
    std::size_t size;

    explicit operator bool() const { return data != nullptr; }
};

// Resource counts read from the compiled shader, in the order SDL GPU asks for them when a shader is created. The
// storage counts of a compute shader are its read-only ones; the read-write ones and the thread group size exist
// for compute shaders only. Acceleration structures exist in ray query entries only (gpu::Caps::rayTracing).
struct ShaderResources {
    std::uint32_t samplers;
    std::uint32_t storageTextures;
    std::uint32_t storageBuffers;
    std::uint32_t uniformBuffers;
    std::uint32_t readWriteStorageTextures;
    std::uint32_t readWriteStorageBuffers;
    std::uint32_t threadCount[3]; // [numthreads(x, y, z)]
    std::uint32_t accelerationStructures;
};

struct ShaderBlob {
    const char* entry;
    ShaderStage stage;
    ShaderResources resources;
    ShaderCode spirv;    // Vulkan
    ShaderCode msl;      // Metal source, only when the Metal toolchain was missing at build time
    ShaderCode metallib; // Metal, precompiled
    ShaderCode dxil;     // D3D12, Windows builds
    ShaderCode wgsl;     // WebGPU source, web builds (which embed nothing else)
    // The set's manifest for shader hot reload, relative to the build directory (gpu::Device::enableShaderHotReload);
    // nullptr in cross builds and with multi-configuration generators.
    const char* manifest;
};

struct ShaderTable {
    const ShaderBlob* blobs;
    std::size_t count;

    const ShaderBlob* find(std::string_view entry) const {
        for (std::size_t i = 0; i < count; ++i)
            if (entry == blobs[i].entry) return &blobs[i];
        return nullptr;
    }
    const ShaderBlob* begin() const { return blobs; }
    const ShaderBlob* end() const { return blobs + count; }
};

} // namespace quench
