#include "GpuTest.h"

#include "reload_test_shaders.h"

#include <quench/core/Json.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

using namespace quench;
using namespace quench::gpu;
using test::Rgba8;

#ifdef QUENCH_TEST_BUILD_DIR
namespace {

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool writeText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return bool(out);
}

bool replaceOnce(std::string& text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), to);
    return true;
}

// The colour a 4 x 4 target has after one fullscreen draw.
Rgba8 drawColor(Device& device, const GraphicsPipeline& pipeline) {
    Result<Texture> target = device.createTexture({.usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
    if (!target) return {};
    CommandBuffer commands = device.acquireCommandBuffer();
    {
        RenderPass pass = commands.beginRenderPass(ColorTarget{.texture = *target});
        pass.bindPipeline(pipeline);
        pass.draw(3);
    }
    if (!commands.submit()) return {};
    const std::vector<Rgba8> pixels = test::readRgba8(device, *target);
    return pixels.size() == 16 ? pixels[5] : Rgba8{};
}

// Polls the device until `done` holds for its reload status, at most 60 s.
template <class Done>
bool pollUntil(Device& device, Done done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < end) {
        device.pollShaderHotReload();
        if (done(device.shaderReloadStatus())) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

constexpr ColorTargetDesc kRgba8{.format = TextureFormat::Rgba8Unorm};

} // namespace
#endif

// Edits a copy of the set's source and of the file it includes (the manifest's copy points at them): each edit is
// compiled again and swapped into the live pipeline; a compile error keeps the code in use.
QUENCH_TEST(shader_hot_reload) {
#ifndef QUENCH_TEST_BUILD_DIR
    QUENCH_SKIP("no build tree here (shader hot reload runs on desktops)");
#else
    const ShaderBlob* vs = shaders::reload_test.find("reload_vs");
    const ShaderBlob* fs = shaders::reload_test.find("reload_fs");
    REQUIRE(vs && fs, "reload_test entries");
    if (!fs->manifest) QUENCH_SKIP("no shader manifests in this build");
    QUENCH_GPU_DEVICE(device);

    // A build directory of its own: the manifest at the blobs' path, with the source and the output in it.
    const Caps& caps = device.caps();
    const std::filesystem::path root =
        std::filesystem::path(test::tmpDir()) /
        ("shader_hot_reload_" + std::string(backendName(caps.backend)) + "_" + caps.driver);
    std::filesystem::remove_all(root);
    Result<json::Value> manifest = json::parse(readText(std::filesystem::path(QUENCH_TEST_BUILD_DIR) / fs->manifest));
    REQUIRE(manifest, "manifest: %s", manifest.error().c_str());
    const std::filesystem::path original = (*manifest)["source"].stringOr("");
    const std::filesystem::path source = root / "src" / "reload.hlsl";
    const std::filesystem::path include = root / "src" / "reload_color.hlsli";
    std::string sourceText = readText(original);
    std::string includeText = readText(original.parent_path() / "reload_color.hlsli");
    REQUIRE(!sourceText.empty() && !includeText.empty(), "sources next to %s", original.string().c_str());
    manifest->object["source"] = json::Value::makeString(source.string());
    manifest->object["output"] = json::Value::makeString((root / "out").string());
    REQUIRE(writeText(source, sourceText) && writeText(include, includeText), "copies");
    REQUIRE(writeText(root / fs->manifest, json::write(*manifest)), "manifest copy");

    Result<GraphicsPipeline> pipeline =
        device.createPipeline({.vertexShader = vs, .fragmentShader = fs, .colorTargets = {&kRgba8, 1}});
    REQUIRE(pipeline, "%s", pipeline.error().c_str());
    REQUIRE(device.enableShaderHotReload(root.string()), "enable");
    device.pollShaderHotReload(); // watches the set from here
    CHECK(test::near(drawColor(device, *pipeline), {255, 0, 0, 255}), "embedded code: red");

    // The source: the channels swapped.
    REQUIRE(replaceOnce(sourceText, "float4(RELOAD_COLOR, 1.0)", "float4(RELOAD_COLOR.bgr, 1.0)"), "edit");
    REQUIRE(writeText(source, sourceText), "write");
    REQUIRE(pollUntil(device, [](const ShaderReloadStatus& s) { return s.reloads >= 1 || s.failures > 0; }),
            "no reload in 60 s");
    ShaderReloadStatus status = device.shaderReloadStatus();
    REQUIRE(status.reloads == 1 && status.failures == 0, "reloads %u, failures %u: %s", status.reloads, status.failures,
            status.lastError.c_str());
    CHECK(status.pipelines == 1, "%u pipelines created again", status.pipelines);
    const Rgba8 blue = drawColor(device, *pipeline);
    CHECK(test::near(blue, {0, 0, 255, 255}), "after the source edit: %d %d %d, expected blue", blue.r, blue.g, blue.b);
    // A pipeline created after the reload gets the new code too.
    Result<GraphicsPipeline> later =
        device.createPipeline({.vertexShader = vs, .fragmentShader = fs, .colorTargets = {&kRgba8, 1}});
    REQUIRE(later, "%s", later.error().c_str());
    CHECK(test::near(drawColor(device, *later), {0, 0, 255, 255}), "a pipeline created after the reload");

    // The included file (watched from the reload's depfile).
    REQUIRE(replaceOnce(includeText, "float3(1.0, 0.0, 0.0)", "float3(0.0, 1.0, 0.0)"), "edit");
    REQUIRE(writeText(include, includeText), "write");
    REQUIRE(pollUntil(device, [](const ShaderReloadStatus& s) { return s.reloads >= 2 || s.failures > 0; }),
            "no reload in 60 s");
    status = device.shaderReloadStatus();
    REQUIRE(status.reloads == 2 && status.failures == 0, "reloads %u, failures %u: %s", status.reloads, status.failures,
            status.lastError.c_str());
    CHECK(status.pipelines == 3, "%u pipelines created again", status.pipelines);
    const Rgba8 green = drawColor(device, *pipeline);
    CHECK(test::near(green, {0, 255, 0, 255}), "after the include edit: %d %d %d, expected green", green.r, green.g,
          green.b);

    // A compile error: both pipelines keep the code they have.
    REQUIRE(replaceOnce(sourceText, "float4(RELOAD_COLOR.bgr, 1.0)", "float4(RELOAD_COLOR.bgr, 1.0) +"), "edit");
    REQUIRE(writeText(source, sourceText), "write");
    REQUIRE(pollUntil(device, [](const ShaderReloadStatus& s) { return s.failures >= 1 && !s.compiling; }),
            "no failure in 60 s");
    status = device.shaderReloadStatus();
    CHECK(status.reloads == 2 && status.failures == 1, "reloads %u, failures %u", status.reloads, status.failures);
    CHECK(status.lastError.find("reload_fs") != std::string::npos, "error: %s", status.lastError.c_str());
    CHECK(test::near(drawColor(device, *pipeline), {0, 255, 0, 255}), "the old code after the error");
    CHECK(test::near(drawColor(device, *later), {0, 255, 0, 255}), "the old code after the error");

    later->reset();
    pipeline->reset();
    std::filesystem::remove_all(root);
#endif
}
