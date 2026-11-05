#include <quench/assets/Model.h>

#include <meshoptimizer.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

// Layout (little endian, every field 4-byte aligned):
//   u32 magic "QMDL", u32 version, u32 mesh count, u32 material count, f32 bounds min[3], max[3]
//   materials: string name, f32 base color[4], emissive[3], roughness, metallic, alpha cutoff,
//              u8 alpha mode, u8 double sided, u16 0, string textures[4] (base color, metallic-roughness, normal,
//              emissive)
//   meshes: string name, u32 material, u32 vertex count, u32 index count, u32 LOD count, f32 bounds min[3], max[3],
//           LODs (u32 first index, u32 index count, f32 error), u32 size + meshopt vertex stream (padded),
//           u32 size + meshopt index stream (padded)
//   string: u32 length + bytes (padded)
namespace quench::assets {
namespace {

constexpr std::uint32_t kMagic = 0x4C444D51; // "QMDL"
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaxCount = 1u << 24;

static_assert(std::endian::native == std::endian::little, "cooked models are read in place as little endian");

class Writer {
public:
    void u32(std::uint32_t v) { bytes(&v, 4); }
    void f32(float v) { bytes(&v, 4); }
    void vec(const float* v, int n) {
        for (int i = 0; i < n; ++i) f32(v[i]);
    }
    void string(const std::string& s) {
        u32(std::uint32_t(s.size()));
        bytes(s.data(), s.size());
        pad();
    }
    void blob(std::span<const std::uint8_t> data) {
        u32(std::uint32_t(data.size()));
        bytes(data.data(), data.size());
        pad();
    }
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), p, p + size);
    }
    void pad() {
        while (out.size() % 4 != 0) out.push_back(0);
    }
    std::vector<std::uint8_t> out;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> file) : m_file(file) {}

    bool u32(std::uint32_t& v) { return bytes(&v, 4); }
    bool f32(float& v) { return bytes(&v, 4); }
    bool vec(float* v, int n) {
        for (int i = 0; i < n; ++i)
            if (!f32(v[i])) return false;
        return true;
    }
    bool string(std::string& s) {
        std::uint32_t size = 0;
        if (!u32(size) || size > remaining()) return false;
        s.assign(reinterpret_cast<const char*>(m_file.data() + m_at), size);
        m_at += size;
        return align();
    }
    bool blob(std::span<const std::uint8_t>& data) {
        std::uint32_t size = 0;
        if (!u32(size) || size > remaining()) return false;
        data = m_file.subspan(m_at, size);
        m_at += size;
        return align();
    }
    bool bytes(void* data, std::size_t size) {
        if (size > remaining()) return false;
        std::memcpy(data, m_file.data() + m_at, size);
        m_at += size;
        return true;
    }
    std::size_t remaining() const { return m_file.size() - m_at; }

private:
    bool align() {
        const std::size_t aligned = (m_at + 3) & ~std::size_t(3);
        if (aligned > m_file.size()) return false;
        m_at = aligned;
        return true;
    }

    std::span<const std::uint8_t> m_file;
    std::size_t m_at = 0;
};

} // namespace

Result<std::vector<std::uint8_t>> writeModel(const ModelData& model) {
    Writer w;
    w.u32(kMagic);
    w.u32(kVersion);
    w.u32(std::uint32_t(model.meshes.size()));
    w.u32(std::uint32_t(model.materials.size()));
    w.vec(&model.boundsMin.x, 3);
    w.vec(&model.boundsMax.x, 3);
    for (const ModelMaterial& m : model.materials) {
        w.string(m.name);
        w.vec(&m.baseColor.x, 4);
        w.vec(&m.emissive.x, 3);
        w.f32(m.roughness);
        w.f32(m.metallic);
        w.f32(m.alphaCutoff);
        const std::uint8_t flags[4] = {std::uint8_t(m.alphaMode), std::uint8_t(m.doubleSided ? 1 : 0), 0, 0};
        w.bytes(flags, 4);
        w.string(m.baseColorTexture);
        w.string(m.metallicRoughnessTexture);
        w.string(m.normalTexture);
        w.string(m.emissiveTexture);
    }
    for (const ModelMesh& mesh : model.meshes) {
        if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0 || mesh.lods.empty())
            return Error{std::format("mesh '{}': {} vertices, {} indices, {} LODs", mesh.name, mesh.vertices.size(),
                                     mesh.indices.size(), mesh.lods.size())};
        if (mesh.material >= model.materials.size())
            return Error{std::format("mesh '{}': material {} of {}", mesh.name, mesh.material, model.materials.size())};
        for (const ModelLod& lod : mesh.lods)
            if (lod.indexCount % 3 != 0 || std::uint64_t(lod.firstIndex) + lod.indexCount > mesh.indices.size())
                return Error{std::format("mesh '{}': LOD range {} + {} of {} indices", mesh.name, lod.firstIndex,
                                         lod.indexCount, mesh.indices.size())};
        w.string(mesh.name);
        w.u32(mesh.material);
        w.u32(std::uint32_t(mesh.vertices.size()));
        w.u32(std::uint32_t(mesh.indices.size()));
        w.u32(std::uint32_t(mesh.lods.size()));
        w.vec(&mesh.boundsMin.x, 3);
        w.vec(&mesh.boundsMax.x, 3);
        for (const ModelLod& lod : mesh.lods) {
            w.u32(lod.firstIndex);
            w.u32(lod.indexCount);
            w.f32(lod.error);
        }
        std::vector<std::uint8_t> stream(meshopt_encodeVertexBufferBound(mesh.vertices.size(), sizeof(ModelVertex)));
        stream.resize(meshopt_encodeVertexBufferLevel(stream.data(), stream.size(), mesh.vertices.data(),
                                                      mesh.vertices.size(), sizeof(ModelVertex), 2, 1));
        w.blob(stream);
        stream.assign(meshopt_encodeIndexBufferBound(mesh.indices.size(), mesh.vertices.size()), 0);
        stream.resize(
            meshopt_encodeIndexBuffer(stream.data(), stream.size(), mesh.indices.data(), mesh.indices.size()));
        w.blob(stream);
    }
    return std::move(w.out);
}

Result<ModelData> readModel(std::span<const std::uint8_t> file) {
    Reader r(file);
    std::uint32_t magic = 0, version = 0, meshCount = 0, materialCount = 0;
    if (!r.u32(magic) || magic != kMagic) return Error{"not a cooked model (.qmodel)"};
    if (!r.u32(version) || version != kVersion)
        return Error{std::format("cooked model version {} (this engine reads {})", version, kVersion)};
    ModelData model;
    if (!r.u32(meshCount) || !r.u32(materialCount) || meshCount > kMaxCount || materialCount > kMaxCount ||
        !r.vec(&model.boundsMin.x, 3) || !r.vec(&model.boundsMax.x, 3))
        return Error{"truncated model header"};
    // A material takes at least 64 bytes and a mesh at least 68: larger counts are corrupt.
    if (std::uint64_t(materialCount) * 64 + std::uint64_t(meshCount) * 68 > r.remaining())
        return Error{std::format("corrupt model: {} materials and {} meshes in {} bytes", materialCount, meshCount,
                                 r.remaining())};
    const auto truncated = [](const char* what, std::uint32_t i) {
        return Error{std::format("truncated or corrupt model: {} {}", what, i)};
    };
    model.materials.resize(materialCount);
    for (std::uint32_t i = 0; i < materialCount; ++i) {
        ModelMaterial& m = model.materials[i];
        std::uint8_t flags[4] = {};
        if (!r.string(m.name) || !r.vec(&m.baseColor.x, 4) || !r.vec(&m.emissive.x, 3) || !r.f32(m.roughness) ||
            !r.f32(m.metallic) || !r.f32(m.alphaCutoff) || !r.bytes(flags, 4) || flags[0] > 2 ||
            !r.string(m.baseColorTexture) || !r.string(m.metallicRoughnessTexture) || !r.string(m.normalTexture) ||
            !r.string(m.emissiveTexture))
            return truncated("material", i);
        m.alphaMode = AlphaMode(flags[0]);
        m.doubleSided = flags[1] != 0;
    }
    model.meshes.resize(meshCount);
    for (std::uint32_t i = 0; i < meshCount; ++i) {
        ModelMesh& mesh = model.meshes[i];
        std::uint32_t vertexCount = 0, indexCount = 0, lodCount = 0;
        if (!r.string(mesh.name) || !r.u32(mesh.material) || !r.u32(vertexCount) || !r.u32(indexCount) ||
            !r.u32(lodCount) || !r.vec(&mesh.boundsMin.x, 3) || !r.vec(&mesh.boundsMax.x, 3))
            return truncated("mesh", i);
        if (mesh.material >= materialCount || vertexCount == 0 || vertexCount > kMaxCount || indexCount == 0 ||
            indexCount % 3 != 0 || indexCount > kMaxCount * 3 || lodCount == 0 || lodCount > 64)
            return Error{std::format("corrupt model: mesh {} has material {}, {} vertices, {} indices, {} LODs", i,
                                     mesh.material, vertexCount, indexCount, lodCount)};
        mesh.lods.resize(lodCount);
        for (ModelLod& lod : mesh.lods) {
            if (!r.u32(lod.firstIndex) || !r.u32(lod.indexCount) || !r.f32(lod.error)) return truncated("mesh", i);
            if (std::uint64_t(lod.firstIndex) + lod.indexCount > indexCount || lod.indexCount % 3 != 0)
                return Error{
                    std::format("corrupt model: mesh {} LOD range {} + {}", i, lod.firstIndex, lod.indexCount)};
        }
        std::span<const std::uint8_t> vertexStream, indexStream;
        if (!r.blob(vertexStream) || !r.blob(indexStream)) return truncated("mesh", i);
        // The codecs need at least one byte per triangle and far more than a byte per 64 vertices: larger counts are
        // corrupt, and allocating for them could take gigabytes.
        if (vertexCount > 64 * vertexStream.size() || indexCount / 3 > indexStream.size())
            return Error{std::format("corrupt model: mesh {} has {} vertices in {} bytes, {} indices in {} bytes", i,
                                     vertexCount, vertexStream.size(), indexCount, indexStream.size())};
        mesh.vertices.resize(vertexCount);
        mesh.indices.resize(indexCount);
        if (meshopt_decodeVertexBuffer(mesh.vertices.data(), vertexCount, sizeof(ModelVertex), vertexStream.data(),
                                       vertexStream.size()) != 0)
            return Error{std::format("corrupt model: vertex stream of mesh {}", i)};
        if (meshopt_decodeIndexBuffer(mesh.indices.data(), indexCount, sizeof(std::uint32_t), indexStream.data(),
                                      indexStream.size()) != 0)
            return Error{std::format("corrupt model: index stream of mesh {}", i)};
        if (std::ranges::any_of(mesh.indices, [vertexCount](std::uint32_t index) { return index >= vertexCount; }))
            return Error{std::format("corrupt model: mesh {} has an index out of range", i)};
    }
    if (r.remaining() != 0) return Error{std::format("corrupt model: {} bytes after the last mesh", r.remaining())};
    return model;
}

} // namespace quench::assets
