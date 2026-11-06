#pragma once

// quench_cook (tools/cook): converts source assets into the formats the engine loads at run time. Textures (PNG, JPEG,
// KTX2) become KTX2 files with their mip chain in BC7, ASTC 4x4 or RGBA8; glTF and GLB models become .qmodel files
// (include/quench/assets/Model.h) with meshoptimizer-optimized meshes, levels of detail, and their textures cooked
// next to them; their skins and animations become .qskel skeletons and .qanim clip sets
// (include/quench/animation/). The output depends only on the input files and the options (no time stamps, no
// thread order).

#include <quench/assets/Model.h>
#include <quench/assets/Texture.h>
#include <quench/core/Jobs.h>
#include <quench/core/Result.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::cook {

// BC7 for desktops, ASTC 4x4 for phones (both 8 bits per texel), RGBA8 uncompressed.
enum class TextureFormat : std::uint8_t { Bc7, Rgba8, Astc4x4 };

struct TextureOptions {
    TextureFormat format = TextureFormat::Bc7;
    bool srgb = true;          // color (sRGB transfer); false: linear data (normals, metallic-roughness)
    bool mips = true;          // the full chain down to 1 x 1
    std::uint32_t maxSize = 0; // halves the image until both sides fit; 0: no limit
};

// 8-bit RGBA, rows top to bottom.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

// PNG or JPEG (stb_image), converted to RGBA8.
Result<Image> decodeImage(std::span<const std::uint8_t> bytes);
// Mips (filtered in linear light for sRGB images), then BC7, ASTC 4x4 or RGBA8. The block formats need both sides of
// level 0 to be multiples of 4 (Metal's rule for block-compressed textures); other sizes are written as RGBA8 with a
// note in `notes`.
Result<assets::TextureImage> cookImage(const Image& image, const TextureOptions& options, JobSystem& jobs,
                                       std::vector<std::string>* notes = nullptr);
// A source file: PNG or JPEG, or KTX2 (RGBA8 is cooked like an image, BC7 and ASTC are passed through).
Result<assets::TextureImage> cookTexture(std::span<const std::uint8_t> source, const TextureOptions& options,
                                         JobSystem& jobs, std::vector<std::string>* notes = nullptr);
// BC7 blocks of one RGBA8 image (bc7enc), block rows spread over the job system. Sides need not be multiples of 4:
// edge blocks repeat the last row and column.
std::vector<std::uint8_t> encodeBc7(const Image& image, bool perceptual, JobSystem& jobs);
// ASTC 4x4 blocks of one RGBA8 image (astc-encoder, its medium preset; the LDR sRGB profile for color), block rows
// spread over the job system. Edge blocks repeat the last row and column.
Result<std::vector<std::uint8_t>> encodeAstc(const Image& image, bool srgb, JobSystem& jobs);
// One level of an ASTC 4x4 texture back to RGBA8 (astc-encoder's decoder: for tests and checks, not the runtime).
Result<Image> decodeAstc(const assets::TextureImage& texture, std::uint32_t level);

struct ModelOptions {
    TextureFormat textureFormat = TextureFormat::Bc7;
    std::uint32_t textureMaxSize = 0;
    std::uint32_t lods = 3; // levels of detail, LOD 0 included; 1: no simplification
    float lodRatio = 0.5f;  // target index count of each level relative to the one before
    float lodError = 0.05f; // largest simplification error, relative to the mesh extent
    float scale = 1.0f;     // applied to the positions
};

struct CookedTexture {
    std::string assetPath; // as the materials name it
    assets::TextureImage image;
};

struct CookedModel {
    assets::ModelData model;
    std::vector<CookedTexture> textures;
    std::vector<std::filesystem::path> inputs; // every file read, for the depfile
    std::vector<std::string> notes;            // what was dropped or changed (unsupported primitives, formats)
};

// The default scene (or the first) with node transforms applied, one mesh per material and skin. The materials name
// their textures `<texturePrefix>_<image index>.ktx2` (sRGB) or `<texturePrefix>_<image index>_linear.ktx2` (data),
// asset paths relative to the asset root. Skinned meshes keep their bind pose (glTF ignores their nodes'
// transforms), four joints and weights per vertex and their skin's joint names; `scale` must be 1 for them.
Result<CookedModel> cookModel(const std::filesystem::path& source, std::string_view texturePrefix,
                              const ModelOptions& options, JobSystem& jobs);

struct AnimationOptions {
    std::filesystem::path skeleton; // glTF or GLB whose skeleton the clips are for; empty: the input's own
    std::filesystem::path clips;    // clip settings (docs/animation.md); empty: <input stem>.anim.json, if it exists
};

struct CookedAnimation {
    std::vector<std::uint8_t> file;            // .qskel or .qanim
    std::vector<std::string> names;            // of the joints or the clips
    std::vector<std::filesystem::path> inputs; // every file read, for the depfile
    std::vector<std::string> notes;            // what was dropped or changed
};

// The skeleton of a glTF file (.qskel): the joints of its skins, their ancestors and the nodes below them that hold
// no mesh (attachment points), in the rest pose of their node transforms; a file without skins: its animated nodes
// and their ancestors. Joints are named after their nodes (unnamed or with a name used twice: node<index>).
Result<CookedAnimation> cookSkeleton(const std::filesystem::path& source);
// Every animation of a glTF file as a clip (.qanim) for its own skeleton or for `options.skeleton`'s, joints matched
// by name. Linear, step and cubic spline keys (the last resampled at 30 Hz); morph target weights are dropped.
Result<CookedAnimation> cookAnimation(const std::filesystem::path& source, const AnimationOptions& options);

} // namespace quench::cook
