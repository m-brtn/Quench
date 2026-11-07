#include <quench/navigation/NavMesh.h>

#include "Internal.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <bit>
#include <cstring>
#include <format>

// The .qnav file (little endian): u32 magic "QNAV", u32 version, u64 payload size, u64 XXH64 of the payload, the
// payload. Inside it values are 4 bytes each; strings and blobs are a u32 size, the bytes and padding to 4 bytes.
// Payload: the build settings (agent radius, height, max climb, max slope; cell size, cell height, tile size, max
// error, max layers per tile; bounds min and max), tilesX, tilesZ, the area types (name, cost), the links (start, end,
// radius, flags (1: both ways), area, id), the layers (tile x, tile z, index, bytes).

namespace quench::navigation {
namespace {

static_assert(std::endian::native == std::endian::little, "navmesh files are little endian");

constexpr std::uint32_t kMagic = 0x56414E51; // "QNAV"
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderSize = 24;

class Writer {
public:
    void u32(std::uint32_t v) { bytes(&v, 4); }
    void i32(std::int32_t v) { bytes(&v, 4); }
    void f32(float v) { bytes(&v, 4); }
    void vec3(glm::vec3 v) {
        f32(v.x);
        f32(v.y);
        f32(v.z);
    }
    void blob(const void* data, std::size_t size) {
        u32(std::uint32_t(size));
        bytes(data, size);
        while (out.size() % 4 != 0) out.push_back(0);
    }
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), p, p + size);
    }
    std::vector<std::uint8_t> out;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : m_data(data) {}
    bool u32(std::uint32_t& v) { return bytes(&v, 4); }
    bool i32(std::int32_t& v) { return bytes(&v, 4); }
    bool f32(float& v) { return bytes(&v, 4); }
    bool vec3(glm::vec3& v) { return f32(v.x) && f32(v.y) && f32(v.z); }
    bool blob(std::span<const std::uint8_t>& data) {
        std::uint32_t size = 0;
        if (!u32(size) || size > remaining()) return false;
        data = m_data.subspan(m_at, size);
        m_at = std::min(m_data.size(), (m_at + size + 3) & ~std::size_t(3));
        return true;
    }
    bool bytes(void* data, std::size_t size) {
        if (size > remaining()) return false;
        std::memcpy(data, m_data.data() + m_at, size);
        m_at += size;
        return true;
    }
    std::size_t remaining() const { return m_data.size() - m_at; }

private:
    std::span<const std::uint8_t> m_data;
    std::size_t m_at = 0;
};

} // namespace

std::uint64_t NavMeshData::memoryBytes() const {
    std::uint64_t bytes = sizeof(*this) + links.size() * sizeof(Link);
    for (const Layer& layer : layers) bytes += sizeof(Layer) + layer.bytes.size();
    return bytes;
}

Result<NavMeshData> buildNavMeshData(const Geometry& geometry, const BuildSettings& settings, JobSystem& jobs) {
    Result<NavMesh> mesh = NavMesh::build(geometry, settings, jobs);
    if (!mesh) return Error{mesh.error()};
    mesh->finish();
    return mesh->data();
}

std::vector<std::uint8_t> writeNavMeshData(const NavMeshData& data) {
    Writer w;
    const BuildSettings& s = data.settings;
    w.f32(s.agent.radius);
    w.f32(s.agent.height);
    w.f32(s.agent.maxClimb);
    w.f32(s.agent.maxSlope);
    w.f32(s.cellSize);
    w.f32(s.cellHeight);
    w.u32(std::uint32_t(s.tileSize));
    w.f32(s.maxError);
    w.u32(std::uint32_t(s.maxLayersPerTile));
    w.vec3(s.boundsMin);
    w.vec3(s.boundsMax);
    w.u32(std::uint32_t(data.tilesX));
    w.u32(std::uint32_t(data.tilesZ));
    w.u32(std::uint32_t(s.areas.size()));
    for (const AreaType& area : s.areas) {
        w.blob(area.name.data(), area.name.size());
        w.f32(area.cost);
    }
    w.u32(std::uint32_t(data.links.size()));
    for (const Link& link : data.links) {
        w.vec3(link.start);
        w.vec3(link.end);
        w.f32(link.radius);
        w.u32(link.bidirectional ? 1u : 0u);
        w.u32(link.area);
        w.u32(link.id);
    }
    w.u32(std::uint32_t(data.layers.size()));
    for (const NavMeshData::Layer& layer : data.layers) {
        w.i32(layer.tileX);
        w.i32(layer.tileZ);
        w.i32(layer.index);
        w.blob(layer.bytes.data(), layer.bytes.size());
    }

    Writer file;
    file.u32(kMagic);
    file.u32(kVersion);
    const std::uint64_t size = w.out.size();
    const std::uint64_t hash = XXH64(w.out.data(), w.out.size(), 0);
    file.bytes(&size, 8);
    file.bytes(&hash, 8);
    file.bytes(w.out.data(), w.out.size());
    return std::move(file.out);
}

Result<std::shared_ptr<const NavMeshData>> readNavMeshData(std::span<const std::uint8_t> file) {
    Reader header(file);
    std::uint32_t magic = 0, version = 0;
    std::uint64_t size = 0, hash = 0;
    if (!header.u32(magic) || magic != kMagic) return Error{"not a navmesh file"};
    if (!header.u32(version) || version != kVersion)
        return Error{std::format("navmesh version {} (this engine reads {})", version, kVersion)};
    if (!header.bytes(&size, 8) || !header.bytes(&hash, 8)) return Error{"truncated navmesh header"};
    if (size != header.remaining())
        return Error{std::format("corrupt navmesh: {} payload bytes, the header says {}", header.remaining(), size)};
    const std::span<const std::uint8_t> payload = file.subspan(kHeaderSize);
    if (XXH64(payload.data(), payload.size(), 0) != hash) return Error{"corrupt navmesh: checksum"};

    auto data = std::make_shared<NavMeshData>();
    BuildSettings& s = data->settings;
    Reader r(payload);
    std::uint32_t tileSize = 0, maxLayers = 0, tilesX = 0, tilesZ = 0, areaCount = 0, linkCount = 0, layerCount = 0;
    bool ok = r.f32(s.agent.radius) && r.f32(s.agent.height) && r.f32(s.agent.maxClimb) && r.f32(s.agent.maxSlope) &&
              r.f32(s.cellSize) && r.f32(s.cellHeight) && r.u32(tileSize) && r.f32(s.maxError) && r.u32(maxLayers) &&
              r.vec3(s.boundsMin) && r.vec3(s.boundsMax) && r.u32(tilesX) && r.u32(tilesZ) && r.u32(areaCount) &&
              areaCount <= std::uint32_t(kMaxAreas);
    if (!ok) return Error{"corrupt navmesh: settings"};
    s.tileSize = int(std::min<std::uint32_t>(tileSize, 1u << 16));
    s.maxLayersPerTile = int(std::min<std::uint32_t>(maxLayers, 1u << 16));
    data->tilesX = int(std::min<std::uint32_t>(tilesX, 1u << 30));
    data->tilesZ = int(std::min<std::uint32_t>(tilesZ, 1u << 30));
    for (std::uint32_t i = 0; i < areaCount; ++i) {
        std::span<const std::uint8_t> name;
        AreaType area;
        if (!r.blob(name) || name.size() > 256 || !r.f32(area.cost)) return Error{"corrupt navmesh: area types"};
        area.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
        s.areas.push_back(std::move(area));
    }
    if (!r.u32(linkCount) || std::uint64_t(linkCount) * 40 > r.remaining()) return Error{"corrupt navmesh: links"};
    for (std::uint32_t i = 0; i < linkCount; ++i) {
        Link link;
        std::uint32_t flags = 0, area = 0;
        if (!r.vec3(link.start) || !r.vec3(link.end) || !r.f32(link.radius) || !r.u32(flags) || !r.u32(area) ||
            !r.u32(link.id) || area >= std::uint32_t(kMaxAreas))
            return Error{"corrupt navmesh: links"};
        link.bidirectional = (flags & 1u) != 0;
        link.area = AreaId(area);
        data->links.push_back(link);
    }
    if (!r.u32(layerCount) || std::uint64_t(layerCount) * 16 > r.remaining()) return Error{"corrupt navmesh: layers"};
    data->layers.reserve(layerCount);
    for (std::uint32_t i = 0; i < layerCount; ++i) {
        NavMeshData::Layer layer;
        std::span<const std::uint8_t> bytes;
        if (!r.i32(layer.tileX) || !r.i32(layer.tileZ) || !r.i32(layer.index) || !r.blob(bytes))
            return Error{"corrupt navmesh: layers"};
        layer.bytes.assign(bytes.begin(), bytes.end());
        data->layers.push_back(std::move(layer));
    }
    if (r.remaining() != 0) return Error{std::format("corrupt navmesh: {} bytes left over", r.remaining())};

    Result<detail::Config> config = detail::makeConfig(s, s.boundsMin, s.boundsMax);
    if (!config) return Error{std::format("navmesh settings: {}", config.error())};
    if (config->tilesX != data->tilesX || config->tilesZ != data->tilesZ)
        return Error{std::format("navmesh of {} x {} tiles whose bounds make {} x {}", data->tilesX, data->tilesZ,
                                 config->tilesX, config->tilesZ)};
    for (const Link& link : data->links)
        if (link.area >= config->settings.areas.size())
            return Error{
                std::format("navmesh link {}: area {} of {}", link.id, int(link.area), config->settings.areas.size())};
    for (const NavMeshData::Layer& layer : data->layers)
        if (Result<void> check = detail::checkLayer(*config, layer); !check) return Error{check.error()};
    return std::shared_ptr<const NavMeshData>(std::move(data));
}

} // namespace quench::navigation
