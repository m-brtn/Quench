#include "Gltf.h"

#include <fastgltf/tools.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <format>
#include <fstream>
#include <iterator>
#include <map>

#include <simdjson.h>

namespace quench::cook {

namespace {

void readExtras(simdjson::dom::object* object, std::size_t index, fastgltf::Category category, void* user) {
    if (category != fastgltf::Category::Meshes || !object || !user) return;
    auto& names = static_cast<GltfExtras*>(user)->targetNames;
    if (names.size() <= index) names.resize(index + 1);
    simdjson::dom::array array;
    if ((*object)["targetNames"].get_array().get(array) != simdjson::SUCCESS) return;
    for (simdjson::dom::element element : array) {
        std::string_view name;
        names[index].emplace_back(element.get_string().get(name) == simdjson::SUCCESS ? name : std::string_view());
    }
}

Result<std::vector<std::uint8_t>> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{std::format("cannot open {}", path.string())};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

std::string_view gltfError(fastgltf::Error error) { return fastgltf::getErrorMessage(error); }

glm::mat4 toGlm(const fastgltf::math::fmat4x4& m) {
    glm::mat4 out(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) out[c][r] = m[std::size_t(c)][std::size_t(r)];
    return out;
}

Result<fastgltf::Asset> loadGltf(const std::filesystem::path& source, fastgltf::Options options,
                                 std::vector<std::filesystem::path>& inputs, GltfExtras* extras) {
    fastgltf::Expected<fastgltf::GltfDataBuffer> data = fastgltf::GltfDataBuffer::FromPath(source);
    if (data.error() != fastgltf::Error::None)
        return Error{std::format("{}: {}", source.string(), gltfError(data.error()))};

    fastgltf::Parser parser(fastgltf::Extensions::KHR_mesh_quantization | fastgltf::Extensions::KHR_texture_transform |
                            fastgltf::Extensions::KHR_materials_emissive_strength |
                            fastgltf::Extensions::KHR_materials_unlit);
    if (extras) {
        *extras = {};
        parser.setExtrasParseCallback(readExtras);
        parser.setUserPointer(extras);
    }
    const std::filesystem::path directory = source.parent_path();
    fastgltf::Expected<fastgltf::Asset> asset = parser.loadGltf(data.get(), directory, options);
    if (asset.error() != fastgltf::Error::None)
        return Error{std::format("{}: {}", source.string(), gltfError(asset.error()))};

    // External buffers are read here, not by fastgltf, so that every file read is listed in the depfile.
    for (fastgltf::Buffer& buffer : asset->buffers) {
        const auto* uri = std::get_if<fastgltf::sources::URI>(&buffer.data);
        if (!uri) continue;
        if (!uri->uri->isLocalPath()) return Error{std::format("{}: a buffer is not a local file", source.string())};
        const std::filesystem::path path = directory / uri->uri->fspath();
        inputs.push_back(path);
        Result<std::vector<std::uint8_t>> bytes = readFile(path);
        if (!bytes) return Error{bytes.error()};
        if (uri->fileByteOffset > bytes->size()) return Error{std::format("{}: offset past the end", path.string())};
        const auto* begin = reinterpret_cast<const std::byte*>(bytes->data()) + uri->fileByteOffset;
        buffer.data = fastgltf::sources::Vector{
            .bytes = std::vector<std::byte>(begin, reinterpret_cast<const std::byte*>(bytes->data()) + bytes->size()),
            .mimeType = fastgltf::MimeType::GltfBuffer};
    }
    if (const fastgltf::Error error = fastgltf::validate(asset.get()); error != fastgltf::Error::None)
        return Error{std::format("{}: {}", source.string(), gltfError(error))};
    return std::move(asset.get());
}

std::string morphTargetName(const fastgltf::Asset& asset, const GltfExtras& extras, std::size_t mesh,
                            std::size_t target) {
    if (mesh < extras.targetNames.size() && target < extras.targetNames[mesh].size() &&
        !extras.targetNames[mesh][target].empty())
        return extras.targetNames[mesh][target];
    const std::string_view name = mesh < asset.meshes.size() ? std::string_view(asset.meshes[mesh].name) : "";
    return name.empty() ? std::format("mesh{}.target{}", mesh, target) : std::format("{}.target{}", name, target);
}

std::vector<std::string> jointNames(const fastgltf::Asset& asset) {
    // "node<digits>" is kept for the generated names.
    const auto generated = [](std::string_view name) {
        return name.size() > 4 && name.starts_with("node") &&
               name.find_first_not_of("0123456789", 4) == std::string_view::npos;
    };
    std::map<std::string_view, int> uses;
    for (const fastgltf::Node& node : asset.nodes) ++uses[node.name];
    std::vector<std::string> names(asset.nodes.size());
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const std::string& name = asset.nodes[i].name;
        names[i] = !name.empty() && uses[name] == 1 && !generated(name) ? name : std::format("node{}", i);
    }
    return names;
}

NodeTransform nodeTransform(const fastgltf::Node& node) {
    NodeTransform out;
    if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
        out.translation = {trs->translation[0], trs->translation[1], trs->translation[2]};
        out.rotation =
            glm::normalize(glm::quat(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]));
        out.scale = {trs->scale[0], trs->scale[1], trs->scale[2]};
        return out;
    }
    const glm::mat4 m = toGlm(std::get<fastgltf::math::fmat4x4>(node.transform));
    out.translation = glm::vec3(m[3]);
    glm::vec3 axes[3] = {glm::vec3(m[0]), glm::vec3(m[1]), glm::vec3(m[2])};
    for (int i = 0; i < 3; ++i) out.scale[i] = glm::length(axes[i]);
    if (glm::determinant(glm::mat3(m)) < 0.0f) out.scale.x = -out.scale.x;
    for (int i = 0; i < 3; ++i)
        if (out.scale[i] != 0.0f) axes[i] /= out.scale[i];
    out.rotation = glm::normalize(glm::quat_cast(glm::mat3(axes[0], axes[1], axes[2])));
    return out;
}

std::vector<int> nodeParents(const fastgltf::Asset& asset) {
    std::vector<int> parents(asset.nodes.size(), -1);
    for (std::size_t i = 0; i < asset.nodes.size(); ++i)
        for (const std::size_t child : asset.nodes[i].children)
            if (child < parents.size()) parents[child] = int(i);
    return parents;
}

} // namespace quench::cook
