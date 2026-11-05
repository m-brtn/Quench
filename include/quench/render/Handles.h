#pragma once

// Plain handles to what a render::Scene or a render::Renderer holds: an index and a generation. A handle to a removed
// object is detected (the generation no longer matches) instead of addressing whatever took its place. A
// default-constructed handle is null.

#include <cstdint>

namespace quench::render {

template <class Tag>
struct Handle {
    std::uint32_t index = 0;
    std::uint32_t generation = 0; // 0: null

    explicit operator bool() const { return generation != 0; }
    friend bool operator==(Handle, Handle) = default;
};

using MeshId = Handle<struct MeshTag>;
using TextureId = Handle<struct TextureTag>;
using MaterialId = Handle<struct MaterialTag>;
using InstanceId = Handle<struct InstanceTag>;
using LightId = Handle<struct LightTag>;
using SurfaceId = Handle<struct SurfaceTag>;
using PassId = Handle<struct PassTag>;
using ModelInstanceId = Handle<struct ModelInstanceTag>; // render::SceneAssets

} // namespace quench::render
