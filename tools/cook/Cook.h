#pragma once

// quench_cook (tools/cook): converts source assets into the formats the engine loads at run time. Textures (PNG, JPEG,
// KTX2) become KTX2 files with their mip chain in BC7, ASTC 4x4, RGBA8 or Basis Universal (ETC1S or UASTC, transcoded
// at load time to what the device samples); glTF and GLB models become .qmodel files
// (include/quench/assets/Model.h) with meshoptimizer-optimized meshes, levels of detail, and their textures cooked
// next to them; their skins and animations become .qskel skeletons and .qanim clip sets
// (include/quench/animation/); level geometry becomes .qnav navmeshes (include/quench/navigation/). The output depends
// only on the input files and the options (no time stamps, no thread order).

#include <quench/animation/Clip.h>
#include <quench/animation/Skeleton.h>
#include <quench/assets/Model.h>
#include <quench/assets/Texture.h>
#include <quench/core/Jobs.h>
#include <quench/core/Result.h>
#include <quench/navigation/NavMesh.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quench::cook {

// BC7 for desktops, ASTC 4x4 for phones (both 8 bits per texel), RGBA8 uncompressed; Basis Universal for every device
// (assets::prepareTexture transcodes it at load time).
enum class TextureFormat : std::uint8_t { Bc7, Rgba8, Astc4x4, Basis };

// Basis Universal's codec: ETC1S (BasisLZ: small files, lower quality, blocky on normal maps), UASTC LDR 4x4 (about
// BC7's quality, Zstandard supercompressed), or Auto: UASTC for linear data (normals, metallic-roughness), ETC1S for
// color.
enum class BasisMode : std::uint8_t { Auto, Etc1s, Uastc };

struct TextureOptions {
    TextureFormat format = TextureFormat::Bc7;
    bool srgb = true;          // color (sRGB transfer); false: linear data (normals, metallic-roughness)
    bool mips = true;          // the full chain down to 1 x 1
    std::uint32_t maxSize = 0; // halves the image until both sides fit; 0: no limit
    BasisMode basis = BasisMode::Auto;
};

// The codec TextureFormat::Basis encodes with.
assets::BasisCodec basisCodec(const TextureOptions& options);

// 8-bit RGBA, rows top to bottom.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

// PNG or JPEG (stb_image), converted to RGBA8.
Result<Image> decodeImage(std::span<const std::uint8_t> bytes);
// Mips (filtered in linear light for sRGB images), then BC7, ASTC 4x4 or RGBA8 (TextureFormat::Basis is
// cookImageFile's). The block formats need both sides of level 0 to be multiples of 4 (Metal's rule for
// block-compressed textures); other sizes are written as RGBA8 with a note in `notes`.
Result<assets::TextureImage> cookImage(const Image& image, const TextureOptions& options, JobSystem& jobs,
                                       std::vector<std::string>* notes = nullptr);
// A source file: PNG or JPEG, or KTX2 (RGBA8 is cooked like an image, BC7 and ASTC are passed through, Basis
// Universal is transcoded to the format asked for).
Result<assets::TextureImage> cookTexture(std::span<const std::uint8_t> source, const TextureOptions& options,
                                         JobSystem& jobs, std::vector<std::string>* notes = nullptr);
// The KTX2 file of cookImage or cookTexture, in any format. TextureFormat::Basis: the same mips encoded with
// basisCodec(options); a Basis Universal source is passed through, a BC7 or ASTC one decoded and encoded again (with a
// note). Basis Universal files of any size are written (devices get RGBA8 where a side is not a multiple of 4, with
// a note).
Result<std::vector<std::uint8_t>> cookImageFile(const Image& image, const TextureOptions& options, JobSystem& jobs,
                                                std::vector<std::string>* notes = nullptr);
Result<std::vector<std::uint8_t>> cookTextureFile(std::span<const std::uint8_t> source, const TextureOptions& options,
                                                  JobSystem& jobs, std::vector<std::string>* notes = nullptr);
// "128 x 64, 8 levels, bc7_srgb" or "128 x 64, 8 levels, basis uastc (zstd), srgb" for a KTX2 file.
std::string describeTexture(std::span<const std::uint8_t> file);
// BC7 blocks of one RGBA8 image (bc7enc), block rows spread over the job system. Sides need not be multiples of 4:
// edge blocks repeat the last row and column.
std::vector<std::uint8_t> encodeBc7(const Image& image, bool perceptual, JobSystem& jobs);
// ASTC 4x4 blocks of one RGBA8 image (astc-encoder, its medium preset; the LDR sRGB profile for color), block rows
// spread over the job system. Edge blocks repeat the last row and column.
Result<std::vector<std::uint8_t>> encodeAstc(const Image& image, bool srgb, JobSystem& jobs);
// One level of an ASTC 4x4 texture back to RGBA8 (astc-encoder's decoder: for tests and checks, not the runtime).
Result<Image> decodeAstc(const assets::TextureImage& texture, std::uint32_t level);
// A Basis Universal KTX2 file of `levels` (level 0 first, each half the one before; any size) with Basis Universal's
// encoder (v2.50: ETC1S at quality 128, compression level 2; UASTC at its default pack level, Zstandard level 6). On
// the calling thread only, so the output depends on the input alone.
Result<std::vector<std::uint8_t>> encodeBasis(std::span<const Image> levels, bool srgb, assets::BasisCodec codec);
// One level of an ETC2 RGB8 or RGBA8 texture back to RGBA8 (Basis Universal's block unpacker, which knows the ETC1
// modes its transcoder writes, not ETC2's T, H and planar modes: for tests and checks, not the runtime).
Result<Image> decodeEtc2(const assets::TextureImage& texture, std::uint32_t level);

struct ModelOptions {
    TextureFormat textureFormat = TextureFormat::Bc7;
    BasisMode textureBasis = BasisMode::Auto;
    std::uint32_t textureMaxSize = 0;
    std::uint32_t lods = 3; // levels of detail, LOD 0 included; 1: no simplification
    float lodRatio = 0.5f;  // target index count of each level relative to the one before
    float lodError = 0.05f; // largest simplification error, relative to the mesh extent
    float scale = 1.0f;     // applied to the positions
};

struct CookedTexture {
    std::string assetPath;          // as the materials name it
    std::vector<std::uint8_t> file; // KTX2 (cookTextureFile)
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
    // A bone map (parseRetargetMap): the clips are made for the input's own skeleton and retargeted onto
    // `skeleton`'s, which may have other names, proportions and rest poses. Empty: joints matched by name.
    std::filesystem::path retarget;
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
// by name, or retargeted through `options.retarget`. Linear, step and cubic spline keys (the last resampled at 30 Hz);
// morph target weights are dropped.
Result<CookedAnimation> cookAnimation(const std::filesystem::path& source, const AnimationOptions& options);

// Retargeting (docs/animation.md, "Retargeting"): a JSON bone map from the joints of a target skeleton to those of
// the skeleton the clips were made for,
//     {"joints": {"<target joint>": "<source joint>", ...}, "translations": ["<target joint>", ...],
//      "scale": "<target joint>" | <number>}
// Every mapped joint takes the source joint's rotation in model space, corrected for the two rest poses: a joint
// whose bone runs to one mapped joint below it is turned so that its rest bone points where the source's does (a
// T-pose arm follows an arm that hangs at rest); a joint with several (hips, chest) keeps the difference of the two
// rest orientations; one with none (a hand, a head) turns as its nearest mapped ancestor. The translations' joints
// (the root and the hips) move as the source's do, times `scale`: a number, or the named joint's rest height in the
// target over its source's (default: the first translation joint above the ground in both). Other joints keep the
// target's rest translations (its bone lengths) and scales; unmapped joints keep their rest pose. Root motion and
// events carry over (the root motion joint through the map).
struct RetargetMap {
    std::vector<std::pair<std::string, std::string>> joints; // target joint, source joint
    std::vector<std::string> translations;                   // target joints
    std::string scaleJoint;                                  // the translations' scale from its rest heights
    float scale = 0.0f;                                      // > 0: this scale instead
};
Result<RetargetMap> parseRetargetMap(std::string_view json);
// Clips for `source` (tracks in its description's order) as clips for `target`, resampled at the source's key times
// and at least 30 Hz. Notes: unmapped joints, dropped scale keys.
Result<std::vector<animation::ClipDesc>> retargetClips(const animation::SkeletonDesc& source,
                                                       const animation::SkeletonDesc& target,
                                                       std::span<const animation::ClipDesc> clips,
                                                       const RetargetMap& map, std::vector<std::string>* notes);

// The settings of a navmesh cook (docs/navigation.md, "Cooking"): JSON with the build settings, area types, which
// surfaces of the source are which area type (by node, mesh or material name), area boxes and off-mesh links.
struct NavMeshSettings {
    static constexpr int kIgnore = -1; // a surface left out of the navmesh's geometry
    navigation::BuildSettings build;
    std::vector<std::pair<std::string, int>> surfaces; // name, area id (navigation::kNotWalkable: none) or kIgnore
    std::vector<navigation::AreaBox> boxes;
    std::vector<navigation::Link> links;
};
Result<NavMeshSettings> parseNavMeshSettings(std::string_view json);

struct NavMeshOptions {
    std::filesystem::path settings; // empty: <input stem>.nav.json when it exists, else the default settings
};

struct CookedNavMesh {
    std::vector<std::uint8_t> file; // .qnav (navigation::writeNavMeshData)
    std::size_t triangles = 0;
    int tiles = 0;
    int layers = 0;
    std::vector<std::filesystem::path> inputs; // every file read, for the depfile
    std::vector<std::string> notes;            // what was left out, settings that name nothing
};

// A navmesh of a glTF or GLB file (its default scene with node transforms; triangle lists, skinned meshes left out)
// or of a .qmodel (every mesh's LOD 0), built tile by tile on the job system. A triangle's area type is the one the
// settings give the first of its node's, mesh's and material's names (.qmodel: mesh and material) they name, else
// the first area type.
Result<CookedNavMesh> cookNavMesh(const std::filesystem::path& source, const NavMeshOptions& options, JobSystem& jobs);

} // namespace quench::cook
