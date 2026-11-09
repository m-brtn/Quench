// quench_shaderc: build-time shader compiler (host tool, see cmake/QuenchShaders.cmake).
//
// Compiles one entry point of an HLSL file to SPIR-V for SDL GPU's Vulkan backend (glslang HLSL front end,
// SPIRV-Tools legalization + validation) and, optionally, translates that SPIR-V to MSL for SDL GPU's Metal backend
// (SPIRV-Cross), remapping the resources to SDL's MSL slots:
//   uniform buffer b[n]            -> [[buffer(n)]]
//   texture t[n] + sampler s[n]    -> [[texture(n)]] + [[sampler(n)]]
//   storage buffer (k-th by slot)  -> [[buffer(uniform buffer count + k)]]
//   compute: read-only storage buffers, then read-write ones, after the uniform buffers
// Descriptor sets must follow SDL's SPIR-V rules: vertex resources in set 0 and uniforms in set 1, fragment resources
// in set 2 and uniforms in set 3; compute read-only resources in set 0, read-write ones in set 1, uniforms in set 2.
// DXIL is produced separately by DXC (Windows hosts only).
//
// Entries with ray queries (RayQuery, RaytracingAccelerationStructure), which glslang cannot parse, are compiled with
// --dxc: DXC (shader model 6.5) emits SPIR-V 1.5 for Vulkan 1.2, which is validated and translated the same way. Their
// acceleration structures take the buffer slots after the storage buffers (after the read-write ones in compute).
//
// QUENCH_ENTRY_<entry> and QUENCH_STAGE_VERTEX, QUENCH_STAGE_FRAGMENT or QUENCH_STAGE_COMPUTE are defined while
// compiling. #include "x" resolves relative to the including file, then in the --include-dir directories in order.
// --depfile writes the input and every included file as Make dependencies of the SPIR-V output; --reflect writes the
// stage, the resource counts SDL GPU needs to create the shader and a compute shader's thread group size, as
// key=value lines.
//
// Usage: quench_shaderc --input FILE.hlsl --entry NAME --stage vertex|fragment|compute --spirv OUT.spv
//                       [--reflect OUT] [--msl OUT.metal] [--msl-platform macos|ios] [--include-dir DIR]...
//                       [--depfile OUT.d] [--dxc PATH]

#include "core/Platform.h"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv_msl.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    std::string input, entry, stage, spirvOut, reflectOut, mslOut, mslPlatform = "macos", depfileOut, dxc;
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
        else if (k == "--dxc")
            a.dxc = v;
        else
            return false;
    }
    return argc % 2 == 1 && (a.mslPlatform == "macos" || a.mslPlatform == "ios") && !a.input.empty() &&
           !a.entry.empty() && (a.stage == "vertex" || a.stage == "fragment" || a.stage == "compute") &&
           !a.spirvOut.empty();
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
    const EShLanguage lang = a.stage == "vertex"     ? EShLangVertex
                             : a.stage == "fragment" ? EShLangFragment
                                                     : EShLangCompute;
    glslang::TShader shader(lang);
    std::string stageName = a.stage;
    std::transform(stageName.begin(), stageName.end(), stageName.begin(), [](char c) { return char(c - 'a' + 'A'); });
    const std::string preamble = "#define QUENCH_ENTRY_" + a.entry + " 1\n#define QUENCH_STAGE_" + stageName + " 1\n";
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

// Runs glslang's preprocessor only, for the files a DXC-compiled entry includes (the depfile): it resolves #include
// like DXC and needs no HLSL parser for what it skips.
bool preprocessIncludes(const Args& a, const std::string& source, Includer& includer, std::string& log) {
    const EShLanguage lang = a.stage == "vertex"     ? EShLangVertex
                             : a.stage == "fragment" ? EShLangFragment
                                                     : EShLangCompute;
    glslang::TShader shader(lang);
    std::string stageName = a.stage;
    std::transform(stageName.begin(), stageName.end(), stageName.begin(), [](char c) { return char(c - 'a' + 'A'); });
    const std::string preamble = "#define QUENCH_ENTRY_" + a.entry + " 1\n#define QUENCH_STAGE_" + stageName + " 1\n";
    const char* text = source.c_str();
    const char* name = a.input.c_str();
    shader.setStringsWithLengthsAndNames(&text, nullptr, &name, 1);
    shader.setPreamble(preamble.c_str());
    shader.setEntryPoint(a.entry.c_str());
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    std::string output;
    const auto messages = EShMessages(EShMsgReadHlsl);
    if (!shader.preprocess(GetDefaultResources(), 100, ENoProfile, false, false, messages, &output, includer)) {
        log = std::string(shader.getInfoLog()) + shader.getInfoDebugLog();
        return false;
    }
    return true;
}

// An argument for std::system's shell: sh on POSIX hosts; on Windows cmd.exe, which hands the line to the program,
// whose C runtime splits it (only a quote is escaped there; backslashes in paths stay as they are).
std::string quoteArg(const std::string& arg) {
    std::string out = "\"";
    for (const char c : arg) {
        if (QUENCH_PLATFORM_WINDOWS ? c == '"' : c == '"' || c == '\\' || c == '$' || c == '`') out += '\\';
        out += c;
    }
    return out + "\"";
}

// DXC (a separate executable) -> SPIR-V 1.5 for Vulkan 1.2, shader model 6.5 (ray queries).
bool compileDxc(const Args& a, std::vector<std::uint32_t>& spirv, std::string& log) {
    const char* profile = a.stage == "vertex" ? "vs_6_5" : a.stage == "fragment" ? "ps_6_5" : "cs_6_5";
    std::string stageName = a.stage;
    std::transform(stageName.begin(), stageName.end(), stageName.begin(), [](char c) { return char(c - 'a' + 'A'); });
    const std::string out = a.spirvOut + ".dxc";
    const std::string errors = a.spirvOut + ".dxc.log";
    std::string command = quoteArg(a.dxc) + " -nologo -spirv -fspv-target-env=vulkan1.2 -HV 2021 -O3 -T " + profile +
                          " -E " + quoteArg(a.entry) + " -D QUENCH_ENTRY_" + a.entry + "=1 -D QUENCH_STAGE_" +
                          stageName + "=1";
    for (const std::string& dir : a.includeDirs) command += " -I " + quoteArg(dir);
    command += " -Fo " + quoteArg(out) + " " + quoteArg(a.input) + " > " + quoteArg(errors) + " 2>&1";
    // cmd.exe /c drops the first and the last quote of a line that starts with one.
    if (QUENCH_PLATFORM_WINDOWS) command = "\"" + command + "\"";
    const int status = std::system(command.c_str());
    std::string text;
    readText(errors, text);
    std::error_code ec;
    fs::remove(errors, ec);
    if (status != 0) {
        log = text.empty() ? "dxc failed (" + std::to_string(status) + "): " + command : text;
        return false;
    }
    std::string bytes;
    const bool read = readText(out, bytes);
    fs::remove(out, ec);
    if (!read || bytes.size() % 4 != 0 || bytes.empty()) {
        log = "dxc wrote no SPIR-V";
        return false;
    }
    spirv.resize(bytes.size() / 4);
    std::memcpy(spirv.data(), bytes.data(), bytes.size());
    return true;
}

// glslang leaves HLSL constructs (resources passed to functions, struct copies) that Vulkan rejects until the
// SPIRV-Tools legalization passes have run (the same step glslangValidator and DXC perform). DXC's output is legal
// already and only validated.
bool legalizeAndValidate(std::vector<std::uint32_t>& spirv, bool legalize, spv_target_env env, std::string& log) {
    auto consumer = [&log](spv_message_level_t, const char*, const spv_position_t& pos, const char* msg) {
        log += "  [" + std::to_string(pos.index) + "] " + msg + "\n";
    };
    spvtools::ValidatorOptions vopts;
    vopts.SetRelaxBlockLayout(true);
    std::vector<std::uint32_t> legal;
    if (legalize) {
        spvtools::Optimizer opt(env);
        opt.SetMessageConsumer(consumer);
        opt.RegisterLegalizationPasses();
        if (!opt.Run(spirv.data(), spirv.size(), &legal, vopts, true)) return false;
    } else {
        legal = spirv;
    }
    spvtools::SpirvTools tools(env);
    tools.SetMessageConsumer(consumer);
    if (!tools.Validate(legal.data(), legal.size(), vopts)) return false;
    spirv.swap(legal);
    return true;
}

// glslang gives the block type of a StructuredBuffer the variable's name, and buffers of one element type share that
// type. MoltenVK's argument buffers then declare `device b_a* b_b`, where the type name is hidden by the variable
// b_a, and the pipeline fails to compile. Struct types whose debug name is also another id's get "_block" appended.
void renameBlockTypes(std::vector<std::uint32_t>& spirv) {
    constexpr std::uint32_t kOpName = 5, kOpTypeStruct = 30, kHeaderWords = 5;
    auto literal = [&](std::size_t word, std::size_t end) {
        std::string s;
        for (std::size_t w = word; w < end; ++w)
            for (int b = 0; b < 4; ++b) {
                const char c = char((spirv[w] >> (8 * b)) & 0xff);
                if (c == '\0') return s;
                s += c;
            }
        return s;
    };
    std::set<std::uint32_t> structs;
    std::map<std::string, int> names;                             // name -> number of ids carrying it
    std::vector<std::pair<std::size_t, std::string>> structNames; // OpName offset, name
    std::vector<std::pair<std::size_t, std::uint32_t>> nameOps;   // OpName offset, target id
    for (std::size_t i = kHeaderWords; i < spirv.size();) {
        const std::uint32_t count = spirv[i] >> 16, op = spirv[i] & 0xffff;
        if (count == 0 || i + count > spirv.size()) return;
        if (op == kOpTypeStruct) structs.insert(spirv[i + 1]);
        if (op == kOpName) nameOps.emplace_back(i, spirv[i + 1]);
        i += count;
    }
    for (const auto& [offset, id] : nameOps) {
        const std::string name = literal(offset + 2, offset + (spirv[offset] >> 16));
        ++names[name];
        if (structs.count(id)) structNames.emplace_back(offset, name);
    }
    std::map<std::size_t, std::string> renamed;
    for (const auto& [offset, name] : structNames)
        if (!name.empty() && names[name] > 1) renamed[offset] = name + "_block";
    if (renamed.empty()) return;
    std::vector<std::uint32_t> out(spirv.begin(), spirv.begin() + kHeaderWords);
    for (std::size_t i = kHeaderWords; i < spirv.size();) {
        const std::uint32_t count = spirv[i] >> 16;
        const auto it = renamed.find(i);
        if (it == renamed.end()) {
            out.insert(out.end(), spirv.begin() + std::ptrdiff_t(i), spirv.begin() + std::ptrdiff_t(i + count));
        } else {
            const std::string& name = it->second;
            const std::uint32_t words = std::uint32_t(name.size() / 4 + 1);
            out.push_back(((2 + words) << 16) | kOpName);
            out.push_back(spirv[i + 1]);
            std::vector<std::uint32_t> text(words, 0);
            std::memcpy(text.data(), name.data(), name.size());
            out.insert(out.end(), text.begin(), text.end());
        }
        i += count;
    }
    spirv.swap(out);
}

struct Slots {
    std::uint32_t uniformBuffers = 0, samplers = 0, storageBuffers = 0, readWriteStorageBuffers = 0;
    std::uint32_t accelerationStructures = 0;
    std::uint32_t threads[3] = {1, 1, 1};
};

// Checks the descriptor sets, counts the resources and maps them to SDL's MSL slots; with `msl`, also translates.
bool mapResources(const Args& a, const std::vector<std::uint32_t>& spirv, Slots& slots, std::string* msl,
                  std::string& log) {
    const bool vertex = a.stage == "vertex", compute = a.stage == "compute";
    const std::uint32_t resourceSet = vertex || compute ? 0 : 2, uniformSet = vertex ? 1 : compute ? 2 : 3;
    const std::uint32_t writeSet = 1; // compute only
    const spv::ExecutionModel model = vertex    ? spv::ExecutionModelVertex
                                      : compute ? spv::ExecutionModelGLCompute
                                                : spv::ExecutionModelFragment;
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
    std::vector<std::uint32_t> storage, writable;
    for (const auto& r : res.storage_buffers) {
        if (compute && setOf(r) == writeSet) {
            writable.push_back(bindingOf(r));
            continue;
        }
        ok &= check(r, resourceSet, "storage buffer");
        storage.push_back(bindingOf(r));
    }
    std::sort(storage.begin(), storage.end());
    std::sort(writable.begin(), writable.end());
    for (std::size_t i = 0; i < storage.size(); ++i)
        entry(resourceSet, storage[i]).msl_buffer = slots.uniformBuffers + std::uint32_t(i);
    for (std::size_t i = 0; i < writable.size(); ++i)
        entry(writeSet, writable[i]).msl_buffer =
            slots.uniformBuffers + std::uint32_t(storage.size()) + std::uint32_t(i);
    slots.storageBuffers = std::uint32_t(storage.size());
    slots.readWriteStorageBuffers = std::uint32_t(writable.size());
    std::vector<std::uint32_t> structures;
    for (const auto& r : res.acceleration_structures) {
        ok &= check(r, resourceSet, "acceleration structure");
        structures.push_back(bindingOf(r));
    }
    std::sort(structures.begin(), structures.end());
    for (std::size_t i = 0; i < structures.size(); ++i) {
        if (bindings.count((resourceSet << 16) | structures[i])) {
            log += "acceleration structure binding " + std::to_string(structures[i]) + " is also another resource's\n";
            ok = false;
        }
        entry(resourceSet, structures[i]).msl_buffer =
            slots.uniformBuffers + std::uint32_t(storage.size() + writable.size() + i);
    }
    slots.accelerationStructures = std::uint32_t(structures.size());
    if (compute)
        for (std::uint32_t i = 0; i < 3; ++i)
            slots.threads[i] = std::max(1u, compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, i));
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
        std::fprintf(stderr, "usage: quench_shaderc --input FILE.hlsl --entry NAME --stage vertex|fragment|compute "
                             "--spirv OUT.spv [--reflect OUT] [--msl OUT.metal] [--msl-platform macos|ios] "
                             "[--include-dir DIR]... [--depfile OUT.d] [--dxc PATH]\n");
        return 2;
    }
    std::string source;
    if (!readText(a.input, source)) return fail(a, "cannot read the input");

    Includer includer(a.includeDirs);
    glslang::InitializeProcess();
    std::vector<std::uint32_t> spirv;
    std::string log;
    const bool dxc = !a.dxc.empty();
    const bool compiled = dxc ? preprocessIncludes(a, source, includer, log) && compileDxc(a, spirv, log)
                              : compileHlsl(a, source, includer, spirv, log);
    glslang::FinalizeProcess();
    if (!compiled) return fail(a, std::string(dxc ? "DXC" : "HLSL") + " compile failed\n" + log);
    log.clear();
    if (!legalizeAndValidate(spirv, !dxc, dxc ? SPV_ENV_VULKAN_1_2 : SPV_ENV_VULKAN_1_0, log))
        return fail(a, "SPIR-V legalization/validation failed\n" + log);
    renameBlockTypes(spirv);

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
        const std::string reflect =
            "stage=" + a.stage + "\nsamplers=" + std::to_string(slots.samplers) +
            "\nstorage_textures=0\nstorage_buffers=" + std::to_string(slots.storageBuffers) +
            "\nuniform_buffers=" + std::to_string(slots.uniformBuffers) +
            "\nreadwrite_storage_textures=0\nreadwrite_storage_buffers=" +
            std::to_string(slots.readWriteStorageBuffers) + "\nthreads_x=" + std::to_string(slots.threads[0]) +
            "\nthreads_y=" + std::to_string(slots.threads[1]) + "\nthreads_z=" + std::to_string(slots.threads[2]) +
            "\nacceleration_structures=" + std::to_string(slots.accelerationStructures) + "\n";
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
