// Decals: marks laid on flat surfaces (render::Decals, docs/render.md). A lit floor and a wall with painted
// stripes, plates and rings, scorches and splats; a click on the floor stamps one more of the chosen shape. Marks
// with a life fade out and go; past the capacity the oldest goes.

#include "Demo.h"

#include <quench/render/Decals.h>

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace demo {
namespace {

using namespace quench;
using namespace quench::render;

constexpr std::uint32_t kCapacity = 256;
constexpr std::array<const char*, 6> kShapeNames{"Disc", "Ring", "Splat", "Scorch", "Square", "Stripes"};
constexpr std::array<glm::vec4, 6> kShapeColors{
    glm::vec4(0.10f, 0.75f, 0.70f, 0.9f), glm::vec4(1.00f, 0.80f, 0.15f, 0.95f), glm::vec4(0.55f, 0.06f, 0.42f, 0.95f),
    glm::vec4(0.02f, 0.02f, 0.02f, 0.9f), glm::vec4(0.85f, 0.87f, 0.90f, 0.95f), glm::vec4(1.00f, 0.62f, 0.05f, 0.95f)};

class DecalsSection final : public Section {
public:
    DecalsSection() {
        camera.target = glm::vec3(0.0f, 0.4f, -0.5f);
        camera.distance = 13.0f;
        camera.yaw = glm::radians(22.0f);
        camera.pitch = glm::radians(38.0f);
    }

    std::string_view id() const override { return "decals"; }
    std::string_view title() const override { return "Decals"; }
    std::string_view about() const override {
        return "Marks on flat surfaces: painted stripes, plates and rings, scorches and splats, lit and shadowed like "
               "the floor under them. Click the floor to stamp the chosen shape; marks with a life fade out, and past "
               "256 the oldest goes. They are flat: a mark does not wrap around an edge.";
    }

    Result<void> init(Context& c) override {
        m_scene.emplace(c.app.device());
        Scene& s = *m_scene;
        const Result<MeshId> floor = addMesh(s, planeMesh(glm::vec2(14.0f), 14.0f));
        const Result<MeshId> box = addMesh(s, boxMesh(glm::vec3(0.5f)));
        const Result<MeshId> ball = addMesh(s, sphereMesh(0.5f, 24, 48));
        for (const Result<MeshId>* m : {&floor, &box, &ball})
            if (!*m) return Error{m->error()};
        s.addInstance(
            {.mesh = *floor,
             .material = s.addMaterial({.baseColor = glm::vec4(0.26f, 0.28f, 0.31f, 1.0f), .roughness = 0.7f})});
        const MaterialId wall = s.addMaterial({.baseColor = glm::vec4(0.5f, 0.52f, 0.56f, 1.0f), .roughness = 0.8f});
        s.addInstance({.mesh = *box, .material = wall, .transform = placed({0.0f, 1.5f, -5.0f}, {12.0f, 3.0f, 0.4f})});
        // A crate and a ball throw their shadows across the marks.
        s.addInstance({.mesh = *box,
                       .material = s.addMaterial({.baseColor = glm::vec4(0.75f, 0.35f, 0.1f, 1.0f), .roughness = 0.6f}),
                       .transform = placed({-2.6f, 0.6f, -1.2f}, glm::vec3(1.2f), 0.5f)});
        s.addInstance({.mesh = *ball,
                       .material = s.addMaterial(
                           {.baseColor = glm::vec4(0.8f, 0.82f, 0.85f, 1.0f), .roughness = 0.25f, .metallic = 1.0f}),
                       .transform = placed({2.8f, 0.7f, 0.6f}, glm::vec3(1.4f))});
        m_sun = s.addLight({.type = LightType::Directional});
        setDaylight(s, m_sun, 42.0f, 215.0f, 0.55f);
        m_decals.emplace(s, kCapacity);
        layOut();
        return {};
    }

    void update(Context& c, double frameDt) override {
        (void)c;
        m_decals->update(float(frameDt));
        // A stream of marks walking around the floor, when asked for.
        if (m_stream) {
            m_streamTime += float(frameDt);
            while (m_streamTime >= 0.08f) {
                m_streamTime -= 0.08f;
                const float a = m_random.uniform(0.0f, 6.2831853f);
                const float r = m_random.uniform(1.0f, 5.5f);
                stamp({std::cos(a) * r, 0.0f, std::sin(a) * r * 0.7f + 0.5f});
            }
        }
    }

    void controls(Context& c) override {
        (void)c;
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::Combo("Shape", &m_shape, kShapeNames.data(), int(kShapeNames.size()));
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::SliderFloat("Size (m)", &m_size, 0.3f, 4.0f, "%.1f");
        ImGui::SetNextItemWidth(itemWidth());
        ImGui::SliderFloat("Life (s)", &m_life, 0.0f, 20.0f, m_life <= 0.0f ? "stays" : "%.0f");
        ImGui::Checkbox("A stream of marks", &m_stream);
        if (ImGui::Button("Clear")) m_decals->clear();
        ImGui::SameLine();
        if (ImGui::Button("Lay out again")) {
            m_decals->clear();
            layOut();
        }
    }

    void stats(Context& c) override {
        (void)c;
        ImGui::TextWrapped("%u decals of %u; %d stamped", m_decals->count(), m_decals->capacity(), m_stamped);
    }

    void click(Context& c, glm::vec2 point) override {
        if (const std::optional<glm::vec3> ground = groundPoint(c, camera, point)) stamp(*ground);
    }

    Scene& scene() override { return *m_scene; }

    std::optional<double> value(std::string_view name) const override {
        if (!m_decals) return std::nullopt;
        if (name == "decals") return double(m_decals->count());
        if (name == "stamped") return double(m_stamped);
        if (name == "decal_shape") return double(m_shape);
        return std::nullopt;
    }

    bool set(Context& c, std::string_view name, std::string_view value) override {
        (void)c;
        const std::optional<float> number = parseFloat(value);
        const std::optional<bool> flag = parseBool(value);
        if (name == "decal_shape" && number) {
            m_shape = std::clamp(int(*number), 0, int(kShapeNames.size()) - 1);
        } else if (name == "decal_size" && number) {
            m_size = std::clamp(*number, 0.1f, 10.0f);
        } else if (name == "decal_life" && number) {
            m_life = std::max(*number, 0.0f);
        } else if (name == "decal_stream" && flag) {
            m_stream = *flag;
        } else if (name == "decal_fill" && number) { // that many stamps in a grid
            for (int i = 0; i < int(*number); ++i)
                stamp({float(i % 20) * 0.5f - 4.75f, 0.0f, float(i / 20) * 0.35f - 2.0f});
        } else if (name == "decal_clear" && flag) {
            if (*flag) m_decals->clear();
        } else if (name == "decal_layout" && flag) {
            m_decals->clear();
            if (*flag) layOut();
        } else {
            return false;
        }
        return true;
    }

    // `@floor`: a free place on the floor in front of the camera.
    std::optional<glm::vec2> point(const Context& c, std::string_view name) const override {
        if (name == "floor") return camera.project(glm::vec3(0.5f, 0.0f, 2.2f), windowSize(c));
        return std::nullopt;
    }

    void shutdown(Context& c) override {
        (void)c;
        m_decals.reset(); // before its scene
        m_scene.reset();
    }

private:
    void stamp(const glm::vec3& at) {
        const float turn = m_random.uniform(0.0f, 6.2831853f);
        m_decals->add({.position = at,
                       .rotation = turn,
                       .size = glm::vec2(m_size),
                       .shape = DecalShape(m_shape),
                       .color = kShapeColors[std::size_t(m_shape)],
                       .roughness = DecalShape(m_shape) == DecalShape::Splat ? 0.25f : 0.9f,
                       .life = m_life,
                       .fade = 1.5f});
        ++m_stamped;
    }

    // What a level would paint and what a fight would leave.
    void layOut() {
        Decals& d = *m_decals;
        // Painted on the floor: a hazard border in front of the wall, landing rings, numbered plates.
        for (int i = 0; i < 6; ++i)
            d.add({.position = {float(i) * 2.0f - 5.0f, 0.0f, -4.0f},
                   .size = {2.0f, 0.7f},
                   .shape = DecalShape::Stripes,
                   .color = kShapeColors[5]});
        for (const float x : {-4.0f, 4.0f}) {
            d.add({.position = {x, 0.0f, 2.5f},
                   .size = glm::vec2(3.0f),
                   .shape = DecalShape::Ring,
                   .color = kShapeColors[1]});
            d.add({.position = {x, 0.0f, 2.5f},
                   .size = glm::vec2(1.0f),
                   .shape = DecalShape::Disc,
                   .color = kShapeColors[0]});
        }
        for (int i = 0; i < 3; ++i)
            d.add({.position = {float(i) * 1.3f - 1.3f, 0.0f, 4.2f},
                   .size = glm::vec2(1.0f),
                   .shape = DecalShape::Square,
                   .color = kShapeColors[4]});
        // Left by a fight: scorches and wet splats, turned every way.
        const glm::vec3 scorches[4] = {
            {-0.6f, 0.0f, -0.4f}, {1.1f, 0.0f, 1.0f}, {-3.9f, 0.0f, 0.4f}, {4.6f, 0.0f, -1.6f}};
        for (int i = 0; i < 4; ++i)
            d.add({.position = scorches[i],
                   .rotation = 1.3f * float(i),
                   .size = glm::vec2(2.2f + 0.4f * float(i % 2)),
                   .shape = DecalShape::Scorch,
                   .color = kShapeColors[3]});
        const glm::vec3 splats[3] = {{0.6f, 0.0f, -1.9f}, {-1.9f, 0.0f, 1.9f}, {2.6f, 0.0f, 3.0f}};
        for (int i = 0; i < 3; ++i)
            d.add({.position = splats[i],
                   .rotation = 2.1f * float(i),
                   .size = glm::vec2(1.6f),
                   .shape = DecalShape::Splat,
                   .color = kShapeColors[2],
                   .roughness = 0.25f});
        // On the wall: stripes, a plate, a scorch.
        const glm::vec3 out(0.0f, 0.0f, 1.0f);
        d.add({.position = {-3.5f, 1.5f, -4.8f},
               .normal = out,
               .size = {2.4f, 0.8f},
               .shape = DecalShape::Stripes,
               .color = kShapeColors[5]});
        d.add({.position = {0.0f, 1.6f, -4.8f},
               .normal = out,
               .size = glm::vec2(1.2f),
               .shape = DecalShape::Square,
               .color = kShapeColors[0]});
        d.add({.position = {3.4f, 1.3f, -4.8f},
               .normal = out,
               .rotation = 0.7f,
               .size = glm::vec2(2.0f),
               .shape = DecalShape::Scorch,
               .color = kShapeColors[3]});
    }

    // The scene before the decals laid in it.
    std::optional<Scene> m_scene;
    std::optional<Decals> m_decals;
    LightId m_sun;
    Random m_random;
    int m_shape = int(DecalShape::Scorch);
    float m_size = 1.6f;
    float m_life = 0.0f;
    bool m_stream = false;
    float m_streamTime = 0.0f;
    int m_stamped = 0;
};

} // namespace

std::unique_ptr<Section> makeDecalsSection() { return std::make_unique<DecalsSection>(); }

} // namespace demo
