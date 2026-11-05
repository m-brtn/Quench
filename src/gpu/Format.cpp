#include <quench/core/Assert.h>
#include <quench/gpu/Device.h>
#include <quench/gpu/Types.h>

#include <algorithm>
#include <array>
#include <bit>

namespace quench::gpu {
namespace {

using F = TextureFormat;

struct Row {
    F format;
    FormatInfo info;
};

// name, block width, block height, block bytes, depth, stencil, sRGB
constexpr std::array<Row, std::size_t(F::Count)> kFormats{{
    {F::Invalid, {"Invalid", 1, 1, 0, false, false, false}},
    {F::R8Unorm, {"R8Unorm", 1, 1, 1, false, false, false}},
    {F::Rg8Unorm, {"Rg8Unorm", 1, 1, 2, false, false, false}},
    {F::Rgba8Unorm, {"Rgba8Unorm", 1, 1, 4, false, false, false}},
    {F::Rgba8UnormSrgb, {"Rgba8UnormSrgb", 1, 1, 4, false, false, true}},
    {F::Bgra8Unorm, {"Bgra8Unorm", 1, 1, 4, false, false, false}},
    {F::Bgra8UnormSrgb, {"Bgra8UnormSrgb", 1, 1, 4, false, false, true}},
    {F::Rgb10a2Unorm, {"Rgb10a2Unorm", 1, 1, 4, false, false, false}},
    {F::R16Float, {"R16Float", 1, 1, 2, false, false, false}},
    {F::Rg16Float, {"Rg16Float", 1, 1, 4, false, false, false}},
    {F::Rgba16Float, {"Rgba16Float", 1, 1, 8, false, false, false}},
    {F::R32Float, {"R32Float", 1, 1, 4, false, false, false}},
    {F::Rg32Float, {"Rg32Float", 1, 1, 8, false, false, false}},
    {F::Rgba32Float, {"Rgba32Float", 1, 1, 16, false, false, false}},
    {F::Rg11b10Ufloat, {"Rg11b10Ufloat", 1, 1, 4, false, false, false}},
    {F::R32Uint, {"R32Uint", 1, 1, 4, false, false, false}},
    {F::Bc1RgbaUnorm, {"Bc1RgbaUnorm", 4, 4, 8, false, false, false}},
    {F::Bc1RgbaUnormSrgb, {"Bc1RgbaUnormSrgb", 4, 4, 8, false, false, true}},
    {F::Bc3RgbaUnorm, {"Bc3RgbaUnorm", 4, 4, 16, false, false, false}},
    {F::Bc3RgbaUnormSrgb, {"Bc3RgbaUnormSrgb", 4, 4, 16, false, false, true}},
    {F::Bc4RUnorm, {"Bc4RUnorm", 4, 4, 8, false, false, false}},
    {F::Bc5RgUnorm, {"Bc5RgUnorm", 4, 4, 16, false, false, false}},
    {F::Bc6hRgbUfloat, {"Bc6hRgbUfloat", 4, 4, 16, false, false, false}},
    {F::Bc7RgbaUnorm, {"Bc7RgbaUnorm", 4, 4, 16, false, false, false}},
    {F::Bc7RgbaUnormSrgb, {"Bc7RgbaUnormSrgb", 4, 4, 16, false, false, true}},
    {F::Astc4x4Unorm, {"Astc4x4Unorm", 4, 4, 16, false, false, false}},
    {F::Astc4x4UnormSrgb, {"Astc4x4UnormSrgb", 4, 4, 16, false, false, true}},
    {F::Depth16Unorm, {"Depth16Unorm", 1, 1, 2, true, false, false}},
    {F::Depth24UnormStencil8, {"Depth24UnormStencil8", 1, 1, 4, true, true, false}},
    {F::Depth32Float, {"Depth32Float", 1, 1, 4, true, false, false}},
    {F::Depth32FloatStencil8, {"Depth32FloatStencil8", 1, 1, 5, true, true, false}},
}};

constexpr bool tableInOrder() {
    for (std::size_t i = 0; i < kFormats.size(); ++i)
        if (std::size_t(kFormats[i].format) != i) return false;
    return true;
}
static_assert(tableInOrder(), "kFormats rows must follow the TextureFormat order");

} // namespace

const FormatInfo& formatInfo(TextureFormat format) {
    QUENCH_ASSERT(format < F::Count, "invalid TextureFormat");
    return kFormats[std::size_t(format)].info;
}

std::uint64_t levelByteSize(TextureFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t layers) {
    const FormatInfo& info = formatInfo(format);
    const std::uint64_t blocksX = (std::uint64_t(width) + info.blockWidth - 1) / info.blockWidth;
    const std::uint64_t blocksY = (std::uint64_t(height) + info.blockHeight - 1) / info.blockHeight;
    return blocksX * blocksY * info.blockBytes * layers;
}

std::uint32_t fullMipCount(std::uint32_t width, std::uint32_t height) {
    return std::uint32_t(std::bit_width(std::max({width, height, 1u})));
}

const char* shaderFormatName(ShaderFormat format) {
    switch (format) {
    case ShaderFormat::Spirv: return "SPIR-V";
    case ShaderFormat::Msl: return "MSL source";
    case ShaderFormat::Metallib: return "metallib";
    case ShaderFormat::Dxil: return "DXIL";
    }
    return "unknown";
}

} // namespace quench::gpu
