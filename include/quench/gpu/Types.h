#pragma once

// Plain types of the GPU layer: formats, flags and the descriptions resources, pipelines and passes are created from.
// Backend types never appear here; the SDL GPU backend maps these in src/gpu/sdlgpu.

#include <quench/core/Flags.h>

#include <quench/gpu/ShaderBlob.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace quench::gpu {

// ------------------------------------------------------------------ formats

enum class TextureFormat : std::uint8_t {
    Invalid,
    R8Unorm,
    Rg8Unorm,
    Rgba8Unorm,
    Rgba8UnormSrgb,
    Bgra8Unorm,
    Bgra8UnormSrgb,
    Rgb10a2Unorm,
    R16Float,
    Rg16Float,
    Rgba16Float,
    R32Float,
    Rg32Float,
    Rgba32Float,
    Rg11b10Ufloat,
    R32Uint,
    Bc1RgbaUnorm,
    Bc1RgbaUnormSrgb,
    Bc3RgbaUnorm,
    Bc3RgbaUnormSrgb,
    Bc4RUnorm,
    Bc5RgUnorm,
    Bc6hRgbUfloat,
    Bc7RgbaUnorm,
    Bc7RgbaUnormSrgb,
    Astc4x4Unorm,
    Astc4x4UnormSrgb,
    Depth16Unorm,
    Depth24UnormStencil8,
    Depth32Float,
    Depth32FloatStencil8,
    Count
};

struct FormatInfo {
    const char* name;
    std::uint8_t blockWidth; // texels per block side (1 for uncompressed formats)
    std::uint8_t blockHeight;
    std::uint8_t blockBytes; // bytes per block (per texel for uncompressed formats)
    bool depth;
    bool stencil;
    bool srgb;
};

const FormatInfo& formatInfo(TextureFormat format);
// Bytes of one tightly packed mip level of `layers` layers (or depth slices).
std::uint64_t levelByteSize(TextureFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t layers = 1);
// Levels of a full mip chain down to 1 x 1.
std::uint32_t fullMipCount(std::uint32_t width, std::uint32_t height);

// ------------------------------------------------------------------ resources

enum class TextureType : std::uint8_t { Texture2D, Texture2DArray, Texture3D, Cube, CubeArray };

enum class TextureUsage : std::uint8_t {
    None = 0,
    Sampled = 1 << 0,
    ColorTarget = 1 << 1,
    DepthStencilTarget = 1 << 2,
    GraphicsStorageRead = 1 << 3,
};
QUENCH_FLAGS(TextureUsage)

struct TextureDesc {
    TextureType type = TextureType::Texture2D;
    TextureFormat format = TextureFormat::Rgba8Unorm;
    TextureUsage usage = TextureUsage::Sampled;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t layers = 1; // array layers (6 per cube), or the depth of a 3D texture
    std::uint32_t mipLevels = 1;
    std::uint32_t sampleCount = 1; // 1, 2, 4 or 8
    std::string_view name{};       // debug name for GPU captures; not kept after creation
};

enum class BufferUsage : std::uint8_t {
    None = 0,
    Vertex = 1 << 0,
    Index = 1 << 1,
    Indirect = 1 << 2,
    GraphicsStorageRead = 1 << 3,
    ComputeStorageRead = 1 << 4,
    ComputeStorageWrite = 1 << 5, // ComputePass read-write bindings (also readable there)
};
QUENCH_FLAGS(BufferUsage)

struct BufferDesc {
    BufferUsage usage = BufferUsage::Vertex;
    std::uint32_t size = 0;
    std::string_view name{};
};

enum class TransferDirection : std::uint8_t { Upload, Download };

struct TransferBufferDesc {
    TransferDirection direction = TransferDirection::Upload;
    std::uint32_t size = 0;
    std::string_view name{};
};

enum class Filter : std::uint8_t { Nearest, Linear };
enum class MipmapMode : std::uint8_t { Nearest, Linear };
enum class AddressMode : std::uint8_t { Repeat, MirroredRepeat, ClampToEdge };
enum class CompareOp : std::uint8_t { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    MipmapMode mipmapMode = MipmapMode::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    float mipLodBias = 0.0f;
    float maxAnisotropy = 1.0f; // > 1 enables anisotropic filtering
    bool compare = false;       // depth comparison sampler (shadow maps)
    CompareOp compareOp = CompareOp::LessOrEqual;
    float minLod = 0.0f;
    float maxLod = 1000.0f; // every level
    std::string_view name{};
};

// ------------------------------------------------------------------ pipelines

enum class PrimitiveType : std::uint8_t { TriangleList, TriangleStrip, LineList, LineStrip, PointList };
enum class FillMode : std::uint8_t { Fill, Line };
enum class CullMode : std::uint8_t { None, Front, Back };
enum class FrontFace : std::uint8_t { CounterClockwise, Clockwise };

enum class VertexFormat : std::uint8_t {
    Float,
    Float2,
    Float3,
    Float4,
    Half2,
    Half4,
    Uint,
    Uint2,
    Uint4,
    Ubyte4,
    Ubyte4Norm,
    Short2Norm,
    Short4Norm,
    Ushort4,
    Ushort4Norm,
};
enum class VertexInputRate : std::uint8_t { PerVertex, PerInstance };

struct VertexBufferLayout {
    std::uint32_t slot = 0;
    std::uint32_t stride = 0;
    VertexInputRate rate = VertexInputRate::PerVertex;
};

struct VertexAttribute {
    std::uint32_t location = 0;
    std::uint32_t slot = 0;
    VertexFormat format = VertexFormat::Float3;
    std::uint32_t offset = 0;
};

enum class BlendFactor : std::uint8_t {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstantColor,
    OneMinusConstantColor,
    SrcAlphaSaturate,
};
enum class BlendOp : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

enum class ColorWrite : std::uint8_t { None = 0, R = 1, G = 2, B = 4, A = 8, All = 15 };
QUENCH_FLAGS(ColorWrite)

struct BlendState {
    bool enabled = false;
    BlendFactor srcColor = BlendFactor::One;
    BlendFactor dstColor = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::Zero;
    BlendOp alphaOp = BlendOp::Add;
    ColorWrite writeMask = ColorWrite::All;

    // Straight alpha: color * a + dst * (1 - a); alpha accumulates as coverage.
    static constexpr BlendState alpha() {
        return {true,         BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha,
                BlendOp::Add, BlendFactor::One,      BlendFactor::OneMinusSrcAlpha,
                BlendOp::Add, ColorWrite::All};
    }
    static constexpr BlendState premultiplied() {
        return {true,         BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                BlendOp::Add, BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                BlendOp::Add, ColorWrite::All};
    }
    static constexpr BlendState additive() {
        return {true,         BlendFactor::One, BlendFactor::One, BlendOp::Add, BlendFactor::One, BlendFactor::One,
                BlendOp::Add, ColorWrite::All};
    }
};

struct ColorTargetDesc {
    TextureFormat format = TextureFormat::Invalid;
    BlendState blend{};
};

struct RasterizerState {
    FillMode fill = FillMode::Fill;
    CullMode cull = CullMode::None;
    FrontFace frontFace = FrontFace::CounterClockwise;
    bool depthBias = false;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    float depthBiasClamp = 0.0f; // ignored where !Caps::depthBiasClamp
    bool depthClip = true;       // false clamps depth instead (shadow casters); needs Caps::depthClamp
};

// The engine renders the scene with reversed depth (near 1, far 0), hence the default comparison.
struct DepthState {
    bool test = false;
    bool write = false;
    CompareOp compare = CompareOp::GreaterOrEqual;
};

struct GraphicsPipelineDesc {
    const ShaderBlob* vertexShader = nullptr;
    const ShaderBlob* fragmentShader = nullptr;
    std::span<const VertexBufferLayout> vertexBuffers{};
    std::span<const VertexAttribute> vertexAttributes{};
    PrimitiveType primitive = PrimitiveType::TriangleList;
    RasterizerState rasterizer{};
    DepthState depth{};
    std::span<const ColorTargetDesc> colorTargets{};
    TextureFormat depthFormat = TextureFormat::Invalid; // Invalid: no depth target
    std::uint32_t sampleCount = 1;
    bool alphaToCoverage = false;
    std::string_view name{};
};

// The resource counts and the thread group size come from the shader's reflection (ShaderBlob::resources).
struct ComputePipelineDesc {
    const ShaderBlob* shader = nullptr;
    std::string_view name{};
};

// ------------------------------------------------------------------ passes

enum class LoadOp : std::uint8_t { Load, Clear, DontCare };
enum class StoreOp : std::uint8_t { Store, DontCare, Resolve, ResolveAndStore };
enum class IndexFormat : std::uint8_t { Uint16, Uint32 };

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

enum class PresentMode : std::uint8_t {
    Vsync,     // waits for the display; always supported
    Immediate, // no wait, may tear; falls back to Vsync where unsupported
    Mailbox,   // no wait, no tearing; falls back to Vsync where unsupported
};

// How the system composites a window's swapchain. Each composition has its own swapchain format.
enum class SwapchainComposition : std::uint8_t {
    Sdr,               // Bgra8Unorm: display values (sRGB-encoded); always supported
    SdrLinear,         // Bgra8UnormSrgb: linear values, encoded by the hardware
    HdrExtendedLinear, // Rgba16Float: linear, BT.709 primaries; paper white at DisplayOutput::paperWhite, brighter
                       // above
    Hdr10,             // Rgb10a2Unorm: BT.2020 primaries, PQ (SMPTE ST 2084) encoded
};

const char* swapchainCompositionName(SwapchainComposition composition);

// What the values of a frame mean on the display: the swapchain composition and, for HDR, the brightness range.
struct DisplayOutput {
    SwapchainComposition composition = SwapchainComposition::Sdr;
    float headroom = 1.0f;   // the brightest value shown, in multiples of paper white (1: no HDR range)
    float paperWhite = 1.0f; // the value of SDR white in HdrExtendedLinear (1 on Apple platforms)

    bool hdr() const {
        return composition == SwapchainComposition::HdrExtendedLinear || composition == SwapchainComposition::Hdr10;
    }
    friend bool operator==(const DisplayOutput&, const DisplayOutput&) = default;
};

} // namespace quench::gpu
