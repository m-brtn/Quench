// quench_cook: the asset cooker (Cook.h). One source file per run; cmake/QuenchCook.cmake runs it at build time.

#include "Cook.h"

#include <quench/core/Log.h>
#include <quench/core/Parse.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace quench;

constexpr const char* kUsage =
    "usage:\n"
    "  quench_cook texture <input.png|.jpg|.ktx2> <output.ktx2> [options]\n"
    "  quench_cook model <input.gltf|.glb> <output.qmodel> [options]\n"
    "  quench_cook --version | --help\n"
    "options:\n"
    "  --format bc7|rgba8   texture format (default bc7)\n"
    "  --linear             texture: linear data, not sRGB color (normals, masks)\n"
    "  --no-mips            texture: level 0 only\n"
    "  --max-size N         halve textures until both sides are at most N\n"
    "  --asset-root DIR     model: the directory asset paths are relative to (default: the output's directory)\n"
    "  --lods N             model: levels of detail including LOD 0 (default 3)\n"
    "  --lod-ratio R        model: index count of each level relative to the one before (default 0.5)\n"
    "  --lod-error E        model: largest simplification error relative to the mesh extent (default 0.05)\n"
    "  --scale S            model: scales the positions (default 1)\n"
    "  --depfile FILE       writes the files read, for the build system\n"
    "  --verbose            one line per file written\n";

struct Options {
    std::string command;
    fs::path input;
    fs::path output;
    fs::path assetRoot;
    fs::path depfile;
    cook::TextureOptions texture;
    cook::ModelOptions model;
    bool verbose = false;
};

int fail(std::string_view message) {
    std::fprintf(stderr, "quench_cook: error: %.*s\n", int(message.size()), message.data());
    return 1;
}

Result<std::vector<std::uint8_t>> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{std::format("cannot open {}", path.string())};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Written to a temporary file and renamed, so that a running game polling for changes never reads half a file.
// `onlyIfChanged` keeps the file (and its time stamp) when the bytes are the same.
Result<void> writeFile(const fs::path& path, std::span<const std::uint8_t> bytes, bool onlyIfChanged) {
    if (onlyIfChanged) {
        if (Result<std::vector<std::uint8_t>> old = readFile(path);
            old && std::equal(old->begin(), old->end(), bytes.begin(), bytes.end()))
            return {};
    }
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const fs::path temporary = fs::path(path).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return Error{std::format("cannot write {}", temporary.string())};
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out) return Error{std::format("cannot write {}", temporary.string())};
    }
    fs::rename(temporary, path, ec);
    if (ec) return Error{std::format("cannot rename {} to {}: {}", temporary.string(), path.string(), ec.message())};
    return {};
}

std::string depfileEscape(const fs::path& path) {
    std::string out;
    for (const char c : fs::absolute(path).lexically_normal().string()) {
        if (c == ' ' || c == '#') out += '\\';
        if (c == '$') out += '$';
        out += c;
    }
    return out;
}

Result<void> writeDepfile(const Options& o, const std::vector<fs::path>& inputs) {
    std::string text = depfileEscape(o.output) + ":";
    for (const fs::path& input : inputs) text += " \\\n  " + depfileEscape(input);
    text += "\n";
    return writeFile(o.depfile, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), true);
}

std::optional<Options> parseArgs(int argc, char** argv, std::string& error) {
    Options o;
    std::vector<std::string_view> positional;
    const auto bad = [&error](std::string message) -> std::optional<Options> {
        error = std::move(message);
        return std::nullopt;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string_view(argv[++i]);
        };
        const auto number = [&]() -> std::optional<double> {
            const std::optional<std::string_view> v = value();
            return v ? parseDouble(*v) : std::nullopt;
        };
        if (arg == "--format") {
            const std::optional<std::string_view> v = value();
            if (v == "bc7")
                o.texture.format = o.model.textureFormat = cook::TextureFormat::Bc7;
            else if (v == "rgba8")
                o.texture.format = o.model.textureFormat = cook::TextureFormat::Rgba8;
            else
                return bad("--format: bc7 or rgba8");
        } else if (arg == "--linear") {
            o.texture.srgb = false;
        } else if (arg == "--no-mips") {
            o.texture.mips = false;
        } else if (arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "--max-size" || arg == "--lods") {
            const std::optional<double> v = number();
            if (!v || *v < 1 || *v > 65536 || *v != double(std::uint32_t(*v)))
                return bad(std::format("{}: a positive whole number", arg));
            if (arg == "--lods")
                o.model.lods = std::uint32_t(*v);
            else
                o.texture.maxSize = o.model.textureMaxSize = std::uint32_t(*v);
        } else if (arg == "--lod-ratio" || arg == "--lod-error" || arg == "--scale") {
            const std::optional<double> v = number();
            if (!v || *v <= 0.0) return bad(std::format("{}: a positive number", arg));
            (arg == "--lod-ratio"   ? o.model.lodRatio
             : arg == "--lod-error" ? o.model.lodError
                                    : o.model.scale) = float(*v);
        } else if (arg == "--asset-root" || arg == "--depfile") {
            const std::optional<std::string_view> v = value();
            if (!v) return bad(std::format("{}: a path", arg));
            (arg == "--asset-root" ? o.assetRoot : o.depfile) = fs::path(*v);
        } else if (arg.starts_with("--")) {
            return bad(std::format("unknown option {}", arg));
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 3 || (positional[0] != "texture" && positional[0] != "model"))
        return bad("expected: texture|model <input> <output> (--help)");
    o.command = positional[0];
    o.input = positional[1];
    o.output = positional[2];
    return o;
}

void printNotes(const Options& o, const std::vector<std::string>& notes) {
    for (const std::string& n : notes)
        std::fprintf(stderr, "quench_cook: %s: note: %s\n", o.input.string().c_str(), n.c_str());
}

int cookTexture(const Options& o, JobSystem& jobs) {
    Result<std::vector<std::uint8_t>> source = readFile(o.input);
    if (!source) return fail(source.error());
    std::vector<std::string> notes;
    Result<assets::TextureImage> image = cook::cookTexture(*source, o.texture, jobs, &notes);
    if (!image) return fail(std::format("{}: {}", o.input.string(), image.error()));
    printNotes(o, notes);
    Result<std::vector<std::uint8_t>> file = assets::writeKtx2(*image);
    if (!file) return fail(file.error());
    if (Result<void> r = writeFile(o.output, *file, false); !r) return fail(r.error());
    if (!o.depfile.empty())
        if (Result<void> r = writeDepfile(o, {o.input}); !r) return fail(r.error());
    if (o.verbose)
        std::fprintf(stderr, "quench_cook: %s: %u x %u, %u levels, %s, %zu bytes\n", o.output.string().c_str(),
                     image->width, image->height, image->mipLevels, assets::pixelFormatInfo(image->format).name,
                     file->size());
    return 0;
}

int cookModel(const Options& o, JobSystem& jobs) {
    const fs::path root = o.assetRoot.empty() ? o.output.parent_path() : o.assetRoot;
    const fs::path relative =
        fs::absolute(o.output).lexically_normal().lexically_relative(fs::absolute(root).lexically_normal());
    if (relative.empty() || relative.native().starts_with(".."))
        return fail(std::format("{} is not inside the asset root {}", o.output.string(), root.string()));
    const std::string prefix = fs::path(relative).replace_extension().generic_string();

    Result<cook::CookedModel> cooked = cook::cookModel(o.input, prefix, o.model, jobs);
    if (!cooked) return fail(cooked.error());
    printNotes(o, cooked->notes);
    for (const cook::CookedTexture& texture : cooked->textures) {
        Result<std::vector<std::uint8_t>> file = assets::writeKtx2(texture.image);
        if (!file) return fail(file.error());
        if (Result<void> r = writeFile(root / texture.assetPath, *file, true); !r) return fail(r.error());
        if (o.verbose)
            std::fprintf(stderr, "quench_cook: %s: %u x %u, %u levels, %s\n", texture.assetPath.c_str(),
                         texture.image.width, texture.image.height, texture.image.mipLevels,
                         assets::pixelFormatInfo(texture.image.format).name);
    }
    Result<std::vector<std::uint8_t>> file = assets::writeModel(cooked->model);
    if (!file) return fail(file.error());
    if (Result<void> r = writeFile(o.output, *file, false); !r) return fail(r.error());
    if (!o.depfile.empty())
        if (Result<void> r = writeDepfile(o, cooked->inputs); !r) return fail(r.error());
    if (o.verbose) {
        std::size_t triangles = 0;
        for (const assets::ModelMesh& mesh : cooked->model.meshes) triangles += mesh.lods[0].indexCount / 3;
        std::fprintf(stderr, "quench_cook: %s: %zu meshes, %zu triangles, %zu materials, %zu bytes\n",
                     o.output.string().c_str(), cooked->model.meshes.size(), triangles, cooked->model.materials.size(),
                     file->size());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        if (arg == "--version") {
            std::puts("quench_cook " QUENCH_COOK_VERSION);
            return 0;
        }
    }
    std::string error;
    const std::optional<Options> options = parseArgs(argc, argv, error);
    if (!options) return fail(error);
    log::setLevel(options->verbose ? log::Level::Info : log::Level::Warning);
    JobSystem jobs;
    return options->command == "texture" ? cookTexture(*options, jobs) : cookModel(*options, jobs);
}
