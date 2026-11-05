#include "Cook.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

namespace quench::cook {
namespace {

using assets::ModelMaterial;
using assets::ModelMesh;
using assets::ModelVertex;

Result<std::vector<std::uint8_t>> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{std::format("cannot open {}", path.string())};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string_view gltfError(fastgltf::Error error) { return fastgltf::getErrorMessage(error); }

glm::mat4 toGlm(const fastgltf::math::fmat4x4& m) {
    glm::mat4 out(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) out[c][r] = m[std::size_t(c)][std::size_t(r)];
    return out;
}

// Triangles of one material, in model space.
struct Batch {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
};

class ModelCooker {
public:
    ModelCooker(fastgltf::Asset& asset, std::filesystem::path directory, std::string_view texturePrefix,
                const ModelOptions& options, JobSystem& jobs, CookedModel& out)
        : m_asset(asset), m_directory(std::move(directory)), m_texturePrefix(texturePrefix), m_options(options),
          m_jobs(jobs), m_out(out) {}

    Result<void> run() {
        for (const fastgltf::Material& m : m_asset.materials) m_out.model.materials.push_back(material(m));
        if (m_asset.scenes.empty()) return Error{"the file has no scene"};
        const std::size_t scene = m_asset.defaultScene.value_or(0);
        if (scene >= m_asset.scenes.size()) return Error{"the default scene does not exist"};
        Result<void> status{};
        fastgltf::iterateSceneNodes(m_asset, scene, fastgltf::math::fmat4x4(),
                                    [&](fastgltf::Node& node, const fastgltf::math::fmat4x4& matrix) {
                                        if (!status || !node.meshIndex) return;
                                        status = addMesh(m_asset.meshes[*node.meshIndex], toGlm(matrix));
                                    });
        if (!status) return status;
        if (m_batches.empty()) return Error{"the scene has no triangles"};

        for (auto& [materialIndex, batch] : m_batches) {
            Result<ModelMesh> mesh = optimize(std::move(batch), materialIndex);
            if (!mesh) return Error{mesh.error()};
            m_out.model.meshes.push_back(std::move(*mesh));
        }
        if (m_defaultMaterial)
            m_out.model.materials.push_back({.name = "default", .roughness = 0.8f, .metallic = 0.0f});
        assets::ModelData& model = m_out.model;
        model.boundsMin = model.meshes[0].boundsMin;
        model.boundsMax = model.meshes[0].boundsMax;
        for (const ModelMesh& mesh : model.meshes) {
            model.boundsMin = glm::min(model.boundsMin, mesh.boundsMin);
            model.boundsMax = glm::max(model.boundsMax, mesh.boundsMax);
        }
        return cookTextures();
    }

private:
    void note(std::string text) {
        if (std::find(m_out.notes.begin(), m_out.notes.end(), text) == m_out.notes.end())
            m_out.notes.push_back(std::move(text));
    }

    // The asset path of an image's cooked texture, cooked later in cookTextures().
    std::string texture(const fastgltf::TextureInfo& info, bool srgb, std::string_view slot) {
        if (info.textureIndex >= m_asset.textures.size()) return {};
        const fastgltf::Texture& t = m_asset.textures[info.textureIndex];
        if (!t.imageIndex) {
            note(std::format("{} texture {}: no PNG, JPEG or KTX2 image (an extension's format); dropped", slot,
                             info.textureIndex));
            return {};
        }
        if (info.texCoordIndex != 0)
            note(std::format("{} texture: TEXCOORD_{} read as TEXCOORD_0", slot, info.texCoordIndex));
        if (info.transform) note(std::format("{} texture: KHR_texture_transform ignored", slot));
        const auto key = std::make_pair(*t.imageIndex, srgb);
        auto [it, added] = m_images.try_emplace(key);
        if (added) it->second = std::format("{}_{}{}.ktx2", m_texturePrefix, *t.imageIndex, srgb ? "" : "_linear");
        return it->second;
    }

    ModelMaterial material(const fastgltf::Material& m) {
        ModelMaterial out;
        out.name = std::string(m.name);
        const auto& f = m.pbrData.baseColorFactor;
        out.baseColor = glm::vec4(f[0], f[1], f[2], f[3]);
        out.emissive = glm::vec3(m.emissiveFactor[0], m.emissiveFactor[1], m.emissiveFactor[2]) * m.emissiveStrength;
        out.roughness = m.pbrData.roughnessFactor;
        out.metallic = m.pbrData.metallicFactor;
        out.alphaMode = m.alphaMode == fastgltf::AlphaMode::Mask    ? assets::AlphaMode::Mask
                        : m.alphaMode == fastgltf::AlphaMode::Blend ? assets::AlphaMode::Blend
                                                                    : assets::AlphaMode::Opaque;
        out.alphaCutoff = m.alphaCutoff;
        out.doubleSided = m.doubleSided;
        if (m.pbrData.baseColorTexture) out.baseColorTexture = texture(*m.pbrData.baseColorTexture, true, "base color");
        if (m.pbrData.metallicRoughnessTexture)
            out.metallicRoughnessTexture = texture(*m.pbrData.metallicRoughnessTexture, false, "metallic-roughness");
        if (m.normalTexture) out.normalTexture = texture(*m.normalTexture, false, "normal");
        if (m.emissiveTexture) out.emissiveTexture = texture(*m.emissiveTexture, true, "emissive");
        if (m.occlusionTexture) note("occlusion textures are not cooked");
        if (m.unlit) note("KHR_materials_unlit: cooked as a lit material");
        return out;
    }

    template <class T, class F>
    Result<void> read(std::size_t accessorIndex, fastgltf::AccessorType type, std::size_t count, std::string_view what,
                      F&& f) {
        if (accessorIndex >= m_asset.accessors.size()) return Error{std::format("{}: no such accessor", what)};
        const fastgltf::Accessor& accessor = m_asset.accessors[accessorIndex];
        if (accessor.type != type) return Error{std::format("{}: unexpected accessor type", what)};
        if (accessor.count != count)
            return Error{std::format("{}: {} values for {} vertices", what, accessor.count, count)};
        fastgltf::iterateAccessorWithIndex<T>(m_asset, accessor, f);
        return {};
    }

    Result<void> addMesh(const fastgltf::Mesh& mesh, const glm::mat4& transform) {
        glm::mat4 scale(m_options.scale);
        scale[3][3] = 1.0f;
        const glm::mat4 toModel = scale * transform;
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(toModel)));
        const bool flip = glm::determinant(glm::mat3(toModel)) < 0.0f;

        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const fastgltf::Primitive& prim = mesh.primitives[p];
            const std::string what = std::format("mesh '{}' primitive {}", mesh.name, p);
            if (prim.type != fastgltf::PrimitiveType::Triangles) {
                note(std::format("{}: not a triangle list; dropped", what));
                continue;
            }
            const auto position = prim.findAttribute("POSITION");
            if (position == prim.attributes.end()) return Error{std::format("{}: no POSITION", what)};
            const std::size_t vertexCount = m_asset.accessors.at(position->accessorIndex).count;
            if (vertexCount == 0) continue;

            std::vector<ModelVertex> vertices(vertexCount);
            Result<void> r =
                read<fastgltf::math::fvec3>(position->accessorIndex, fastgltf::AccessorType::Vec3, vertexCount,
                                            what + " POSITION", [&](const fastgltf::math::fvec3& v, std::size_t i) {
                                                vertices[i].position =
                                                    glm::vec3(toModel * glm::vec4(v[0], v[1], v[2], 1.0f));
                                            });
            if (!r) return r;
            bool hasNormals = false;
            if (const auto normal = prim.findAttribute("NORMAL"); normal != prim.attributes.end()) {
                hasNormals = true;
                r = read<fastgltf::math::fvec3>(normal->accessorIndex, fastgltf::AccessorType::Vec3, vertexCount,
                                                what + " NORMAL", [&](const fastgltf::math::fvec3& v, std::size_t i) {
                                                    const glm::vec3 n = normalMatrix * glm::vec3(v[0], v[1], v[2]);
                                                    const float length = glm::length(n);
                                                    vertices[i].normal =
                                                        length > 1e-12f ? n / length : glm::vec3(0, 1, 0);
                                                });
                if (!r) return r;
            }
            if (const auto uv = prim.findAttribute("TEXCOORD_0"); uv != prim.attributes.end()) {
                r = read<fastgltf::math::fvec2>(
                    uv->accessorIndex, fastgltf::AccessorType::Vec2, vertexCount, what + " TEXCOORD_0",
                    [&](const fastgltf::math::fvec2& v, std::size_t i) { vertices[i].uv = glm::vec2(v[0], v[1]); });
                if (!r) return r;
            }
            if (const auto color = prim.findAttribute("COLOR_0"); color != prim.attributes.end()) {
                const fastgltf::AccessorType type = m_asset.accessors.at(color->accessorIndex).type;
                if (type == fastgltf::AccessorType::Vec4)
                    r = read<fastgltf::math::fvec4>(color->accessorIndex, type, vertexCount, what + " COLOR_0",
                                                    [&](const fastgltf::math::fvec4& v, std::size_t i) {
                                                        vertices[i].color = glm::vec4(v[0], v[1], v[2], v[3]);
                                                    });
                else
                    r = read<fastgltf::math::fvec3>(color->accessorIndex, fastgltf::AccessorType::Vec3, vertexCount,
                                                    what + " COLOR_0",
                                                    [&](const fastgltf::math::fvec3& v, std::size_t i) {
                                                        vertices[i].color = glm::vec4(v[0], v[1], v[2], 1.0f);
                                                    });
                if (!r) return r;
            }

            std::vector<std::uint32_t> indices;
            if (prim.indicesAccessor) {
                if (*prim.indicesAccessor >= m_asset.accessors.size())
                    return Error{std::format("{}: no such index accessor", what)};
                const fastgltf::Accessor& accessor = m_asset.accessors[*prim.indicesAccessor];
                if (accessor.type != fastgltf::AccessorType::Scalar)
                    return Error{std::format("{}: indices are not scalars", what)};
                indices.resize(accessor.count);
                fastgltf::iterateAccessorWithIndex<std::uint32_t>(
                    m_asset, accessor, [&](std::uint32_t index, std::size_t i) { indices[i] = index; });
            } else {
                indices.resize(vertexCount);
                for (std::size_t i = 0; i < vertexCount; ++i) indices[i] = std::uint32_t(i);
            }
            if (indices.size() % 3 != 0) return Error{std::format("{}: {} indices", what, indices.size())};
            for (const std::uint32_t index : indices)
                if (index >= vertexCount) return Error{std::format("{}: index {} out of range", what, index)};
            if (flip)
                for (std::size_t i = 0; i < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
            if (!hasNormals) smoothNormals(vertices, indices);

            std::uint32_t materialIndex = 0;
            if (prim.materialIndex && *prim.materialIndex < m_asset.materials.size()) {
                materialIndex = std::uint32_t(*prim.materialIndex);
            } else {
                m_defaultMaterial = true;
                materialIndex = std::uint32_t(m_asset.materials.size());
            }
            Batch& batch = m_batches[materialIndex];
            const std::uint32_t base = std::uint32_t(batch.vertices.size());
            batch.vertices.insert(batch.vertices.end(), vertices.begin(), vertices.end());
            for (const std::uint32_t index : indices) batch.indices.push_back(base + index);
        }
        return {};
    }

    // Area-weighted vertex normals where the file has none.
    static void smoothNormals(std::vector<ModelVertex>& vertices, const std::vector<std::uint32_t>& indices) {
        std::vector<glm::vec3> sums(vertices.size(), glm::vec3(0.0f));
        for (std::size_t i = 0; i < indices.size(); i += 3) {
            const glm::vec3& a = vertices[indices[i]].position;
            const glm::vec3 n =
                glm::cross(vertices[indices[i + 1]].position - a, vertices[indices[i + 2]].position - a);
            for (std::size_t k = 0; k < 3; ++k) sums[indices[i + k]] += n;
        }
        for (std::size_t v = 0; v < vertices.size(); ++v) {
            const float length = glm::length(sums[v]);
            vertices[v].normal = length > 1e-20f ? sums[v] / length : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    // Welds equal vertices, orders triangles for the vertex cache, adds simplified levels of detail and orders the
    // vertices for fetching (one vertex buffer for every level).
    Result<ModelMesh> optimize(Batch batch, std::uint32_t materialIndex) {
        const std::size_t indexCount = batch.indices.size();
        std::vector<std::uint32_t> remap(batch.vertices.size());
        const std::size_t unique =
            meshopt_generateVertexRemap(remap.data(), batch.indices.data(), indexCount, batch.vertices.data(),
                                        batch.vertices.size(), sizeof(ModelVertex));
        std::vector<ModelVertex> vertices(unique);
        std::vector<std::uint32_t> lod0(indexCount);
        meshopt_remapIndexBuffer(lod0.data(), batch.indices.data(), indexCount, remap.data());
        meshopt_remapVertexBuffer(vertices.data(), batch.vertices.data(), batch.vertices.size(), sizeof(ModelVertex),
                                  remap.data());
        meshopt_optimizeVertexCache(lod0.data(), lod0.data(), indexCount, unique);

        std::vector<std::vector<std::uint32_t>> lods{lod0};
        std::vector<float> errors{0.0f};
        const float* positions = &vertices[0].position.x;
        float target = float(indexCount);
        for (std::uint32_t level = 1; level < m_options.lods; ++level) {
            target *= m_options.lodRatio;
            const std::size_t targetCount = std::size_t(target) / 3 * 3;
            if (targetCount < 3) break;
            std::vector<std::uint32_t> lod(indexCount);
            float error = 0.0f;
            lod.resize(meshopt_simplify(lod.data(), lod0.data(), indexCount, positions, unique, sizeof(ModelVertex),
                                        targetCount, m_options.lodError, 0, &error));
            // Stop where simplification no longer pays: under 10% fewer triangles than the level before.
            if (lod.empty() || float(lod.size()) > 0.9f * float(lods.back().size())) break;
            meshopt_optimizeVertexCache(lod.data(), lod.data(), lod.size(), unique);
            lods.push_back(std::move(lod));
            errors.push_back(error);
        }

        ModelMesh mesh;
        mesh.material = materialIndex;
        mesh.name = materialIndex < m_out.model.materials.size() && !m_out.model.materials[materialIndex].name.empty()
                        ? m_out.model.materials[materialIndex].name
                        : std::format("mesh{}", m_out.model.meshes.size());
        for (std::size_t l = 0; l < lods.size(); ++l) {
            mesh.lods.push_back({.firstIndex = std::uint32_t(mesh.indices.size()),
                                 .indexCount = std::uint32_t(lods[l].size()),
                                 .error = errors[l]});
            mesh.indices.insert(mesh.indices.end(), lods[l].begin(), lods[l].end());
        }
        mesh.vertices.resize(unique);
        mesh.vertices.resize(meshopt_optimizeVertexFetch(mesh.vertices.data(), mesh.indices.data(), mesh.indices.size(),
                                                         vertices.data(), unique, sizeof(ModelVertex)));
        mesh.boundsMin = mesh.boundsMax = mesh.vertices[0].position;
        for (const ModelVertex& v : mesh.vertices) {
            mesh.boundsMin = glm::min(mesh.boundsMin, v.position);
            mesh.boundsMax = glm::max(mesh.boundsMax, v.position);
        }
        return mesh;
    }

    Result<std::vector<std::uint8_t>> imageBytes(std::size_t imageIndex) {
        const fastgltf::Image& image = m_asset.images[imageIndex];
        const auto copy = [](std::span<const std::byte> bytes) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
            return std::vector<std::uint8_t>(p, p + bytes.size());
        };
        return std::visit(
            fastgltf::visitor{
                [&](const fastgltf::sources::URI& uri) -> Result<std::vector<std::uint8_t>> {
                    if (!uri.uri->isLocalPath()) return Error{std::format("image {}: not a local file", imageIndex)};
                    const std::filesystem::path path = m_directory / uri.uri->fspath();
                    m_out.inputs.push_back(path);
                    Result<std::vector<std::uint8_t>> bytes = readFile(path);
                    if (bytes && uri.fileByteOffset > 0) {
                        if (uri.fileByteOffset >= bytes->size())
                            return Error{std::format("image {}: offset past the end of {}", imageIndex, path.string())};
                        bytes->erase(bytes->begin(), bytes->begin() + std::ptrdiff_t(uri.fileByteOffset));
                    }
                    return bytes;
                },
                [&](const fastgltf::sources::Array& array) -> Result<std::vector<std::uint8_t>> {
                    return copy({array.bytes.data(), array.bytes.size()});
                },
                [&](const fastgltf::sources::Vector& vector) -> Result<std::vector<std::uint8_t>> {
                    return copy(vector.bytes);
                },
                [&](const fastgltf::sources::ByteView& view) -> Result<std::vector<std::uint8_t>> {
                    return copy(view.bytes);
                },
                [&](const fastgltf::sources::BufferView& view) -> Result<std::vector<std::uint8_t>> {
                    if (view.bufferViewIndex >= m_asset.bufferViews.size())
                        return Error{std::format("image {}: no such buffer view", imageIndex)};
                    return copy(fastgltf::DefaultBufferDataAdapter{}(m_asset, view.bufferViewIndex));
                },
                [&](const auto&) -> Result<std::vector<std::uint8_t>> {
                    return Error{std::format("image {}: unsupported data source", imageIndex)};
                },
            },
            image.data);
    }

    Result<void> cookTextures() {
        for (const auto& [key, path] : m_images) {
            const auto [imageIndex, srgb] = key;
            if (imageIndex >= m_asset.images.size()) return Error{std::format("no image {}", imageIndex)};
            Result<std::vector<std::uint8_t>> bytes = imageBytes(imageIndex);
            if (!bytes) return Error{bytes.error()};
            std::vector<std::string> notes;
            Result<assets::TextureImage> image = cookTexture(
                *bytes, {.format = m_options.textureFormat, .srgb = srgb, .maxSize = m_options.textureMaxSize}, m_jobs,
                &notes);
            if (!image) return Error{std::format("image {}: {}", imageIndex, image.error())};
            for (std::string& n : notes) note(std::format("{}: {}", path, n));
            m_out.textures.push_back({path, std::move(*image)});
        }
        return {};
    }

    fastgltf::Asset& m_asset;
    std::filesystem::path m_directory;
    std::string m_texturePrefix;
    const ModelOptions& m_options;
    JobSystem& m_jobs;
    CookedModel& m_out;
    std::map<std::uint32_t, Batch> m_batches;                     // by material; ordered: deterministic output
    std::map<std::pair<std::size_t, bool>, std::string> m_images; // (image, sRGB) -> asset path
    bool m_defaultMaterial = false;
};

} // namespace

Result<CookedModel> cookModel(const std::filesystem::path& source, std::string_view texturePrefix,
                              const ModelOptions& options, JobSystem& jobs) {
    if (options.lods == 0 || options.lodRatio <= 0.0f || options.lodRatio >= 1.0f || options.scale <= 0.0f)
        return Error{"cook: lods must be at least 1, the LOD ratio between 0 and 1, the scale positive"};
    CookedModel out;
    out.inputs.push_back(source);
    fastgltf::Expected<fastgltf::GltfDataBuffer> data = fastgltf::GltfDataBuffer::FromPath(source);
    if (data.error() != fastgltf::Error::None)
        return Error{std::format("{}: {}", source.string(), gltfError(data.error()))};

    fastgltf::Parser parser(fastgltf::Extensions::KHR_mesh_quantization | fastgltf::Extensions::KHR_texture_transform |
                            fastgltf::Extensions::KHR_materials_emissive_strength |
                            fastgltf::Extensions::KHR_materials_unlit);
    const std::filesystem::path directory = source.parent_path();
    fastgltf::Expected<fastgltf::Asset> asset =
        parser.loadGltf(data.get(), directory, fastgltf::Options::GenerateMeshIndices);
    if (asset.error() != fastgltf::Error::None)
        return Error{std::format("{}: {}", source.string(), gltfError(asset.error()))};

    // External buffers are read here, not by fastgltf, so that every file read is listed in the depfile.
    for (fastgltf::Buffer& buffer : asset->buffers) {
        const auto* uri = std::get_if<fastgltf::sources::URI>(&buffer.data);
        if (!uri) continue;
        if (!uri->uri->isLocalPath()) return Error{std::format("{}: a buffer is not a local file", source.string())};
        const std::filesystem::path path = directory / uri->uri->fspath();
        out.inputs.push_back(path);
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

    ModelCooker cooker(asset.get(), directory, texturePrefix, options, jobs, out);
    if (Result<void> r = cooker.run(); !r) return Error{std::format("{}: {}", source.string(), r.error())};
    return out;
}

} // namespace quench::cook
