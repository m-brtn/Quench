#include "Test.h"

#include <quench/assets/Model.h>

#include <array>
#include <cstring>

using namespace quench;
using namespace quench::assets;

namespace {

ModelData sampleModel() {
    ModelData model;
    ModelMaterial stone;
    stone.name = "stone";
    stone.baseColor = {0.5f, 0.4f, 0.3f, 1.0f};
    stone.roughness = 0.8f;
    stone.metallic = 0.0f;
    stone.baseColorTexture = "textures/stone.ktx2";
    ModelMaterial glass;
    glass.name = "glass";
    glass.baseColor = {0.9f, 0.9f, 1.0f, 0.3f};
    glass.emissive = {0.1f, 0.2f, 0.3f};
    glass.alphaMode = AlphaMode::Blend;
    glass.doubleSided = true;
    glass.normalTexture = "textures/glass_n.ktx2";
    model.materials = {stone, glass};

    // A 4 x 4 grid of quads with a second LOD of two triangles.
    ModelMesh grid;
    grid.name = "grid";
    for (int z = 0; z <= 4; ++z)
        for (int x = 0; x <= 4; ++x)
            grid.vertices.push_back({{float(x), 0.1f * float(x * z), float(z)},
                                     {0.0f, 1.0f, 0.0f},
                                     {float(x) / 4.0f, float(z) / 4.0f},
                                     {1.0f, 0.5f, 0.25f, 1.0f},
                                     {1.0f, 0.0f, 0.0f, z % 2 == 0 ? 1.0f : -1.0f}});
    for (std::uint32_t z = 0; z < 4; ++z)
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint32_t i = z * 5 + x;
            grid.indices.insert(grid.indices.end(), {i, i + 5, i + 1, i + 1, i + 5, i + 6});
        }
    grid.lods.push_back({0, std::uint32_t(grid.indices.size()), 0.0f});
    grid.indices.insert(grid.indices.end(), {0, 20, 4, 4, 20, 24});
    grid.lods.push_back({96, 6, 0.05f});
    grid.boundsMin = {0.0f, 0.0f, 0.0f};
    grid.boundsMax = {4.0f, 1.6f, 4.0f};

    ModelMesh tri;
    tri.name = "triangle";
    tri.material = 1;
    tri.vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}},
                    {{1, 0, 0}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}},
                    {{0, 1, 0}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}}};
    tri.indices = {0, 1, 2};
    tri.lods = {{0, 3, 0.0f}};
    tri.boundsMax = {1.0f, 1.0f, 0.0f};
    tri.skin = 0;
    tri.skinning.resize(3);
    tri.skinning[1] = {{1, 0, 0, 0}, {65535, 0, 0, 0}};
    tri.skinning[2] = {{0, 1, 0, 0}, {30000, 35535, 0, 0}};
    ModelSkin skin;
    skin.name = "rig";
    skin.joints = {"root", "arm"};
    glm::mat4 arm(1.0f);
    arm[3] = {-1.0f, 0.0f, 0.0f, 1.0f};
    skin.inverseBindMatrices = {glm::mat4(1.0f), arm};
    model.skins = {skin};
    model.meshes = {grid, tri};
    model.boundsMin = {0.0f, 0.0f, 0.0f};
    model.boundsMax = {4.0f, 1.6f, 4.0f};
    return model;
}

// meshoptimizer's index codec keeps the triangle order and winding but may rotate a triangle's indices.
bool sameTriangles(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    if (a.size() != b.size()) return false;
    const auto rotated = [](const std::uint32_t* t) {
        const int first = t[0] <= t[1] && t[0] <= t[2] ? 0 : (t[1] <= t[2] ? 1 : 2);
        return std::array<std::uint32_t, 3>{t[first], t[(first + 1) % 3], t[(first + 2) % 3]};
    };
    for (std::size_t i = 0; i + 2 < a.size(); i += 3)
        if (rotated(&a[i]) != rotated(&b[i])) return false;
    return true;
}

bool sameVertices(const std::vector<ModelVertex>& a, const std::vector<ModelVertex>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(ModelVertex)) == 0;
}

} // namespace

QUENCH_TEST(model_round_trip) {
    const ModelData model = sampleModel();
    const Result<std::vector<std::uint8_t>> file = writeModel(model);
    REQUIRE(file.ok(), "write: %s", file.error().c_str());
    CHECK(writeModel(model).value() == *file, "two writes differ");
    const Result<ModelData> read = readModel(*file);
    REQUIRE(read.ok(), "read: %s", read.error().c_str());
    REQUIRE(read->meshes.size() == 2 && read->materials.size() == 2, "%zu meshes, %zu materials", read->meshes.size(),
            read->materials.size());
    for (std::size_t i = 0; i < 2; ++i) {
        const ModelMesh& a = model.meshes[i];
        const ModelMesh& b = read->meshes[i];
        CHECK(a.name == b.name && a.material == b.material && sameTriangles(a.indices, b.indices),
              "mesh %zu header or triangles", i);
        CHECK(sameVertices(a.vertices, b.vertices), "mesh %zu vertices", i);
        CHECK(a.lods.size() == b.lods.size() && a.boundsMax == b.boundsMax, "mesh %zu LODs or bounds", i);
        for (std::size_t l = 0; l < a.lods.size() && l < b.lods.size(); ++l)
            CHECK(a.lods[l].firstIndex == b.lods[l].firstIndex && a.lods[l].indexCount == b.lods[l].indexCount &&
                      a.lods[l].error == b.lods[l].error,
                  "mesh %zu LOD %zu", i, l);
    }
    const ModelMaterial& glass = read->materials[1];
    CHECK(glass.name == "glass" && glass.alphaMode == AlphaMode::Blend && glass.doubleSided &&
              glass.baseColor.w == 0.3f && glass.emissive.z == 0.3f && glass.normalTexture == "textures/glass_n.ktx2" &&
              glass.baseColorTexture.empty(),
          "material 1");
    CHECK(read->materials[0].baseColorTexture == "textures/stone.ktx2" && read->materials[0].roughness == 0.8f,
          "material 0");
    CHECK(read->boundsMax == model.boundsMax, "model bounds");
    REQUIRE(read->skins.size() == 1, "%zu skins", read->skins.size());
    CHECK(read->skins[0].name == "rig" && read->skins[0].joints == model.skins[0].joints &&
              read->skins[0].inverseBindMatrices == model.skins[0].inverseBindMatrices,
          "skin");
    CHECK(read->meshes[0].skin == -1 && read->meshes[0].skinning.empty(), "mesh 0 is not skinned");
    CHECK(read->meshes[1].skin == 0 && read->meshes[1].skinning.size() == 3 &&
              std::memcmp(read->meshes[1].skinning.data(), model.meshes[1].skinning.data(),
                          3 * sizeof(ModelSkinVertex)) == 0,
          "mesh 1 skin vertices");
}

QUENCH_TEST(model_rejects_versions_without_tangents) {
    std::vector<std::uint8_t> file = writeModel(sampleModel()).value();
    for (const std::uint8_t version : {1, 2}) {
        file[4] = version;
        const Result<ModelData> read = readModel(file);
        CHECK(!read && read.error().find("cook its source again") != std::string::npos, "version %u: %s", version,
              read ? "accepted" : read.error().c_str());
    }
}

QUENCH_TEST(model_rejects_corrupt) {
    const std::vector<std::uint8_t> file = writeModel(sampleModel()).value();
    for (std::size_t size = 0; size < file.size(); ++size)
        CHECK(!readModel(std::span(file).first(size)), "prefix of %zu of %zu bytes accepted", size, file.size());
    std::vector<std::uint8_t> bad = file;
    bad[4] = 4;
    const Result<ModelData> version = readModel(bad);
    CHECK(!version && version.error().find("version 4") != std::string::npos, "version: %s",
          version ? "accepted" : version.error().c_str());
    // Every byte flip is rejected or gives a model whose indices are in range (asan checks the reads).
    for (std::size_t i = 0; i < file.size(); ++i) {
        bad = file;
        bad[i] ^= 0xa5;
        const Result<ModelData> model = readModel(bad);
        if (!model) continue;
        for (const ModelMesh& mesh : model->meshes) {
            for (std::uint32_t index : mesh.indices)
                CHECK(index < mesh.vertices.size(), "flip at %zu: index %u of %zu vertices", i, index,
                      mesh.vertices.size());
            if (mesh.skin < 0) continue;
            CHECK(std::size_t(mesh.skin) < model->skins.size() && mesh.skinning.size() == mesh.vertices.size(),
                  "flip at %zu: skin %d", i, mesh.skin);
            for (const ModelSkinVertex& v : mesh.skinning)
                for (const std::uint16_t j : v.joints)
                    CHECK(j < model->skins[std::size_t(mesh.skin)].joints.size(), "flip at %zu: joint %u", i, j);
        }
    }
}

QUENCH_TEST(model_write_checks) {
    ModelData model = sampleModel();
    model.meshes[1].material = 7;
    CHECK(!writeModel(model), "material out of range written");
    model = sampleModel();
    model.meshes[0].lods[1].indexCount = 600;
    CHECK(!writeModel(model), "LOD out of range written");
    model = sampleModel();
    model.meshes[1].indices.pop_back();
    CHECK(!writeModel(model), "partial triangle written");
    model = sampleModel();
    model.meshes[1].skinning.pop_back();
    CHECK(!writeModel(model), "a skin vertex missing");
    model = sampleModel();
    model.meshes[1].skinning[0].joints[0] = 2;
    CHECK(!writeModel(model), "a joint out of the skin's range");
    model = sampleModel();
    model.skins[0].inverseBindMatrices.pop_back();
    CHECK(!writeModel(model), "an inverse bind matrix missing");
}
