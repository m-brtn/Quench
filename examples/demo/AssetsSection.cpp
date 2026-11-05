// Assets: models and textures cooked at build time (quench_cook: glTF, GLB, PNG, JPEG -> .qmodel, KTX2 with BC7 mips)
// and loaded through the app's assets::AssetManager into the scene (render::SceneAssets). A row of models shows the
// load states: gray placeholder boxes while loading (a simulated latency makes them visible), magenta boxes for a
// broken and a missing file. Behind it a grid of tiles streams around a moving focus point (assets::StreamingGrid),
// each tile with one of eight textures and a model: cells that come near load, cells left behind release their assets
// to the cache, which evicts the least recently used ones over the texture budget. Reload swaps new data in under the
// same handles; with hot reload on, files changed on disk (re-cooked by the build) are loaded again.

#include "Demo.h"

#include <quench/assets/AssetManager.h>
#include <quench/assets/StreamingGrid.h>
#include <quench/render/SceneAssets.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;
using assets::AssetManager;
using assets::AssetRef;
using assets::AssetState;
using assets::StreamingGrid;

struct ShowcaseEntry {
    const char* path;
    const char* label;
};
constexpr std::array<ShowcaseEntry, 5> kShowcase{{{"models/crate.qmodel", "crate"},
                                                  {"models/pillar.qmodel", "pillar"},
                                                  {"models/ball.qmodel", "ball"},
                                                  {"models/broken.qmodel", "broken"},
                                                  {"models/missing.qmodel", "missing"}}};
constexpr std::array<const char*, 3> kCellModels{"models/crate.qmodel", "models/pillar.qmodel", "models/ball.qmodel"};
constexpr int kTileTextures = 8;
constexpr float kCellSize = 3.0f;
constexpr int kCellsX = 6;
constexpr int kCellsZ = 5;
const glm::vec2 kGridOrigin(-9.0f, -15.0f); // the grid covers x -9..9, z -15..0
const glm::vec2 kFocusCenter(0.0f, -7.5f);
const glm::vec2 kFocusRadius(6.0f, 3.5f);
constexpr float kShowcaseZ = 2.5f;
constexpr float kFocusPeriod = 16.0f; // s per lap

struct BudgetChoice {
    const char* name;
    std::uint64_t bytes;
};
constexpr std::uint64_t kNoLimit = ~std::uint64_t(0);
constexpr std::array<BudgetChoice, 4> kTextureBudgets{
    {{"unlimited", kNoLimit}, {"64 KB", 64u << 10}, {"16 KB", 16u << 10}, {"4 KB", 4u << 10}}};
constexpr std::array<BudgetChoice, 3> kUploadBudgets{{{"8 MB", 8u << 20}, {"16 KB", 16u << 10}, {"1 B", 1}}};

std::string bytesText(std::uint64_t bytes) {
    if (bytes == kNoLimit) return "unlimited";
    if (bytes < 1024) return std::format("{} B", bytes);
    if (bytes < (1u << 20)) return std::format("{:.0f} KB", double(bytes) / 1024.0);
    return std::format("{:.1f} MB", double(bytes) / double(1u << 20));
}

class AssetsSection final : public Section {
public:
    AssetsSection() {
        camera.target = glm::vec3(0.0f, 0.0f, -3.0f);
        camera.distance = 24.0f;
        camera.yaw = 0.0f;
        camera.pitch = glm::radians(55.0f);
        camera.maxDistance = 80.0f;
    }

    std::string_view id() const override { return "assets"; }
    std::string_view title() const override { return "Assets"; }
    std::string_view about() const override {
        return "Cooked glTF models and KTX2 textures loading on jobs: gray boxes while loading, magenta for a broken "
               "and a missing file. Behind them tiles stream around the orange focus; left cells go to the cache, "
               "which evicts over its texture budget.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> ground = addMesh(s, planeMesh(glm::vec2(22.0f), 1.0f));
        const Result<MeshId> tile = addMesh(s, planeMesh(glm::vec2(kCellSize * 0.5f - 0.1f), 1.0f));
        const Result<MeshId> slab = addMesh(s, boxMesh(glm::vec3(0.9f, 0.04f, 0.9f)));
        const Result<MeshId> marker = addMesh(s, sphereMesh(0.3f, 12, 24));
        for (const Result<MeshId>* m : {&ground, &tile, &slab, &marker})
            if (!*m) return Error{m->error()};
        m_tileMesh = *tile;
        s.addInstance({.mesh = *ground,
                       .material = addCheckerMaterial(c, s, glm::vec3(0.42f, 0.43f, 0.45f),
                                                      glm::vec3(0.34f, 0.35f, 0.37f), 1.0f)});
        const MaterialId slabMaterial =
            s.addMaterial({.baseColor = glm::vec4(0.2f, 0.22f, 0.25f, 1.0f), .roughness = 0.7f, .name = "slab"});
        for (std::size_t i = 0; i < kShowcase.size(); ++i)
            s.addInstance({.mesh = *slab, .material = slabMaterial, .transform = placed(showcasePosition(i))});
        m_marker = s.addInstance({.mesh = *marker,
                                  .material = s.addMaterial({.baseColor = glm::vec4(1.0f, 0.45f, 0.1f, 1.0f),
                                                             .emissive = glm::vec3(2.0f, 0.7f, 0.1f),
                                                             .name = "focus"}),
                                  .castsShadows = false});
        const glm::vec3 towardSun = glm::normalize(glm::vec3(0.45f, 0.8f, 0.35f));
        s.addLight({.type = LightType::Directional, .color = glm::vec3(5.0f, 4.8f, 4.4f), .direction = -towardSun});

        m_manager = &c.app.assets();
        m_assets.emplace(s, *m_manager);
        m_grid.emplace(StreamingGrid::Settings{.origin = kGridOrigin,
                                               .cellSize = kCellSize,
                                               .cellsX = kCellsX,
                                               .cellsZ = kCellsZ,
                                               .loadRadius = 4.0f,
                                               .keepRadius = 5.5f});
        return {};
    }

    void enter(Context& c) override {
        AssetManager& manager = c.app.assets();
        m_savedLatency = manager.settings().simulatedLatency;
        m_savedHotReload = manager.settings().hotReload;
        m_savedUploadBudget = manager.settings().uploadBytesPerFrame;
        manager.setSimulatedLatency(m_latency);
        manager.setHotReload(m_hotReload);
        manager.setUploadBudget(kUploadBudgets[std::size_t(m_uploadBudget)].bytes);
        applyTextureBudget(manager);
        if (m_showShowcase) placeShowcase();
        updateStreaming();
        m_assets->update();
    }

    void leave(Context& c) override {
        clearCells();
        removeShowcase();
        m_assets->update();
        AssetManager& manager = c.app.assets();
        manager.setSimulatedLatency(m_savedLatency);
        manager.setHotReload(m_savedHotReload);
        manager.setUploadBudget(m_savedUploadBudget);
    }

    void update(Context& c, double frameDt) override {
        (void)c;
        if (m_moveFocus) m_focusAngle = std::fmod(m_focusAngle + float(frameDt) * 360.0f / kFocusPeriod, 360.0f);
        updateStreaming();
        m_assets->update();
        const glm::vec2 f = focus();
        m_scene->setTransform(m_marker, placed(glm::vec3(f.x, 0.35f, f.y)));
    }

    void ui(Context& c) override {
        AssetManager& manager = c.app.assets();
        const AssetManager::Stats stats = manager.stats();
        const std::uint32_t total = stats.loading + stats.ready + stats.failed;
        const std::string progress =
            std::format("{} ready, {} loading, {} failed", stats.ready, stats.loading, stats.failed);
        ImGui::ProgressBar(total > 0 ? float(stats.ready + stats.failed) / float(total) : 1.0f, ImVec2(-1.0f, 0.0f),
                           progress.c_str());
        ImGui::Text("Queued %u, reading %u, uploads %u", stats.queued, stats.decoding, stats.waitingForUpload);
        ImGui::Text("Reloads %llu, evictions %llu", static_cast<unsigned long long>(stats.reloads),
                    static_cast<unsigned long long>(stats.evictions));
        for (const AssetManager::TypeStats& type : stats.types) {
            std::string line = std::format("{}s {}, {} unused, {}", type.name, type.count, type.unreferenced,
                                           bytesText(type.memory.total()));
            if (type.budget.gpu != kNoLimit) line += std::format("/{}", bytesText(type.budget.gpu));
            ImGui::TextUnformatted(line.c_str());
        }
        const StreamingGrid::Stats& grid = m_grid->stats();
        ImGui::Text("Cells %u, %llu loads, %llu unloads", grid.active, static_cast<unsigned long long>(grid.loads),
                    static_cast<unsigned long long>(grid.unloads));

        float latency = m_latency;
        if (ImGui::SliderFloat("Latency", &latency, 0.0f, 2.0f, "%.2f s")) setLatency(manager, latency);
        if (ImGui::BeginCombo("Texture cache", kTextureBudgets[std::size_t(m_textureBudget)].name)) {
            for (std::size_t i = 0; i < kTextureBudgets.size(); ++i)
                if (ImGui::Selectable(kTextureBudgets[i].name, int(i) == m_textureBudget)) {
                    m_textureBudget = int(i);
                    applyTextureBudget(manager);
                }
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo("Uploads/frame", kUploadBudgets[std::size_t(m_uploadBudget)].name)) {
            for (std::size_t i = 0; i < kUploadBudgets.size(); ++i)
                if (ImGui::Selectable(kUploadBudgets[i].name, int(i) == m_uploadBudget)) {
                    m_uploadBudget = int(i);
                    manager.setUploadBudget(kUploadBudgets[i].bytes);
                }
            ImGui::EndCombo();
        }
        ImGui::SliderFloat("Focus", &m_focusAngle, 0.0f, 360.0f, "%.0f deg");
        ImGui::Checkbox("Move", &m_moveFocus);
        ImGui::SameLine();
        bool showcase = m_showShowcase;
        if (ImGui::Checkbox("Models", &showcase)) setShowcase(showcase);
        ImGui::SameLine();
        if (ImGui::Checkbox("Hot reload", &m_hotReload)) manager.setHotReload(m_hotReload);
        if (ImGui::Button("Reload all")) reloadAll(manager);
        ImGui::SameLine();
        ImGui::TextDisabled("BC7 %s", m_assets->bcTextures() ? "sampled" : "decoded on load");
        if (m_hotReload)
            ImGui::TextWrapped("Edit examples/demo/assets_src, rebuild quench_demo_assets: changed files reload.");

        // Labels under the models of the row.
        const glm::vec2 window(float(c.app.windowWidth()), float(c.app.windowHeight()));
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        for (std::size_t i = 0; i < kShowcase.size() && m_showShowcase; ++i) {
            const std::optional<glm::vec2> p =
                camera.project(showcasePosition(i) + glm::vec3(0.0f, 0.0f, 1.0f), window);
            if (!p) continue;
            const AssetRef<ModelAsset>& model = m_assets->model(m_showcase[i]);
            const std::string text = std::format("{}\n{}", kShowcase[i].label, assets::stateName(model.state()));
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            draw->AddText(ImVec2(p->x - size.x * 0.5f, p->y), IM_COL32(240, 240, 240, 255), text.c_str());
        }
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (!m_manager) return std::nullopt;
        const AssetManager& manager = *m_manager;
        const AssetManager::Stats stats = manager.stats();
        if (name == "assets_loading") return double(stats.loading);
        if (name == "assets_ready") return double(stats.ready);
        if (name == "assets_failed") return double(stats.failed);
        if (name == "assets_idle") return manager.idle() ? 1.0 : 0.0;
        if (name == "assets_loads") return double(stats.loadsFinished);
        if (name == "assets_reloads") return double(stats.reloads);
        if (name == "assets_evictions") return double(stats.evictions);
        if (name == "showcase_ready" || name == "showcase_failed") {
            const AssetState wanted = name == "showcase_ready" ? AssetState::Ready : AssetState::Failed;
            int n = 0;
            for (std::size_t i = 0; i < kShowcase.size() && m_showShowcase; ++i)
                if (m_assets->model(m_showcase[i]).state() == wanted) ++n;
            return double(n);
        }
        if (name == "streaming_active") return double(m_grid->stats().active);
        if (name == "streaming_loads") return double(m_grid->stats().loads);
        if (name == "streaming_unloads") return double(m_grid->stats().unloads);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        AssetManager& manager = c.app.assets();
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "latency" && number) {
            setLatency(manager, std::clamp(*number, 0.0f, 10.0f));
        } else if (name == "texture_budget" || name == "upload_budget") {
            // Bytes of one of the menu's choices, or "unlimited".
            const bool textures = name == "texture_budget";
            const std::span<const BudgetChoice> choices = textures ? std::span<const BudgetChoice>(kTextureBudgets)
                                                                   : std::span<const BudgetChoice>(kUploadBudgets);
            const std::uint64_t bytes = value == "unlimited" ? kNoLimit : number ? std::uint64_t(*number) : 0;
            const auto found = std::find_if(choices.begin(), choices.end(),
                                            [bytes](const BudgetChoice& choice) { return choice.bytes == bytes; });
            if (found == choices.end()) return false;
            (textures ? m_textureBudget : m_uploadBudget) = int(found - choices.begin());
            if (textures)
                applyTextureBudget(manager);
            else
                manager.setUploadBudget(bytes);
        } else if (name == "focus_angle" && number) {
            m_moveFocus = false;
            m_focusAngle = std::fmod(std::fmod(*number, 360.0f) + 360.0f, 360.0f);
        } else if (name == "move_focus" && flag) {
            m_moveFocus = *flag;
        } else if (name == "reload" && flag) {
            if (*flag) reloadAll(manager);
        } else if (name == "models" && flag) {
            setShowcase(*flag);
        } else if (name == "hot_reload" && flag) {
            m_hotReload = *flag;
            manager.setHotReload(m_hotReload);
        } else {
            return false;
        }
        return true;
    }

    void shutdown(Context& c) override {
        (void)c;
        clearCells();
        removeShowcase();
        m_assets.reset();
        m_scene.reset();
    }

private:
    struct CellContent {
        int texture = 0;
        InstanceId tile;
        ModelInstanceId model;
    };
    struct TileTexture {
        AssetRef<TextureAsset> texture;
        MaterialId material;
        int users = 0;
    };

    static glm::vec3 showcasePosition(std::size_t i) {
        return glm::vec3((float(i) - float(kShowcase.size() - 1) * 0.5f) * 3.0f, 0.0f, kShowcaseZ);
    }

    glm::vec2 focus() const {
        const float a = glm::radians(m_focusAngle);
        return kFocusCenter + glm::vec2(std::cos(a), std::sin(a)) * kFocusRadius;
    }

    void setLatency(AssetManager& manager, float seconds) {
        m_latency = seconds;
        manager.setSimulatedLatency(seconds);
    }

    void applyTextureBudget(AssetManager& manager) {
        const std::uint64_t bytes = kTextureBudgets[std::size_t(m_textureBudget)].bytes;
        manager.setBudget("texture", {.cpu = kNoLimit, .gpu = bytes});
    }

    void placeShowcase() {
        for (std::size_t i = 0; i < kShowcase.size(); ++i)
            m_showcase[i] = m_assets->addModel(m_assets->loadModel(kShowcase[i].path, float(i)),
                                               placed(showcasePosition(i) + glm::vec3(0.0f, 0.04f, 0.0f)));
    }

    void removeShowcase() {
        for (ModelInstanceId& id : m_showcase)
            if (id) {
                m_assets->removeModel(id);
                id = {};
            }
    }

    void setShowcase(bool show) {
        if (show == m_showShowcase) return;
        m_showShowcase = show;
        if (show)
            placeShowcase();
        else
            removeShowcase();
    }

    // Reloads every asset in use (the row, the cells, their textures); unchanged files give the same pictures.
    void reloadAll(AssetManager& manager) {
        manager.forEach([&](const assets::Asset& asset) {
            if (asset.refs() > 0) manager.reload(asset.id());
        });
    }

    void updateStreaming() {
        const StreamingGrid::Changes changes = m_grid->update(focus());
        for (const StreamingGrid::Cell& cell : changes.unload) unloadCell(cell.x, cell.z);
        for (const StreamingGrid::Cell& cell : changes.load) loadCell(cell);
    }

    void loadCell(const StreamingGrid::Cell& cell) {
        const int t = (cell.x * 3 + cell.z * 5) % kTileTextures;
        TileTexture& tile = m_tiles[std::size_t(t)];
        if (tile.users++ == 0) {
            // The texture handle is valid at once (white until the pixels arrive): the material is made now.
            tile.texture = m_assets->loadTexture(std::format("tiles/tile_{}.ktx2", t), cell.distance);
            MaterialDesc desc{.roughness = 0.85f, .name = "tile"};
            desc.textures[0] = tile.texture->texture();
            tile.material = m_scene->addMaterial(desc);
        }
        const glm::vec2 center = m_grid->cellCenter(cell.x, cell.z);
        CellContent content;
        content.texture = t;
        content.tile = m_scene->addInstance(
            {.mesh = m_tileMesh, .material = tile.material, .transform = placed(glm::vec3(center.x, 0.01f, center.y))});
        const float yaw = glm::radians(45.0f * float((cell.x * 7 + cell.z * 13) % 8));
        content.model =
            m_assets->addModel(m_assets->loadModel(kCellModels[std::size_t((cell.x + 2 * cell.z) % 3)], cell.distance),
                               placed(glm::vec3(center.x, 0.01f, center.y), glm::vec3(0.75f), yaw));
        m_cells[{cell.x, cell.z}] = content;
    }

    void unloadCell(int x, int z) {
        const auto it = m_cells.find({x, z});
        if (it == m_cells.end()) return;
        const CellContent content = it->second;
        m_cells.erase(it);
        m_scene->removeInstance(content.tile);
        m_assets->removeModel(content.model);
        TileTexture& tile = m_tiles[std::size_t(content.texture)];
        if (--tile.users == 0) {
            m_scene->removeMaterial(tile.material);
            tile.material = {};
            tile.texture = {}; // to the cache
        }
    }

    void clearCells() {
        if (!m_grid) return;
        for (const StreamingGrid::Cell& cell : m_grid->clear().unload) unloadCell(cell.x, cell.z);
    }

    AssetManager* m_manager = nullptr;
    std::optional<Scene> m_scene;
    std::optional<SceneAssets> m_assets;
    std::optional<StreamingGrid> m_grid;
    MeshId m_tileMesh;
    InstanceId m_marker;
    std::array<ModelInstanceId, kShowcase.size()> m_showcase{};
    std::array<TileTexture, kTileTextures> m_tiles{};
    std::map<std::pair<int, int>, CellContent> m_cells;
    float m_latency = 0.4f;
    int m_textureBudget = 1;
    int m_uploadBudget = 0;
    bool m_hotReload = false;
    bool m_showShowcase = true;
    bool m_moveFocus = true;
    float m_focusAngle = 0.0f;
    double m_savedLatency = 0.0;
    bool m_savedHotReload = false;
    std::uint64_t m_savedUploadBudget = 0;
};

} // namespace

std::unique_ptr<Section> makeAssetsSection() { return std::make_unique<AssetsSection>(); }

} // namespace demo
