// The smallest Quench game: a lit, procedurally built torus over a checkered ground, an ImGui window, input actions
// for keyboard, mouse, gamepad and touch, and scenario values for the checks (examples/hello/scenarios), with HTTP
// requests for the web check (NetCheck.h) and a microphone check for a person (MicCheck.h, --mic-check SECONDS).

#include "MicCheck.h"
#include "NetCheck.h"
#include "shaders/HelloUniforms.h"

#include <quench/app/App.h>
#include <quench/app/Input.h>
#include <quench/core/Log.h>
#include <quench/core/Parse.h>
#include <quench/gpu/Device.h>

#include <hello_shaders.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <numbers>
#include <optional>
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

// Counter-clockwise triangles seen from outside.
void buildTorus(float ringRadius, float tubeRadius, int rings, int sides, std::vector<Vertex>& vertices,
                std::vector<std::uint32_t>& indices) {
    constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
    for (int i = 0; i <= rings; ++i) {
        const float theta = kTau * float(i) / float(rings);
        for (int j = 0; j <= sides; ++j) {
            const float phi = kTau * float(j) / float(sides);
            const glm::vec3 normal(std::cos(phi) * std::cos(theta), std::sin(phi), std::cos(phi) * std::sin(theta));
            const glm::vec3 center(ringRadius * std::cos(theta), 0.0f, ringRadius * std::sin(theta));
            vertices.push_back({center + tubeRadius * normal, normal});
        }
    }
    const auto at = [sides](int i, int j) { return std::uint32_t(i * (sides + 1) + j); };
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < sides; ++j)
            indices.insert(indices.end(),
                           {at(i, j), at(i, j + 1), at(i + 1, j), at(i + 1, j), at(i, j + 1), at(i + 1, j + 1)});
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
    Mesh mesh;
    const auto vertexBytes = std::uint32_t(vertices.size() * sizeof(Vertex));
    const auto indexBytes = std::uint32_t(indices.size() * sizeof(std::uint32_t));
    Result<gpu::Buffer> vb =
        device.createBuffer({.usage = gpu::BufferUsage::Vertex, .size = vertexBytes, .name = name});
    if (!vb) return Error{vb.error()};
    Result<gpu::Buffer> ib = device.createBuffer({.usage = gpu::BufferUsage::Index, .size = indexBytes, .name = name});
    if (!ib) return Error{ib.error()};
    if (Result<void> r = device.upload(*vb, 0, std::as_bytes(std::span(vertices))); !r) return Error{r.error()};
    if (Result<void> r = device.upload(*ib, 0, std::as_bytes(std::span(indices))); !r) return Error{r.error()};
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

class Hello final : public app::Game {
public:
    explicit Hello(std::optional<double> micCheck) {
        if (micCheck) m_mic.emplace(*micCheck);
    }

    Result<void> init(App& app) override {
        gpu::Device& device = app.device();
        if (!device.supportsFormat(kDepthFormat, gpu::TextureType::Texture2D, gpu::TextureUsage::DepthStencilTarget))
            return Error{"Depth32Float depth targets are not supported"};

        const gpu::VertexBufferLayout layout{.slot = 0, .stride = sizeof(Vertex)};
        const gpu::VertexAttribute attributes[] = {
            {0, 0, gpu::VertexFormat::Float3, offsetof(Vertex, position)},
            {1, 0, gpu::VertexFormat::Float3, offsetof(Vertex, normal)},
        };
        const gpu::ColorTargetDesc color{app.swapchainFormat()};
        Result<gpu::GraphicsPipeline> pipeline = device.createPipeline({
            .vertexShader = shaders::hello.find("hello_vs"),
            .fragmentShader = shaders::hello.find("hello_fs"),
            .vertexBuffers = {&layout, 1},
            .vertexAttributes = attributes,
            .rasterizer = {.cull = gpu::CullMode::Back},
            .depth = {.test = true, .write = true},
            .colorTargets = {&color, 1},
            .depthFormat = kDepthFormat,
            .name = "hello",
        });
        if (!pipeline) return Error{pipeline.error()};
        m_pipeline = std::move(*pipeline);

        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        buildTorus(0.75f, 0.3f, 96, 48, vertices, indices);
        Result<Mesh> torus = uploadMesh(device, vertices, indices, "torus");
        if (!torus) return Error{torus.error()};
        m_torus = std::move(*torus);
        buildGround(30.0f, vertices, indices);
        Result<Mesh> ground = uploadMesh(device, vertices, indices, "ground");
        if (!ground) return Error{ground.error()};
        m_ground = std::move(*ground);

        using app::Binding;
        using app::GamepadAxis;
        using app::GamepadButton;
        app::Input& input = app.input();
        m_spinLeft = input.addAction(
            "spin_left", {*Binding::key("A"), *Binding::key("Left"), Binding::gamepadAxis(GamepadAxis::LeftX, false)});
        m_spinRight = input.addAction(
            "spin_right", {*Binding::key("D"), *Binding::key("Right"), Binding::gamepadAxis(GamepadAxis::LeftX, true)});
        m_pause = input.addAction("pause", {*Binding::key("Space"), Binding::gamepad(GamepadButton::South)});
        m_drag = input.addAction("drag", {Binding::mouse(app::MouseButton::Left)});
        log::info("hello", "torus: {} triangles; GPU {}", m_torus.indexCount / 3, device.caps().deviceName);
        return {};
    }

    void fixedStep(App& app, double dt) override {
        const app::Input& input = app.input();
        m_previousYaw = m_yaw;
        const double spin = m_paused ? 0.0 : m_speed;
        m_yaw += (spin + 2.0 * input.axis(m_spinLeft, m_spinRight)) * dt;
    }

    void update(App& app, double frameDt) override {
        m_net.update();
        if (m_mic) m_mic->update(app, frameDt);
        const app::Input& input = app.input();
        if (input.pressed(m_pause)) m_paused = !m_paused;
        // Dragging turns the torus directly (finger or mouse; mouse events SDL makes from touches are ignored).
        float dragX = 0.0f;
        for (const app::Finger& finger : input.fingers()) dragX += finger.delta.x;
        if (input.down(m_drag) && !input.mouseFromTouch()) dragX += input.mouseDelta().x;
        const double turn = double(dragX) * 0.01;
        m_yaw += turn;
        m_previousYaw += turn;
    }

    void ui(App& app) override {
        const app::FrameStats& stats = app.stats();
        const app::WindowRect safe = app.safeArea();
        ImGui::SetNextWindowPos(ImVec2(float(safe.x) + 12.0f, float(safe.y) + 12.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Quench hello", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("%s, %s", app.device().caps().driver.c_str(), app.device().caps().deviceName.c_str());
        ImGui::Text("%u x %u px, %.2f px/pt", app.pixelWidth(), app.pixelHeight(), double(app.pixelDensity()));
        if (app.deterministic()) {
            ImGui::TextUnformatted("Timings hidden (deterministic run)"); // captures compare against golden images
        } else {
            ImGui::Text("%.1f fps, %.2f ms; CPU %.2f ms", stats.fps, stats.frameMs, stats.cpuMs);
            if (stats.gpuMs >= 0.0)
                ImGui::Text("GPU %.2f ms", stats.gpuMs);
            else
                ImGui::TextUnformatted("GPU time unavailable");
        }
        ImGui::Text("t = %.2f s, %llu ticks", app.time(), static_cast<unsigned long long>(app.ticks()));
        ImGui::Checkbox("Paused (Space, gamepad A)", &m_paused);
        float speed = float(m_speed);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        if (ImGui::SliderFloat("Spin (rad/s)", &speed, -3.0f, 3.0f)) m_speed = speed;
        if (app.input().gamepadConnected()) ImGui::Text("Gamepad: %s", app.input().gamepadName().c_str());
        if (m_mic) m_mic->ui(app);
        ImGui::End();
    }

    void render(App& app, app::Frame& frame) override {
        if (!ensureDepth(app.device(), frame.width, frame.height)) return;
        const float aspect = float(frame.width) / float(frame.height);
        const glm::vec3 eye(0.0f, 1.7f, 4.2f);
        const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.7f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        // 50 degrees vertically; portrait screens keep at least 64 degrees horizontally. Reversed depth: near maps to
        // 1, far to 0.
        const float fovY = std::max(glm::radians(50.0f), 2.0f * std::atan(std::tan(glm::radians(32.0f)) / aspect));
        const glm::mat4 projection = glm::perspectiveRH_ZO(fovY, aspect, 200.0f, 0.1f);
        const bool encode = !gpu::formatInfo(frame.format).srgb;
        const glm::vec3 fog(0.62f, 0.72f, 0.86f);
        const shader::HelloFrame frameUniforms{
            .viewProj = projection * view,
            .cameraPos = glm::vec4(eye, 1.0f),
            .lightDir = glm::vec4(glm::normalize(glm::vec3(0.5f, 0.9f, 0.35f)), 0.0f),
            .lightColor = glm::vec4(1.45f, 1.38f, 1.25f, 0.0f),
            .fogColor = glm::vec4(fog, 0.02f),
            .options = glm::vec4(encode ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f),
        };
        glm::vec3 clear = glm::vec3(1.0f) - glm::exp(-fog);
        if (encode) clear = linearToSrgb(clear);

        const double yaw = m_previousYaw + (m_yaw - m_previousYaw) * app.alpha();
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.1f, 0.0f));
        model = glm::rotate(model, float(yaw), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, 0.65f, glm::vec3(1.0f, 0.0f, 0.0f));
        const shader::HelloDraw torusDraw{
            .model = model,
            .color = glm::vec4(0.80f, 0.12f, 0.02f, 0.0f),
            .shadow = glm::vec4(0.0f),
        };
        const shader::HelloDraw groundDraw{
            .model = glm::mat4(1.0f),
            .color = glm::vec4(0.45f, 0.46f, 0.43f, 1.0f),
            .shadow = glm::vec4(0.0f, 0.0f, 0.0f, 1.4f),
        };

        const gpu::ColorTarget color{.texture = frame.target, .clear = {clear.r, clear.g, clear.b, 1.0f}};
        const gpu::DepthTarget depth{.texture = m_depth};
        gpu::RenderPass pass = frame.commands.beginRenderPass(color, &depth);
        if (!pass) return;
        pass.bindPipeline(m_pipeline);
        pass.pushUniforms(ShaderStage::Vertex, 0, frameUniforms);
        pass.pushUniforms(ShaderStage::Fragment, 0, frameUniforms);
        for (const auto& [mesh, draw] : {std::pair{&m_ground, groundDraw}, std::pair{&m_torus, torusDraw}}) {
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
        m_torus = {};
        m_ground = {};
        m_pipeline.reset();
    }

    std::optional<double> scenarioValue(std::string_view name) const override {
        if (name == "yaw") return m_yaw;
        if (name == "paused") return m_paused ? 1.0 : 0.0;
        if (name == "speed") return m_speed;
        return m_net.value(name);
    }

    app::ScenarioExec scenarioCommand(App& app, const app::ScenarioCommand& command) override {
        (void)app;
        return m_net.command(command);
    }

private:
    static constexpr gpu::TextureFormat kDepthFormat = gpu::TextureFormat::Depth32Float;

    bool ensureDepth(gpu::Device& device, std::uint32_t width, std::uint32_t height) {
        if (m_depth && m_depth.desc().width == width && m_depth.desc().height == height) return true;
        Result<gpu::Texture> depth = device.createTexture({.format = kDepthFormat,
                                                           .usage = gpu::TextureUsage::DepthStencilTarget,
                                                           .width = width,
                                                           .height = height,
                                                           .name = "hello depth"});
        if (!depth) {
            log::error("hello", "depth target {}x{}: {}", width, height, depth.error());
            return false;
        }
        m_depth = std::move(*depth);
        return true;
    }

    gpu::GraphicsPipeline m_pipeline;
    Mesh m_torus;
    Mesh m_ground;
    gpu::Texture m_depth;
    app::ActionId m_spinLeft = 0, m_spinRight = 0, m_pause = 0, m_drag = 0;
    double m_yaw = -0.5;
    double m_previousYaw = -0.5;
    double m_speed = 0.8;
    bool m_paused = false;
    NetCheck m_net;
    std::optional<MicCheck> m_mic;
};

} // namespace

Result<std::unique_ptr<quench::app::Game>> quench::app::createGame(AppDesc& desc,
                                                                   std::span<const std::string_view> args) {
    desc.name = QUENCH_GAME_NAME;
    desc.organization = "Quench";
    desc.version = QUENCH_GAME_VERSION;
    std::optional<double> micCheck;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help") {
            std::printf("hello options:\n"
                        "  --mic-check SECONDS  ask for the microphone, record SECONDS (1 to 30) from the default\n"
                        "                       input, print the level and the length, write mic_check.wav into the\n"
                        "                       user directory and quit (docs/audio.md, \"The manual check\")\n");
            continue;
        }
        if (arg == "--mic-check") {
            const std::optional<double> seconds = i + 1 < args.size() ? parseDouble(args[++i]) : std::nullopt;
            if (!seconds || *seconds < 1.0 || *seconds > 30.0) return Error{"hello: --mic-check SECONDS (1 to 30)"};
            micCheck = *seconds;
            continue;
        }
        return Error{std::format("hello: unknown argument '{}' (--help lists the options)", arg)};
    }
    return std::unique_ptr<Game>(std::make_unique<Hello>(micCheck));
}
