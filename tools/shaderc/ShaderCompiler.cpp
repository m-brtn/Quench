// quench_shaderc: build-time shader compiler (host tool, see cmake/QuenchShaders.cmake).
//
// Compiles one entry point of an HLSL file to SPIR-V for SDL GPU's Vulkan backend (glslang HLSL front end,
// SPIRV-Tools legalization + validation) and, optionally, translates that SPIR-V to MSL for SDL GPU's Metal backend
// (SPIRV-Cross), remapping the resources to SDL's MSL slots:
//   uniform buffer b[n]            -> [[buffer(n)]]
//   texture t[n] + sampler s[n]    -> [[texture(n)]] + [[sampler(n)]]
//   storage buffer (k-th by slot)  -> [[buffer(uniform buffer count + k)]]
// Descriptor sets must follow SDL's SPIR-V rules: vertex resources in set 0 and uniforms in set 1, fragment resources
// in set 2 and uniforms in set 3. DXIL is produced separately by DXC (Windows hosts only).
//
// QUENCH_ENTRY_<entry> and QUENCH_STAGE_VERTEX or QUENCH_STAGE_FRAGMENT are defined while compiling. #include "x"
// resolves relative to the including file, then in the --include-dir directories in order. --depfile writes the
// input and every included file as Make dependencies of the SPIR-V output; --reflect writes the stage and the
// resource counts SDL GPU needs to create the shader, as key=value lines.
//
// Usage: quench_shaderc --input FILE.hlsl --entry NAME --stage vertex|fragment --spirv OUT.spv [--reflect OUT]
//                       [--msl OUT.metal] [--msl-platform macos|ios] [--include-dir DIR]... [--depfile OUT.d]

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Args {
    std::string input, entry, stage, spirvOut, reflectOut, mslOut, mslPlatform = "macos", depfileOut;
    std::vector<std::string> includeDirs;
};

bool parseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--input")
            a.input = v;
        else if (k == "--entry")
            a.entry = v;
        else if (k == "--stage")
            a.stage = v;
        else if (k == "--spirv")
            a.spirvOut = v;
        else if (k == "--reflect")
            a.reflectOut = v;
        else if (k == "--msl")
            a.mslOut = v;
        else if (k == "--msl-platform")
            a.mslPlatform = v;
        else if (k == "--include-dir")
            a.includeDirs.push_back(v);
        else if (k == "--depfile")
            a.depfileOut = v;
        else
            return false;
    }
    return argc % 2 == 1 && (a.mslPlatform == "macos" || a.mslPlatform == "ios") && !a.input.empty() &&
           !a.entry.empty() && (a.stage == "vertex" || a.stage == "fragment") && !a.spirvOut.empty();
}

bool readText(const fs::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeBytes(const std::string& path, const void* data, std::size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), std::streamsize(size));
    return bool(f);
}

bool writeText(const std::string& path, const std::string& text) { return writeBytes(path, text.data(), text.size()); }

int fail(const Args& a, const std::string& msg) {
    std::fprintf(stderr, "quench_shaderc: %s (%s): %s\n", a.entry.c_str(), a.input.c_str(), msg.c_str());
    return 1;
}

// Resolves #include and records every file it opens.
class Includer final : public glslang::TShader::Includer {
public:
    explicit Includer(std::vector<std::string> dirs) : m_dirs(std::move(dirs)) {}

    IncludeResult* includeLocal(const char* header, const char* includer, std::size_t) override {
        const fs::path base = fs::path(includer).parent_path();
        return open(base / header);
    }

    IncludeResult* includeSystem(const char* header, const char*, std::size_t) override {
        for (const std::string& dir : m_dirs)
            if (IncludeResult* r = open(fs::path(dir) / header)) return r;
        return nullptr;
    }

    void releaseInclude(IncludeResult* result) override {
        if (!result) return;
        delete static_cast<std::string*>(result->userData);
        delete result;
    }

    const std::set<std::string>& files() const { return m_files; }

private:
    IncludeResult* open(const fs::path& path) {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) return nullptr;
        auto* text = new std::string;
        if (!readText(path, *text)) {
            delete text;
            return nullptr;
        }
        const std::string name = fs::absolute(path, ec).lexically_normal().string();
        m_files.insert(name);
        return new IncludeResult(name, text->data(), text->size(), text);
    }

    std::vector<std::string> m_dirs;
    std::set<std::string> m_files;
};

bool compileHlsl(const Args& a, const std::string& source, Includer& includer, std::vector<std::uint32_t>& spirv,
                 std::string& log) {
    const bool vertex = a.stage == "vertex";
    const EShLanguage lang = vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader shader(lang);
    const std::string preamble = "#define QUENCH_ENTRY_" + a.entry + " 1\n#define QUENCH_STAGE_" +
                                 std::string(vertex ? "VERTEX" : "FRAGMENT") + " 1\n";
    const char* text = source.c_str();
    const char* name = a.input.c_str();
    shader.setStringsWithLengthsAndNames(&text, nullptr, &name, 1);
    shader.setPreamble(preamble.c_str());
    shader.setEntryPoint(a.entry.c_str());
    shader.setSourceEntryPoint(a.entry.c_str());
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setHlslIoMapping(true);
    shader.setAutoMapLocations(true);
    const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!shader.parse(GetDefaultResources(), 100, false, messages, includer)) {
        log = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages) || !program.mapIO()) {
        log = std::string(program.getInfoLog()) + program.getInfoDebugLog();
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    options.validate = false;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &options);
    log = logger.getAllMessages();
    return !spirv.empty();
}

// glslang leaves HLSL constructs (resources passed to functions, struct copies) that Vulkan rejects until the
// SPIRV-Tools legalization passes have run (the same step glslangValidator and DXC perform).
bool legalizeAndValidate(std::vector<std::uint32_t>& spirv, std::string& log) {
    const spv_target_env env = SPV_ENV_VULKAN_1_0;
    auto consumer = [&log](spv_message_level_t, const char*, const spv_position_t& pos, const char* msg) {
        log += "  [" + std::to_string(pos.index) + "] " + msg + "\n";
    };
    spvtools::Optimizer opt(env);
    opt.SetMessageConsumer(consumer);
    opt.RegisterLegalizationPasses();
    std::vector<std::uint32_t> legal;
    spvtools::ValidatorOptions vopts;
    vopts.SetRelaxBlockLayout(true);
    if (!opt.Run(spirv.data(), spirv.size(), &legal, vopts, true)) return false;
    spvtools::SpirvTools tools(env);
    tools.SetMessageConsumer(consumer);
    if (!tools.Validate(legal.data(), legal.size(), vopts)) return false;
    spirv.swap(legal);
    return true;
}

struct Slots {
    std::uint32_t uniformBuffers = 0, samplers = 0, storageBuffers = 0;
};

// Checks the descriptor sets, counts the resources and maps them to SDL's MSL slots; with `msl`, also translates.
bool mapResources(const Args& a, const std::vector<std::uint32_t>& spirv, Slots& slots, std::string* msl,
                  std::string& log) {
    const bool vertex = a.stage == "vertex";
    const std::uint32_t resourceSet = vertex ? 0 : 2, uniformSet = vertex ? 1 : 3;
    const spv::ExecutionModel model = vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
    spirv_cross::CompilerMSL compiler(spirv);
    spirv_cross::CompilerMSL::Options options;
    options.platform =
        a.mslPlatform == "ios" ? spirv_cross::CompilerMSL::Options::iOS : spirv_cross::CompilerMSL::Options::macOS;
    options.set_msl_version(3, 0);
    compiler.set_msl_options(options);

    const auto active = compiler.get_active_interface_variables();
    const spirv_cross::ShaderResources res = compiler.get_shader_resources(active);
    compiler.set_enabled_interface_variables(active);
    auto setOf = [&](const spirv_cross::Resource& r) {
        return compiler.get_decoration(r.id, spv::DecorationDescriptorSet);
    };
    auto bindingOf = [&](const spirv_cross::Resource& r) {
        return compiler.get_decoration(r.id, spv::DecorationBinding);
    };
    auto check = [&](const spirv_cross::Resource& r, std::uint32_t set, const char* what) {
        if (setOf(r) == set) return true;
        log += std::string(what) + " '" + r.name + "' is in descriptor set " + std::to_string(setOf(r)) +
               ", SDL expects " + std::to_string(set) + "\n";
        return false;
    };

    std::map<std::uint32_t, spirv_cross::MSLResourceBinding> bindings; // by (resource set) binding
    auto entry = [&](std::uint32_t set, std::uint32_t binding) -> spirv_cross::MSLResourceBinding& {
        spirv_cross::MSLResourceBinding& b = bindings[(set << 16) | binding];
        b.stage = model;
        b.desc_set = set;
        b.binding = binding;
        return b;
    };
    bool ok = true;
    for (const auto& r : res.uniform_buffers) {
        ok &= check(r, uniformSet, "uniform buffer");
        entry(setOf(r), bindingOf(r)).msl_buffer = bindingOf(r);
        slots.uniformBuffers = std::max(slots.uniformBuffers, bindingOf(r) + 1);
    }
    for (const auto& r : res.separate_images) {
        ok &= check(r, resourceSet, "texture");
        entry(setOf(r), bindingOf(r)).msl_texture = bindingOf(r);
        slots.samplers = std::max(slots.samplers, bindingOf(r) + 1);
    }
    for (const auto& r : res.separate_samplers) {
        ok &= check(r, resourceSet, "sampler");
        entry(setOf(r), bindingOf(r)).msl_sampler = bindingOf(r);
    }
    std::vector<std::uint32_t> storage;
    for (const auto& r : res.storage_buffers) {
        ok &= check(r, resourceSet, "storage buffer");
        storage.push_back(bindingOf(r));
    }
    std::sort(storage.begin(), storage.end());
    for (std::size_t i = 0; i < storage.size(); ++i)
        entry(resourceSet, storage[i]).msl_buffer = slots.uniformBuffers + std::uint32_t(i);
    slots.storageBuffers = std::uint32_t(storage.size());
    if (!res.storage_images.empty() || !res.sampled_images.empty()) {
        log += "storage textures and combined image samplers are not supported yet\n";
        ok = false;
    }
    if (!ok || !msl) return ok;
    for (auto& [key, b] : bindings) compiler.add_msl_resource_binding(b);
    *msl = compiler.compile();
    for (const auto& [key, b] : bindings) {
        if (!compiler.is_msl_resource_binding_used(model, b.desc_set, b.binding)) {
            log += "resource set " + std::to_string(b.desc_set) + " binding " + std::to_string(b.binding) +
                   " was not remapped\n";
            ok = false;
        }
    }
    return ok;
}

std::string makeEscape(const std::string& path) {
    std::string out;
    for (const char c : path) {
        if (c == ' ' || c == '#') out += '\\';
        if (c == '$') out += '$';
        out += c;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parseArgs(argc, argv, a)) {
        std::fprintf(stderr, "usage: quench_shaderc --input FILE.hlsl --entry NAME --stage vertex|fragment "
                             "--spirv OUT.spv [--reflect OUT] [--msl OUT.metal] [--msl-platform macos|ios] "
                             "[--include-dir DIR]... [--depfile OUT.d]\n");
        return 2;
    }
    std::string source;
    if (!readText(a.input, source)) return fail(a, "cannot read the input");

    Includer includer(a.includeDirs);
    glslang::InitializeProcess();
    std::vector<std::uint32_t> spirv;
    std::string log;
    const bool compiled = compileHlsl(a, source, includer, spirv, log);
    glslang::FinalizeProcess();
    if (!compiled) return fail(a, "HLSL compile failed\n" + log);
    log.clear();
    if (!legalizeAndValidate(spirv, log)) return fail(a, "SPIR-V legalization/validation failed\n" + log);

    Slots slots;
    std::string msl;
    log.clear();
    try {
        if (!mapResources(a, spirv, slots, a.mslOut.empty() ? nullptr : &msl, log))
            return fail(a, "resource mapping failed\n" + log);
    } catch (const std::exception& e) { return fail(a, std::string("SPIRV-Cross: ") + e.what()); }

    if (!writeBytes(a.spirvOut, spirv.data(), spirv.size() * sizeof(std::uint32_t)))
        return fail(a, "cannot write " + a.spirvOut);
    if (!a.mslOut.empty() && !writeText(a.mslOut, msl)) return fail(a, "cannot write " + a.mslOut);
    if (!a.reflectOut.empty()) {
        const std::string reflect = "stage=" + a.stage + "\nsamplers=" + std::to_string(slots.samplers) +
                                    "\nstorage_textures=0\nstorage_buffers=" + std::to_string(slots.storageBuffers) +
                                    "\nuniform_buffers=" + std::to_string(slots.uniformBuffers) + "\n";
        if (!writeText(a.reflectOut, reflect)) return fail(a, "cannot write " + a.reflectOut);
    }
    if (!a.depfileOut.empty()) {
        std::error_code ec;
        std::string deps = makeEscape(fs::absolute(a.spirvOut, ec).lexically_normal().string()) + ":";
        deps += " " + makeEscape(fs::absolute(a.input, ec).lexically_normal().string());
        for (const std::string& file : includer.files()) deps += " " + makeEscape(file);
        if (!writeText(a.depfileOut, deps + "\n")) return fail(a, "cannot write " + a.depfileOut);
    }
    return 0;
}
