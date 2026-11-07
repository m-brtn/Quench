#include "Cook.h"

#include "Gltf.h"

#include <quench/core/Json.h>

#include <fastgltf/tools.hpp>
#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <map>

namespace quench::cook {

namespace {

using navigation::AreaId;

Result<std::vector<std::uint8_t>> readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{std::format("cannot open {}", path.string())};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Reads values into settings, keeping the first error.
struct Reader {
    std::string error;

    bool ok() const { return error.empty(); }
    void fail(std::string message) {
        if (ok()) error = std::move(message);
    }
    void keys(const json::Value& v, std::string_view what, std::initializer_list<std::string_view> allowed) {
        if (!v.isObject()) return fail(std::format("{}: not an object", what));
        for (const auto& [key, value] : v.object)
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                return fail(std::format("{}: unknown key '{}'", what, key));
    }
    // Absent: the target keeps its value.
    void number(float& target, const json::Value& v, std::string_view what) {
        if (v.isNull()) return;
        if (!v.isNumber() || !std::isfinite(v.number)) return fail(std::format("{}: not a number", what));
        target = float(v.number);
    }
    void whole(int& target, const json::Value& v, std::string_view what) {
        if (v.isNull()) return;
        if (!v.isNumber() || v.number != std::floor(v.number) || std::abs(v.number) > 1e9)
            return fail(std::format("{}: not a whole number", what));
        target = int(v.number);
    }
    glm::vec3 vec3(const json::Value& v, std::string_view what) {
        glm::vec3 out{0.0f};
        if (!v.isArray() || v.array.size() != 3 ||
            !std::all_of(v.array.begin(), v.array.end(), [](const json::Value& e) { return e.isNumber(); })) {
            fail(std::format("{}: not [x, y, z]", what));
            return out;
        }
        for (int i = 0; i < 3; ++i) out[i] = float(v.array[std::size_t(i)].number);
        return out;
    }
};

Result<AreaId> areaOf(const NavMeshSettings& s, const json::Value& v, std::string_view what) {
    if (v.isNull()) return navigation::kDefaultArea;
    const std::string name = v.stringOr("");
    if (name == "none") return navigation::kNotWalkable;
    for (std::size_t i = 0; i < s.build.areas.size(); ++i)
        if (s.build.areas[i].name == name) return AreaId(i);
    return Error{std::format("{}: no area type '{}'", what, v.stringOr("(not a name)"))};
}

// A surface's area by the first of its names the settings know; nullopt: dropped ("ignore").
struct SurfaceAreas {
    const NavMeshSettings& settings;
    std::map<std::string, int, std::less<>> used; // name -> triangles, for the notes

    std::optional<AreaId> of(std::initializer_list<std::string_view> names) {
        for (const std::string_view name : names) {
            if (name.empty()) continue;
            const auto it = std::find_if(settings.surfaces.begin(), settings.surfaces.end(),
                                         [name](const auto& s) { return s.first == name; });
            if (it == settings.surfaces.end()) continue;
            ++used[it->first];
            if (it->second == NavMeshSettings::kIgnore) return std::nullopt;
            return AreaId(it->second);
        }
        return navigation::kDefaultArea;
    }
};

void addTriangles(navigation::Geometry& g, std::span<const glm::vec3> vertices, std::span<const std::uint32_t> indices,
                  AreaId area) {
    const auto base = std::uint32_t(g.vertices.size());
    g.vertices.insert(g.vertices.end(), vertices.begin(), vertices.end());
    for (const std::uint32_t index : indices) g.indices.push_back(base + index);
    g.areas.insert(g.areas.end(), indices.size() / 3, area);
}

Result<void> gltfGeometry(const std::filesystem::path& source, SurfaceAreas& surfaces, navigation::Geometry& g,
                          CookedNavMesh& out) {
    Result<fastgltf::Asset> loaded = loadGltf(source, fastgltf::Options::None, out.inputs);
    if (!loaded) return Error{loaded.error()};
    fastgltf::Asset& asset = *loaded;
    if (asset.scenes.empty()) return Error{"the file has no scene"};
    const std::size_t scene = asset.defaultScene.value_or(0);
    if (scene >= asset.scenes.size()) return Error{"the default scene does not exist"};
    Result<void> status{};
    fastgltf::iterateSceneNodes(
        asset, scene, fastgltf::math::fmat4x4(), [&](fastgltf::Node& node, const fastgltf::math::fmat4x4& matrix) {
            if (!status || !node.meshIndex) return;
            if (node.skinIndex) {
                out.notes.push_back(std::format("node '{}': a skinned mesh; left out", node.name));
                return;
            }
            const glm::mat4 transform = toGlm(matrix);
            const bool flip = glm::determinant(glm::mat3(transform)) < 0.0f;
            const fastgltf::Mesh& mesh = asset.meshes[*node.meshIndex];
            for (std::size_t p = 0; p < mesh.primitives.size() && status; ++p) {
                const fastgltf::Primitive& prim = mesh.primitives[p];
                const std::string what = std::format("mesh '{}' primitive {}", mesh.name, p);
                if (prim.type != fastgltf::PrimitiveType::Triangles) {
                    out.notes.push_back(std::format("{}: not a triangle list; left out", what));
                    continue;
                }
                const std::string_view material = prim.materialIndex && *prim.materialIndex < asset.materials.size()
                                                      ? std::string_view(asset.materials[*prim.materialIndex].name)
                                                      : std::string_view();
                const std::optional<AreaId> area = surfaces.of({node.name, mesh.name, material});
                if (!area) continue;
                const auto position = prim.findAttribute("POSITION");
                if (position == prim.attributes.end()) {
                    status = Error{std::format("{}: no POSITION", what)};
                    return;
                }
                const fastgltf::Accessor& positions = asset.accessors.at(position->accessorIndex);
                if (positions.type != fastgltf::AccessorType::Vec3) {
                    status = Error{std::format("{}: POSITION is not a vec3", what)};
                    return;
                }
                std::vector<glm::vec3> vertices(positions.count);
                fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                    asset, positions, [&](const fastgltf::math::fvec3& v, std::size_t i) {
                        vertices[i] = glm::vec3(transform * glm::vec4(v[0], v[1], v[2], 1.0f));
                    });
                std::vector<std::uint32_t> indices;
                if (prim.indicesAccessor) {
                    const fastgltf::Accessor& accessor = asset.accessors.at(*prim.indicesAccessor);
                    indices.resize(accessor.count);
                    fastgltf::iterateAccessorWithIndex<std::uint32_t>(
                        asset, accessor, [&](std::uint32_t index, std::size_t i) { indices[i] = index; });
                } else {
                    indices.resize(vertices.size());
                    for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = std::uint32_t(i);
                }
                if (indices.size() % 3 != 0) {
                    status = Error{std::format("{}: {} indices", what, indices.size())};
                    return;
                }
                for (const std::uint32_t index : indices)
                    if (index >= vertices.size()) {
                        status = Error{std::format("{}: index {} out of range", what, index)};
                        return;
                    }
                if (flip)
                    for (std::size_t i = 0; i < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
                addTriangles(g, vertices, indices, *area);
            }
        });
    return status;
}

Result<void> modelGeometry(const std::filesystem::path& source, SurfaceAreas& surfaces, navigation::Geometry& g) {
    Result<std::vector<std::uint8_t>> bytes = readBytes(source);
    if (!bytes) return Error{bytes.error()};
    Result<assets::ModelData> model = assets::readModel(*bytes);
    if (!model) return Error{std::format("{}: {}", source.string(), model.error())};
    for (const assets::ModelMesh& mesh : model->meshes) {
        if (mesh.lods.empty()) continue;
        const std::string_view material =
            mesh.material < model->materials.size() ? std::string_view(model->materials[mesh.material].name) : "";
        const std::optional<AreaId> area = surfaces.of({mesh.name, material});
        if (!area) continue;
        std::vector<glm::vec3> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const assets::ModelVertex& v : mesh.vertices) vertices.push_back(v.position);
        const assets::ModelLod& lod = mesh.lods[0];
        addTriangles(g, vertices, std::span(mesh.indices).subspan(lod.firstIndex, lod.indexCount), *area);
    }
    return {};
}

} // namespace

Result<NavMeshSettings> parseNavMeshSettings(std::string_view text) {
    Result<json::Value> parsed = json::parse(std::string(text));
    if (!parsed) return Error{parsed.error()};
    const json::Value& root = *parsed;
    Reader r;
    r.keys(root, "settings",
           {"agent", "cellSize", "cellHeight", "tileSize", "maxError", "maxLayersPerTile", "boundsMin", "boundsMax",
            "areas", "surfaces", "boxes", "links"});
    NavMeshSettings s;
    navigation::BuildSettings& b = s.build;
    if (root.has("agent")) {
        const json::Value& a = root["agent"];
        r.keys(a, "agent", {"radius", "height", "maxClimb", "maxSlope"});
        r.number(b.agent.radius, a["radius"], "agent.radius");
        r.number(b.agent.height, a["height"], "agent.height");
        r.number(b.agent.maxClimb, a["maxClimb"], "agent.maxClimb");
        r.number(b.agent.maxSlope, a["maxSlope"], "agent.maxSlope");
    }
    r.number(b.cellSize, root["cellSize"], "cellSize");
    r.number(b.cellHeight, root["cellHeight"], "cellHeight");
    r.whole(b.tileSize, root["tileSize"], "tileSize");
    r.number(b.maxError, root["maxError"], "maxError");
    r.whole(b.maxLayersPerTile, root["maxLayersPerTile"], "maxLayersPerTile");
    if (root.has("boundsMin") || root.has("boundsMax")) {
        b.boundsMin = r.vec3(root["boundsMin"], "boundsMin");
        b.boundsMax = r.vec3(root["boundsMax"], "boundsMax");
    }
    if (root.has("areas") && !root["areas"].isArray()) r.fail("areas: not a list");
    for (const json::Value& a : root["areas"].array) {
        r.keys(a, "an area type", {"name", "cost"});
        navigation::AreaType area{a["name"].stringOr(""), 1.0f};
        if (area.name.empty() || area.name == "none" || area.name == "ignore")
            r.fail(std::format("area type '{}': a name other than none and ignore", area.name));
        r.number(area.cost, a["cost"], "area cost");
        b.areas.push_back(std::move(area));
    }
    if (b.areas.empty()) b.areas.push_back({"default", 1.0f});
    if (!r.ok()) return Error{r.error};

    const auto area = [&](const json::Value& v, std::string_view what) -> AreaId {
        Result<AreaId> id = areaOf(s, v, what);
        if (!id) r.fail(id.error());
        return id ? *id : navigation::kDefaultArea;
    };
    if (root.has("surfaces") && !root["surfaces"].isObject()) r.fail("surfaces: not an object");
    for (const auto& [name, value] : root["surfaces"].object)
        s.surfaces.emplace_back(name, value.stringOr("") == "ignore" ? NavMeshSettings::kIgnore
                                                                     : int(area(value, "surface '" + name + "'")));
    if (root.has("boxes") && !root["boxes"].isArray()) r.fail("boxes: not a list");
    for (const json::Value& v : root["boxes"].array) {
        r.keys(v, "a box", {"min", "max", "area"});
        s.boxes.push_back({r.vec3(v["min"], "box min"), r.vec3(v["max"], "box max"), area(v["area"], "box")});
    }
    if (root.has("links") && !root["links"].isArray()) r.fail("links: not a list");
    for (const json::Value& v : root["links"].array) {
        r.keys(v, "a link", {"start", "end", "radius", "bidirectional", "area", "id"});
        navigation::Link link;
        link.start = r.vec3(v["start"], "link start");
        link.end = r.vec3(v["end"], "link end");
        link.area = area(v["area"], "link");
        if (link.area == navigation::kNotWalkable) r.fail("link: area 'none'");
        link.bidirectional = v["bidirectional"].boolOr(false);
        r.number(link.radius, v["radius"], "link radius");
        const double id = v["id"].numberOr(0.0);
        if (!v["id"].isNull() && (!v["id"].isNumber() || id < 0.0 || id > 4294967295.0 || id != std::floor(id)))
            r.fail("link id: a whole number from 0 to 2^32 - 1");
        link.id = r.ok() ? std::uint32_t(id) : 0;
        s.links.push_back(link);
    }
    if (!r.ok()) return Error{r.error};
    return s;
}

Result<CookedNavMesh> cookNavMesh(const std::filesystem::path& source, const NavMeshOptions& options, JobSystem& jobs) {
    CookedNavMesh out;
    out.inputs.push_back(source);
    std::filesystem::path settingsPath = options.settings;
    if (settingsPath.empty()) {
        settingsPath = std::filesystem::path(source).replace_extension(".nav.json");
        if (!std::filesystem::exists(settingsPath)) settingsPath.clear();
    }
    NavMeshSettings settings;
    if (!settingsPath.empty()) {
        out.inputs.push_back(settingsPath);
        Result<std::vector<std::uint8_t>> text = readBytes(settingsPath);
        if (!text) return Error{text.error()};
        Result<NavMeshSettings> parsed =
            parseNavMeshSettings(std::string_view(reinterpret_cast<const char*>(text->data()), text->size()));
        if (!parsed) return Error{std::format("{}: {}", settingsPath.string(), parsed.error())};
        settings = std::move(*parsed);
    } else {
        settings.build.areas.push_back({"default", 1.0f});
    }

    navigation::Geometry geometry;
    SurfaceAreas surfaces{settings, {}};
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); });
    Result<void> read = extension == ".qmodel" ? modelGeometry(source, surfaces, geometry)
                                               : gltfGeometry(source, surfaces, geometry, out);
    if (!read) return Error{std::format("{}: {}", source.string(), read.error())};
    for (const auto& [name, area] : settings.surfaces)
        if (!surfaces.used.contains(name)) out.notes.push_back(std::format("surface '{}' names nothing", name));
    geometry.boxes = settings.boxes;
    geometry.links = settings.links;
    out.triangles = geometry.areas.size();

    Result<navigation::NavMeshData> data = navigation::buildNavMeshData(geometry, settings.build, jobs);
    if (!data) return Error{std::format("{}: {}", source.string(), data.error())};
    out.tiles = data->tilesX * data->tilesZ;
    out.layers = int(data->layers.size());
    out.file = navigation::writeNavMeshData(*data);
    return out;
}

} // namespace quench::cook
