#include "Internal.h"

#include <quench/core/Log.h>

#include <DetourAlloc.h>
#include <DetourNavMeshBuilder.h>
#include <DetourTileCacheBuilder.h>
#include <Recast.h>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <string>

namespace quench::navigation::detail {
namespace {

static_assert(sizeof(glm::vec3) == 3 * sizeof(float));

// Collects Recast's error messages for the Result.
class Context final : public rcContext {
public:
    Context() : rcContext(true) { enableTimer(false); }
    const std::string& errors() const { return m_errors; }

protected:
    void doLog(const rcLogCategory category, const char* msg, const int len) override {
        if (category != RC_LOG_ERROR) return;
        if (!m_errors.empty()) m_errors += "; ";
        m_errors.append(msg, std::size_t(len));
    }

private:
    std::string m_errors;
};

// Layers are kept uncompressed: they are small next to the meshes made from them, and the store keeps the build free
// of a compression library.
class StoreCompressor final : public dtTileCacheCompressor {
public:
    int maxCompressedSize(const int bufferSize) override { return bufferSize; }
    dtStatus compress(const unsigned char* buffer, const int bufferSize, unsigned char* compressed,
                      const int maxCompressedSize, int* compressedSize) override {
        if (bufferSize > maxCompressedSize) return DT_FAILURE | DT_BUFFER_TOO_SMALL;
        std::memcpy(compressed, buffer, std::size_t(bufferSize));
        *compressedSize = bufferSize;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char* compressed, const int compressedSize, unsigned char* buffer,
                        const int maxBufferSize, int* bufferSize) override {
        // The layer's three grids; Detour's buffer has room for a fourth it fills itself.
        if (std::int64_t(compressedSize) * 4 != std::int64_t(maxBufferSize) * 3) return DT_FAILURE | DT_INVALID_PARAM;
        std::memcpy(buffer, compressed, std::size_t(compressedSize));
        *bufferSize = compressedSize;
        return DT_SUCCESS;
    }
};

struct HeightfieldFree {
    void operator()(rcHeightfield* p) const { rcFreeHeightField(p); }
};
struct CompactFree {
    void operator()(rcCompactHeightfield* p) const { rcFreeCompactHeightfield(p); }
};
struct LayerSetFree {
    void operator()(rcHeightfieldLayerSet* p) const { rcFreeHeightfieldLayerSet(p); }
};

int layerHeaderSize() { return (int(sizeof(dtTileCacheLayerHeader)) + 3) & ~3; }

bool finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

} // namespace

void DtFree::operator()(unsigned char* data) const { dtFree(data); }

glm::vec3 Config::tileMin(int tx, int tz) const {
    return {settings.boundsMin.x + float(tx) * tileWorld, settings.boundsMin.y,
            settings.boundsMin.z + float(tz) * tileWorld};
}

glm::vec3 Config::tileMax(int tx, int tz) const {
    return {settings.boundsMin.x + float(tx + 1) * tileWorld, settings.boundsMax.y,
            settings.boundsMin.z + float(tz + 1) * tileWorld};
}

Result<Config> makeConfig(const BuildSettings& settings, glm::vec3 boundsMin, glm::vec3 boundsMax) {
    const AgentConfig& a = settings.agent;
    if (!(a.radius >= 0.0f) || !(a.height > 0.0f) || !(a.maxClimb >= 0.0f) || !(a.maxSlope >= 0.0f) ||
        !(a.maxSlope < 90.0f))
        return Error{std::format("bad agent: radius {}, height {}, max climb {}, max slope {}", a.radius, a.height,
                                 a.maxClimb, a.maxSlope)};
    if (!(settings.cellSize > 0.0f) || !(settings.cellHeight > 0.0f))
        return Error{std::format("bad cell size {} or height {}", settings.cellSize, settings.cellHeight)};
    if (settings.tileSize < 16 || settings.tileSize > 255)
        return Error{std::format("tile size {} cells (16 to 255)", settings.tileSize)};
    if (!(settings.maxError >= 0.0f)) return Error{std::format("bad max error {}", settings.maxError)};
    if (settings.maxLayersPerTile < 1 || settings.maxLayersPerTile > 255)
        return Error{std::format("max layers per tile {} (1 to 255)", settings.maxLayersPerTile)};
    if (settings.areas.size() > std::size_t(kMaxAreas))
        return Error{std::format("{} area types (at most {})", settings.areas.size(), kMaxAreas)};
    for (const AreaType& area : settings.areas)
        if (!(area.cost > 0.0f) || !std::isfinite(area.cost))
            return Error{std::format("area '{}': cost {}", area.name, area.cost)};

    Config c;
    c.settings = settings;
    if (c.settings.areas.empty()) c.settings.areas.push_back({"default", 1.0f});
    const bool givenBounds = settings.boundsMax.x > settings.boundsMin.x && settings.boundsMax.z > settings.boundsMin.z;
    if (givenBounds) {
        boundsMin = settings.boundsMin;
        boundsMax = settings.boundsMax;
    }
    if (!finite(boundsMin) || !finite(boundsMax) || !(boundsMax.x > boundsMin.x) || !(boundsMax.z > boundsMin.z) ||
        !(boundsMax.y >= boundsMin.y))
        return Error{std::format("empty bounds ({}, {}, {}) to ({}, {}, {})", boundsMin.x, boundsMin.y, boundsMin.z,
                                 boundsMax.x, boundsMax.y, boundsMax.z)};
    boundsMax.y = std::max(boundsMax.y, boundsMin.y + settings.cellHeight);
    c.tileWorld = float(settings.tileSize) * settings.cellSize;
    // A grid's own bounds (cooked data) give its tile count back despite rounding.
    const double tilesX = std::ceil(double(boundsMax.x - boundsMin.x) / c.tileWorld - 1e-3);
    const double tilesZ = std::ceil(double(boundsMax.z - boundsMin.z) / c.tileWorld - 1e-3);
    if (tilesX * tilesZ * settings.maxLayersPerTile >= double(1u << DT_TILE_BITS))
        return Error{std::format("{} x {} tiles of {} layers: too many (larger tiles or fewer layers)", tilesX, tilesZ,
                                 settings.maxLayersPerTile)};
    c.tilesX = std::max(1, int(tilesX));
    c.tilesZ = std::max(1, int(tilesZ));
    c.settings.boundsMin = boundsMin;
    c.settings.boundsMax = {boundsMin.x + float(c.tilesX) * c.tileWorld, boundsMax.y,
                            boundsMin.z + float(c.tilesZ) * c.tileWorld};
    c.walkableHeight = std::max(3, int(std::ceil(a.height / settings.cellHeight)));
    c.walkableClimb = int(std::floor(a.maxClimb / settings.cellHeight));
    c.walkableRadius = int(std::ceil(a.radius / settings.cellSize));
    c.borderSize = c.walkableRadius + 3;
    c.cosSlope = std::cos(glm::radians(a.maxSlope));
    return c;
}

Result<std::pair<glm::vec3, glm::vec3>> checkGeometry(const Geometry& g, const BuildSettings& settings) {
    if (g.indices.size() % 3 != 0) return Error{std::format("{} indices: not a triangle list", g.indices.size())};
    const std::size_t triangles = g.indices.size() / 3;
    if (!g.areas.empty() && g.areas.size() != triangles)
        return Error{std::format("{} areas for {} triangles", g.areas.size(), triangles)};
    if (g.vertices.size() > std::size_t(std::numeric_limits<int>::max()))
        return Error{std::format("{} vertices: too many", g.vertices.size())};
    const std::size_t areaCount = settings.areas.empty() ? 1 : settings.areas.size();
    const auto badArea = [&](AreaId area) { return area != kNotWalkable && area >= areaCount; };
    for (const AreaId area : g.areas)
        if (badArea(area)) return Error{std::format("area {} of {} area types", int(area), areaCount)};
    for (const AreaBox& box : g.boxes)
        if (badArea(box.area) || !finite(box.min) || !finite(box.max))
            return Error{std::format("bad area box (area {})", int(box.area))};
    for (const Link& link : g.links)
        if (badArea(link.area) || link.area == kNotWalkable || !finite(link.start) || !finite(link.end) ||
            !(link.radius > 0.0f))
            return Error{std::format("bad link {} (area {}, radius {})", link.id, int(link.area), link.radius)};
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const std::uint32_t index : g.indices) {
        if (index >= g.vertices.size()) return Error{std::format("index {} of {} vertices", index, g.vertices.size())};
        const glm::vec3 v = g.vertices[index];
        if (!finite(v)) return Error{std::format("vertex {} is not finite", index)};
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    if (triangles == 0) return Error{"no triangles"};
    return std::make_pair(lo, hi);
}

void SourceGeometry::prepare(const Config& c) {
    const std::size_t triangles = geometry.indices.size() / 3;
    triangleAreas.assign(triangles, 0);
    tileTriangles.assign(std::size_t(c.tilesX) * std::size_t(c.tilesZ), {});
    const float border = float(c.borderSize) * c.settings.cellSize;
    const glm::vec3 origin = c.settings.boundsMin;
    for (std::size_t t = 0; t < triangles; ++t) {
        const glm::vec3 v0 = geometry.vertices[geometry.indices[t * 3 + 0]];
        const glm::vec3 v1 = geometry.vertices[geometry.indices[t * 3 + 1]];
        const glm::vec3 v2 = geometry.vertices[geometry.indices[t * 3 + 2]];
        const AreaId area = geometry.areas.empty() ? kDefaultArea : geometry.areas[t];
        const glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
        const float length = glm::length(n);
        // A triangle that cannot carry the mesh is still voxelized: it is a wall or a ceiling.
        triangleAreas[t] = length > 0.0f && n.y / length > c.cosSlope ? layerArea(area) : 0;

        const glm::vec3 lo = glm::min(v0, glm::min(v1, v2)), hi = glm::max(v0, glm::max(v1, v2));
        const auto tile = [&](float v, float o, int count) {
            return int(std::clamp(std::floor((v - o) / c.tileWorld), -1.0f, float(count)));
        };
        const int x0 = std::max(maskMinX, tile(lo.x - border, origin.x, c.tilesX));
        const int x1 = std::min(maskMaxX, tile(hi.x + border, origin.x, c.tilesX));
        const int z0 = std::max(maskMinZ, tile(lo.z - border, origin.z, c.tilesZ));
        const int z1 = std::min(maskMaxZ, tile(hi.z + border, origin.z, c.tilesZ));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) tileTriangles[std::size_t(z) * std::size_t(c.tilesX) + x].push_back(int(t));
    }
}

Result<std::vector<LayerBytes>> rasterizeTile(const Config& c, const SourceGeometry& source, int tx, int tz) {
    std::vector<LayerBytes> out;
    const std::vector<int>& triangles = source.tileTriangles[std::size_t(tz) * std::size_t(c.tilesX) + tx];
    if (triangles.empty()) return out;
    const BuildSettings& s = c.settings;
    const float border = float(c.borderSize) * s.cellSize;
    glm::vec3 bmin = c.tileMin(tx, tz), bmax = c.tileMax(tx, tz);
    bmin.x -= border;
    bmin.z -= border;
    bmax.x += border;
    bmax.z += border;
    // The heights of the tile's triangles, on the grid's cell height steps so that neighbouring tiles agree.
    float low = std::numeric_limits<float>::max(), high = std::numeric_limits<float>::lowest();
    for (const int t : triangles)
        for (int k = 0; k < 3; ++k) {
            const float y = source.geometry.vertices[source.geometry.indices[std::size_t(t) * 3 + k]].y;
            low = std::min(low, y);
            high = std::max(high, y);
        }
    bmin.y = c.settings.boundsMin.y + std::floor((low - c.settings.boundsMin.y) / s.cellHeight) * s.cellHeight;
    bmax.y = std::max(high, bmin.y + s.cellHeight);
    const int size = s.tileSize + c.borderSize * 2;

    Context ctx;
    const auto failed = [&](const char* step) {
        return Error{
            std::format("tile ({}, {}): {}{}{}", tx, tz, step, ctx.errors().empty() ? "" : ": ", ctx.errors())};
    };
    std::unique_ptr<rcHeightfield, HeightfieldFree> solid(rcAllocHeightfield());
    if (!solid || !rcCreateHeightfield(&ctx, *solid, size, size, &bmin.x, &bmax.x, s.cellSize, s.cellHeight))
        return failed("heightfield");

    std::vector<int> indices;
    std::vector<unsigned char> areas;
    indices.reserve(triangles.size() * 3);
    areas.reserve(triangles.size());
    for (const int t : triangles) {
        for (int k = 0; k < 3; ++k) indices.push_back(int(source.geometry.indices[std::size_t(t) * 3 + k]));
        areas.push_back(source.triangleAreas[std::size_t(t)]);
    }
    if (!rcRasterizeTriangles(&ctx, &source.geometry.vertices[0].x, int(source.geometry.vertices.size()),
                              indices.data(), areas.data(), int(areas.size()), *solid, c.walkableClimb))
        return failed("rasterization");
    rcFilterLowHangingWalkableObstacles(&ctx, c.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, c.walkableHeight, c.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, c.walkableHeight, *solid);

    std::unique_ptr<rcCompactHeightfield, CompactFree> compact(rcAllocCompactHeightfield());
    if (!compact || !rcBuildCompactHeightfield(&ctx, c.walkableHeight, c.walkableClimb, *solid, *compact))
        return failed("compact heightfield");
    solid.reset();
    if (!rcErodeWalkableArea(&ctx, c.walkableRadius, *compact)) return failed("erosion");
    for (const AreaBox& box : source.geometry.boxes) {
        if (box.max.x < bmin.x || box.min.x > bmax.x || box.max.z < bmin.z || box.min.z > bmax.z) continue;
        rcMarkBoxArea(&ctx, &box.min.x, &box.max.x, layerArea(box.area), *compact);
    }

    std::unique_ptr<rcHeightfieldLayerSet, LayerSetFree> layers(rcAllocHeightfieldLayerSet());
    if (!layers || !rcBuildHeightfieldLayers(&ctx, *compact, c.borderSize, c.walkableHeight, *layers))
        return failed("layers");
    if (layers->nlayers > s.maxLayersPerTile)
        log::warning(kLog, "tile ({}, {}): {} floors, {} kept (BuildSettings::maxLayersPerTile)", tx, tz,
                     layers->nlayers, s.maxLayersPerTile);
    StoreCompressor compressor;
    for (int i = 0; i < std::min(layers->nlayers, s.maxLayersPerTile); ++i) {
        const rcHeightfieldLayer& layer = layers->layers[i];
        dtTileCacheLayerHeader header{};
        header.magic = DT_TILECACHE_MAGIC;
        header.version = DT_TILECACHE_VERSION;
        header.tx = tx;
        header.ty = tz;
        header.tlayer = i;
        std::memcpy(header.bmin, layer.bmin, sizeof(header.bmin));
        std::memcpy(header.bmax, layer.bmax, sizeof(header.bmax));
        header.width = (unsigned char)layer.width;
        header.height = (unsigned char)layer.height;
        header.minx = (unsigned char)layer.minx;
        header.maxx = (unsigned char)layer.maxx;
        header.miny = (unsigned char)layer.miny;
        header.maxy = (unsigned char)layer.maxy;
        header.hmin = (unsigned short)layer.hmin;
        header.hmax = (unsigned short)layer.hmax;
        unsigned char* data = nullptr;
        int dataSize = 0;
        if (dtStatusFailed(
                dtBuildTileCacheLayer(&compressor, &header, layer.heights, layer.areas, layer.cons, &data, &dataSize)))
            return failed("layer data");
        std::unique_ptr<unsigned char[], DtFree> owned(data);
        out.push_back(std::make_shared<const std::vector<std::uint8_t>>(data, data + dataSize));
    }
    return out;
}

void obstacleBounds(const Obstacle& o, const AgentConfig& agent, glm::vec3& min, glm::vec3& max) {
    if (o.shape == ObstacleShape::Cylinder) {
        const float r = o.radius + agent.radius;
        min = {o.position.x - r, o.position.y - agent.maxClimb, o.position.z - r};
        max = {o.position.x + r, o.position.y + o.height, o.position.z + r};
        return;
    }
    // A box turned by any yaw stays inside the circle around its corners.
    const float r = std::sqrt(o.halfExtents.x * o.halfExtents.x + o.halfExtents.z * o.halfExtents.z) + agent.radius;
    min = {o.position.x - r, o.position.y - o.halfExtents.y - agent.maxClimb, o.position.z - r};
    max = {o.position.x + r, o.position.y + o.halfExtents.y, o.position.z + r};
}

Result<void> checkLayer(const Config& c, const NavMeshData::Layer& layer) {
    const int headerSize = layerHeaderSize();
    if (layer.bytes.size() < std::size_t(headerSize))
        return Error{std::format("layer of tile ({}, {}): {} bytes", layer.tileX, layer.tileZ, layer.bytes.size())};
    dtTileCacheLayerHeader header;
    std::memcpy(&header, layer.bytes.data(), sizeof(header));
    if (header.magic != DT_TILECACHE_MAGIC || header.version != DT_TILECACHE_VERSION)
        return Error{std::format("layer of tile ({}, {}): not a tile cache layer", layer.tileX, layer.tileZ)};
    const std::size_t cells = std::size_t(header.width) * header.height;
    if (layer.bytes.size() != std::size_t(headerSize) + cells * 3)
        return Error{std::format("layer of tile ({}, {}): {} bytes for {} x {} cells", layer.tileX, layer.tileZ,
                                 layer.bytes.size(), int(header.width), int(header.height))};
    if (header.tx != layer.tileX || header.ty != layer.tileZ || header.tlayer != layer.index || layer.tileX < 0 ||
        layer.tileZ < 0 || layer.tileX >= c.tilesX || layer.tileZ >= c.tilesZ || layer.index < 0 ||
        layer.index >= c.settings.maxLayersPerTile || header.width != c.settings.tileSize ||
        header.height != c.settings.tileSize || header.maxx >= header.width || header.maxy >= header.height)
        return Error{std::format("layer {} of tile ({}, {}): does not fit the tile grid", layer.index, layer.tileX,
                                 layer.tileZ)};
    return {};
}

Result<TileData> buildTile(const Config& c, std::span<const std::uint8_t> bytes, std::span<const Obstacle> obstacles,
                           std::span<const Link> links) {
    dtTileCacheAlloc alloc;
    StoreCompressor compressor;
    dtTileCacheLayer* rawLayer = nullptr;
    if (dtStatusFailed(dtDecompressTileCacheLayer(&alloc, &compressor, const_cast<unsigned char*>(bytes.data()),
                                                  int(bytes.size()), &rawLayer)))
        return Error{"a tile layer does not decode"};
    const auto freeLayer = [&alloc](dtTileCacheLayer* p) { dtFreeTileCacheLayer(&alloc, p); };
    std::unique_ptr<dtTileCacheLayer, decltype(freeLayer)> layer(rawLayer, freeLayer);
    const dtTileCacheLayerHeader& header = *layer->header;
    const BuildSettings& s = c.settings;
    const auto failed = [&](const char* step) {
        return Error{std::format("tile ({}, {}) layer {}: {}", header.tx, header.ty, header.tlayer, step)};
    };

    for (const Obstacle& o : obstacles) {
        const std::uint8_t area = layerArea(o.area);
        if (o.shape == ObstacleShape::Cylinder) {
            const float base[3] = {o.position.x, o.position.y - s.agent.maxClimb, o.position.z};
            dtMarkCylinderArea(*layer, header.bmin, s.cellSize, s.cellHeight, base, o.radius + s.agent.radius,
                               o.height + s.agent.maxClimb, area);
        } else {
            const float center[3] = {o.position.x, o.position.y - s.agent.maxClimb * 0.5f, o.position.z};
            const float half[3] = {o.halfExtents.x + s.agent.radius, o.halfExtents.y + s.agent.maxClimb * 0.5f,
                                   o.halfExtents.z + s.agent.radius};
            // DetourTileCache's oriented boxes (dtTileCache::addBoxObstacle): this turns +X toward -Z, as a turn about
            // +Y does.
            const float cosHalf = std::cos(0.5f * o.yaw), sinHalf = std::sin(-0.5f * o.yaw);
            const float rotAux[2] = {cosHalf * sinHalf, cosHalf * cosHalf - 0.5f};
            dtMarkBoxArea(*layer, header.bmin, s.cellSize, s.cellHeight, center, half, rotAux, area);
        }
    }

    if (dtStatusFailed(dtBuildTileCacheRegions(&alloc, *layer, c.walkableClimb))) return failed("regions");
    const auto freeContours = [&alloc](dtTileCacheContourSet* p) { dtFreeTileCacheContourSet(&alloc, p); };
    std::unique_ptr<dtTileCacheContourSet, decltype(freeContours)> contours(dtAllocTileCacheContourSet(&alloc),
                                                                            freeContours);
    if (!contours || dtStatusFailed(dtBuildTileCacheContours(&alloc, *layer, c.walkableClimb, s.maxError, *contours)))
        return failed("contours");
    const auto freeMesh = [&alloc](dtTileCachePolyMesh* p) { dtFreeTileCachePolyMesh(&alloc, p); };
    std::unique_ptr<dtTileCachePolyMesh, decltype(freeMesh)> mesh(dtAllocTileCachePolyMesh(&alloc), freeMesh);
    if (!mesh || dtStatusFailed(dtBuildTileCachePolyMesh(&alloc, *contours, *mesh))) return failed("polygons");
    if (mesh->npolys == 0) return TileData{};

    for (int i = 0; i < mesh->npolys; ++i) {
        const AreaId area = AreaId(std::max(1, int(mesh->areas[i])) - 1);
        mesh->areas[i] = area;
        mesh->flags[i] = areaFlag(area);
    }

    std::vector<float> linkVerts, linkRadii;
    std::vector<unsigned short> linkFlags;
    std::vector<unsigned char> linkAreas, linkDirs;
    std::vector<unsigned int> linkIds;
    for (const Link& link : links) {
        if (link.start.x < header.bmin[0] || link.start.x >= header.bmax[0] || link.start.z < header.bmin[2] ||
            link.start.z >= header.bmax[2])
            continue;
        linkVerts.insert(linkVerts.end(),
                         {link.start.x, link.start.y, link.start.z, link.end.x, link.end.y, link.end.z});
        linkRadii.push_back(link.radius);
        linkFlags.push_back(areaFlag(link.area));
        linkAreas.push_back(link.area);
        linkDirs.push_back(link.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
        linkIds.push_back(link.id);
    }

    dtNavMeshCreateParams params{};
    params.verts = mesh->verts;
    params.vertCount = mesh->nverts;
    params.polys = mesh->polys;
    params.polyAreas = mesh->areas;
    params.polyFlags = mesh->flags;
    params.polyCount = mesh->npolys;
    params.nvp = DT_VERTS_PER_POLYGON;
    params.offMeshConVerts = linkVerts.data();
    params.offMeshConRad = linkRadii.data();
    params.offMeshConFlags = linkFlags.data();
    params.offMeshConAreas = linkAreas.data();
    params.offMeshConDir = linkDirs.data();
    params.offMeshConUserID = linkIds.data();
    params.offMeshConCount = int(linkIds.size());
    params.walkableHeight = s.agent.height;
    params.walkableRadius = s.agent.radius;
    params.walkableClimb = s.agent.maxClimb;
    params.tileX = header.tx;
    params.tileY = header.ty;
    params.tileLayer = header.tlayer;
    std::memcpy(params.bmin, header.bmin, sizeof(params.bmin));
    std::memcpy(params.bmax, header.bmax, sizeof(params.bmax));
    params.cs = s.cellSize;
    params.ch = s.cellHeight;
    params.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&params, &data, &size)) return failed("tile data");
    TileData out;
    out.data.reset(data);
    out.size = size;
    out.hash = XXH64(data, std::size_t(size), 0);
    out.polygons = mesh->npolys;
    return out;
}

dtQueryFilter toDetour(const Filter& filter) {
    dtQueryFilter out;
    for (int i = 0; i < kMaxAreas; ++i) out.setAreaCost(i, filter.costs[std::size_t(i)]);
    out.setIncludeFlags(filter.includeAreas);
    out.setExcludeFlags(filter.excludeAreas);
    return out;
}

} // namespace quench::navigation::detail
