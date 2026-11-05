// A starting point for a game on Quench: a player cube on a grid, moved with the keyboard, a gamepad stick or a finger
// (a virtual stick around the touch-down point), a camera that follows it, settings read from an asset, an ImGui
// window and scenario values for the checks in scenarios/.

#include "shaders/GameUniforms.h"

#include <quench/app/App.h>
#include <quench/app/Input.h>
#include <quench/core/FileSystem.h>
#include <quench/core/Json.h>
#include <quench/core/Log.h>
#include <quench/gpu/Device.h>

#include <game_shaders.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <vector>

namespace {

using namespace quench;
using app::App;

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
};

struct Mesh {
    gpu::Buffer vertices;
    gpu::Buffer indices;
    std::uint32_t indexCount = 0;
};

// A unit cube centred on the origin; counter-clockwise faces seen from outside.
void buildCube(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    for (int axis = 0; axis < 3; ++axis) {
        for (const float sign : {-1.0f, 1.0f}) {
            glm::vec3 normal(0.0f);
            normal[axis] = sign;
            glm::vec3 u(0.0f), v(0.0f);
            u[(axis + 1) % 3] = 1.0f;
            v[(axis + 2) % 3] = 1.0f;
            if (sign < 0.0f) std::swap(u, v);
            const auto base = std::uint32_t(vertices.size());
            constexpr float kCorners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
            for (const auto& corner : kCorners)
                vertices.push_back({0.5f * (normal + corner[0] * u + corner[1] * v), normal});
            indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
}

void buildGround(float halfSize, std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    vertices = {{{-halfSize, 0.0f, -halfSize}, up},
                {{-halfSize, 0.0f, halfSize}, up},
                {{halfSize, 0.0f, halfSize}, up},
                {{halfSize, 0.0f, -halfSize}, up}};
    indices = {0, 1, 2, 0, 2, 3};
}

Result<Mesh> uploadMesh(gpu::Device& device, const std::vector<Vertex>& vertices,
                        const std::vector<std::uint32_t>& indices, std::string_view name) {
    const auto vertexBytes = std::uint32_t(vertices.size() * sizeof(Vertex));
    const auto indexBytes = std::uint32_t(indices.size() * sizeof(std::uint32_t));
    Result<gpu::Buffer> vb =
        device.createBuffer({.usage = gpu::BufferUsage::Vertex, .size = vertexBytes, .name = name});
    if (!vb) return Error{vb.error()};
    Result<gpu::Buffer> ib = device.createBuffer({.usage = gpu::BufferUsage::Index, .size = indexBytes, .name = name});
    if (!ib) return Error{ib.error()};
    if (Result<void> r = device.upload(*vb, 0, std::as_bytes(std::span(vertices))); !r) return Error{r.error()};
    if (Result<void> r = device.upload(*ib, 0, std::as_bytes(std::span(indices))); !r) return Error{r.error()};
    Mesh mesh;
    mesh.vertices = std::move(*vb);
    mesh.indices = std::move(*ib);
    mesh.indexCount = std::uint32_t(indices.size());
    return mesh;
}

glm::vec3 linearToSrgb(glm::vec3 c) {
    for (int i = 0; i < 3; ++i)
        c[i] = c[i] <= 0.0031308f ? c[i] * 12.92f : 1.055f * std::pow(c[i], 1.0f / 2.4f) - 0.055f;
    return c;
}

class TemplateGame final : public app::Game {
public:
    Result<void> init(App& app) override {
        // Settings from assets/settings.json; missing values keep their defaults.
        if (Result<std::string> text = fs::readAssetText("settings.json")) {
            Result<json::Value> settings = json::parse(*text);
            if (!settings) return Error{std::format("settings.json: {}", settings.error())};
            m_speed = settings->has("speed") ? (*settings)["speed"].numberOr(m_speed) : m_speed;
        } else {
            log::warning("game", "{}; using the default settings", text.error());
        }

        gpu::Device& device = app.device();
        const gpu::VertexBufferLayout layout{.slot = 0, .stride = sizeof(Vertex)};
        const gpu::VertexAttribute attributes[] = {
            {0, 0, gpu::VertexFormat::Float3, offsetof(Vertex, position)},
            {1, 0, gpu::VertexFormat::Float3, offsetof(Vertex, normal)},
        };
        const gpu::ColorTargetDesc color{app.swapchainFormat()};
        Result<gpu::GraphicsPipeline> pipeline = device.createPipeline({
            .vertexShader = shaders::game.find("game_vs"),
            .fragmentShader = shaders::game.find("game_fs"),
            .vertexBuffers = {&layout, 1},
            .vertexAttributes = attributes,
            .rasterizer = {.cull = gpu::CullMode::Back},
            .depth = {.test = true, .write = true},
            .colorTargets = {&color, 1},
            .depthFormat = kDepthFormat,
            .name = "game",
        });
        if (!pipeline) return Error{pipeline.error()};
        m_pipeline = std::move(*pipeline);

        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        buildCube(vertices, indices);
        Result<Mesh> cube = uploadMesh(device, vertices, indices, "cube");
        if (!cube) return Error{cube.error()};
        m_cube = std::move(*cube);
        buildGround(kArenaHalfSize + 10.0f, vertices, indices);
        Result<Mesh> ground = uploadMesh(device, vertices, indices, "ground");
        if (!ground) return Error{ground.error()};
        m_ground = std::move(*ground);

        using app::Binding;
        using app::GamepadAxis;
        app::Input& input = app.input();
        m_left = input.addAction(
            "left", {*Binding::key("A"), *Binding::key("Left"), Binding::gamepadAxis(GamepadAxis::LeftX, false)});
        m_right = input.addAction(
            "right", {*Binding::key("D"), *Binding::key("Right"), Binding::gamepadAxis(GamepadAxis::LeftX, true)});
        m_forward = input.addAction(
            "forward", {*Binding::key("W"), *Binding::key("Up"), Binding::gamepadAxis(GamepadAxis::LeftY, false)});
        m_back = input.addAction(
            "back", {*Binding::key("S"), *Binding::key("Down"), Binding::gamepadAxis(GamepadAxis::LeftY, true)});
        return {};
    }

    void fixedStep(App& app, double dt) override {
        const app::Input& input = app.input();
        glm::vec2 move(input.axis(m_left, m_right), input.axis(m_back, m_forward));
        move += touchStick(input);
        if (const float length = glm::length(move); length > 1.0f) move /= length;
        m_previous = m_position;
        // Forward is -z (away from the camera).
        m_position += glm::vec2(move.x, -move.y) * float(m_speed * dt);
        m_position = glm::clamp(m_position, glm::vec2(-kArenaHalfSize), glm::vec2(kArenaHalfSize));
    }

    void ui(App& app) override {
        const app::FrameStats& stats = app.stats();
        const app::WindowRect safe = app.safeArea();
        ImGui::SetNextWindowPos(ImVec2(float(safe.x) + 12.0f, float(safe.y) + 12.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin(QUENCH_GAME_NAME, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        if (!app.deterministic()) ImGui::Text("%.1f fps, %.2f ms", stats.fps, stats.frameMs);
        ImGui::Text("Position %.2f, %.2f", double(m_position.x), double(m_position.y));
        float speed = float(m_speed);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (ImGui::SliderFloat("Speed (m/s)", &speed, 0.5f, 10.0f)) m_speed = speed;
        if (ImGui::Button("Reset")) m_position = m_previous = glm::vec2(0.0f);
        ImGui::End();
    }

    void render(App& app, app::Frame& frame) override {
        if (!ensureDepth(app.device(), frame.width, frame.height)) return;
        const glm::vec2 position = glm::mix(m_previous, m_position, float(app.alpha()));
        const glm::vec3 target(position.x, 0.5f, position.y);
        const glm::vec3 eye = target + glm::vec3(0.0f, 6.0f, 7.0f);
        const float aspect = float(frame.width) / float(frame.height);
        // Reversed depth: near maps to 1, far to 0.
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(55.0f), aspect, 200.0f, 0.1f);
        const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
        const bool encode = !gpu::formatInfo(frame.format).srgb;
        const shader::GameFrame frameUniforms{
            .viewProj = projection * view,
            .lightDir = glm::vec4(glm::normalize(glm::vec3(0.4f, 1.0f, 0.6f)), 0.0f),
            .lightColor = glm::vec4(1.3f, 1.25f, 1.15f, 0.0f),
            .ambient = glm::vec4(0.25f, 0.28f, 0.34f, 0.0f),
            .options = glm::vec4(encode ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f),
        };
        glm::vec3 clear(0.32f, 0.42f, 0.55f);
        if (encode) clear = linearToSrgb(clear);

        const shader::GameDraw cubeDraw{
            .model = glm::scale(glm::translate(glm::mat4(1.0f), target), glm::vec3(0.8f)),
            .color = glm::vec4(0.10f, 0.45f, 0.85f, 0.0f),
        };
        const shader::GameDraw groundDraw{
            .model = glm::mat4(1.0f),
            .color = glm::vec4(0.50f, 0.50f, 0.48f, 1.0f),
        };

        const gpu::ColorTarget color{.texture = frame.target, .clear = {clear.r, clear.g, clear.b, 1.0f}};
        const gpu::DepthTarget depth{.texture = m_depth};
        gpu::RenderPass pass = frame.commands.beginRenderPass(color, &depth);
        if (!pass) return;
        pass.bindPipeline(m_pipeline);
        pass.pushUniforms(ShaderStage::Vertex, 0, frameUniforms);
        pass.pushUniforms(ShaderStage::Fragment, 0, frameUniforms);
        for (const auto& [mesh, draw] : {std::pair{&m_ground, groundDraw}, std::pair{&m_cube, cubeDraw}}) {
            pass.pushUniforms(ShaderStage::Vertex, 1, draw);
            pass.pushUniforms(ShaderStage::Fragment, 1, draw);
            pass.bindVertexBuffer(0, {mesh->vertices});
            pass.bindIndexBuffer({mesh->indices}, gpu::IndexFormat::Uint32);
            pass.drawIndexed(mesh->indexCount);
        }
    }

    void shutdown(App& app) override {
        (void)app;
        m_depth.reset();
        m_cube = {};
        m_ground = {};
        m_pipeline.reset();
    }

    std::optional<double> scenarioValue(std::string_view name) const override {
        if (name == "x") return m_position.x;
        if (name == "z") return m_position.y;
        if (name == "speed") return m_speed;
        return std::nullopt;
    }

private:
    static constexpr gpu::TextureFormat kDepthFormat = gpu::TextureFormat::Depth32Float;
    static constexpr float kArenaHalfSize = 20.0f;

    // Virtual stick: the first finger's offset from where it touched down, full deflection at 60 points; y up.
    glm::vec2 touchStick(const app::Input& input) {
        const std::span<const app::Finger> fingers = input.fingers();
        if (fingers.empty() || fingers[0].released) return glm::vec2(0.0f);
        const app::Finger& finger = fingers[0];
        if (finger.id != m_touchId) {
            m_touchId = finger.id;
            m_touchStart = finger.position - finger.delta;
        }
        const glm::vec2 offset = (finger.position - m_touchStart) / 60.0f;
        const glm::vec2 stick(offset.x, -offset.y);
        const float length = glm::length(stick);
        return length > 1.0f ? stick / length : stick;
    }

    bool ensureDepth(gpu::Device& device, std::uint32_t width, std::uint32_t height) {
        if (m_depth && m_depth.desc().width == width && m_depth.desc().height == height) return true;
        Result<gpu::Texture> depth = device.createTexture({.format = kDepthFormat,
                                                           .usage = gpu::TextureUsage::DepthStencilTarget,
                                                           .width = width,
                                                           .height = height,
                                                           .name = "depth"});
        if (!depth) {
            log::error("game", "depth target {}x{}: {}", width, height, depth.error());
            return false;
        }
        m_depth = std::move(*depth);
        return true;
    }

    gpu::GraphicsPipeline m_pipeline;
    Mesh m_cube;
    Mesh m_ground;
    gpu::Texture m_depth;
    app::ActionId m_left = 0, m_right = 0, m_forward = 0, m_back = 0;
    glm::vec2 m_position{0.0f}; // x, z on the ground (m)
    glm::vec2 m_previous{0.0f};
    glm::vec2 m_touchStart{0.0f};
    std::uint64_t m_touchId = 0;
    double m_speed = 4.0; // m/s
};

} // namespace

Result<std::unique_ptr<quench::app::Game>> quench::app::createGame(AppDesc& desc,
                                                                   std::span<const std::string_view> args) {
    desc.name = QUENCH_GAME_NAME;
    desc.organization = "Example";
    desc.version = QUENCH_GAME_VERSION;
    for (const std::string_view arg : args) {
        if (arg == "--help") {
            std::printf("%s: no options of its own.\n", QUENCH_GAME_NAME);
            continue;
        }
        return Error{std::format("unknown argument '{}' (--help lists the options)", arg)};
    }
    return std::unique_ptr<Game>(std::make_unique<TemplateGame>());
}
