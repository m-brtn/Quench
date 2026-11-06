// Pipeline slots and shader hot reload (src/gpu/ShaderReload.h; docs/app.md, "Shader hot reload"). A development tool
// on desktops: it reads the build tree directly (the manifests, the sources' times and the compiled files are not
// assets, so not quench::fs) and runs the build's compilers as processes, polled every frame and never waited for.

#include "gpu/ShaderReload.h"

#include "core/Platform.h"

#include <quench/core/Json.h>
#include <quench/core/Log.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace quench::gpu {

namespace detail {

namespace {

constexpr Uint64 kCheckIntervalNs = 500'000'000;
constexpr std::size_t kMaxProcesses = 4;
constexpr std::size_t kMaxOutputBytes = 16 * 1024;
constexpr std::size_t kMaxLoggedBytes = 600;

std::optional<SDL_Time> modifiedTime(const std::string& path) {
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) return std::nullopt;
    return info.modify_time;
}

Result<std::vector<std::uint8_t>> readFile(const std::string& path) {
    std::size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) return Error{std::format("cannot read {}: {}", path, SDL_GetError())};
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::vector<std::uint8_t> out(bytes, bytes + size);
    SDL_free(data);
    return out;
}

// The files a Make depfile of quench_shaderc names after its target ("out.spv: in.hlsl a.hlsli \\\n b.hlsli", with
// '\ ', '\#' and '$$' escapes).
std::vector<std::string> readDepfile(const std::string& path) {
    Result<std::vector<std::uint8_t>> bytes = readFile(path);
    if (!bytes) return {};
    const std::string text(bytes->begin(), bytes->end());
    std::vector<std::string> files;
    std::string current;
    bool target = true;
    const auto flush = [&] {
        if (!current.empty() && !target) files.push_back(current);
        current.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '\\' && (next == ' ' || next == '#' || next == '\\')) {
            current += next;
            ++i;
        } else if (c == '$' && next == '$') {
            current += '$';
            ++i;
        } else if (c == '\\' && (next == '\n' || next == '\r')) {
            flush();
        } else if (c == ':' && target && (next == ' ' || next == '\n' || next == '\0')) {
            current.clear();
            target = false;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            current += c;
        }
    }
    flush();
    return files;
}

// The resource counts quench_shaderc writes (<entry>.reflect: key=value lines), as EmbedShaders.cmake reads them.
Result<std::pair<ShaderStage, ShaderResources>> readReflect(const std::string& path) {
    Result<std::vector<std::uint8_t>> bytes = readFile(path);
    if (!bytes) return Error{bytes.error()};
    const std::string text(bytes->begin(), bytes->end());
    std::map<std::string, std::string, std::less<>> values;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        const std::string_view line(text.data() + begin, end - begin);
        if (const std::size_t eq = line.find('='); eq != std::string_view::npos)
            values[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
        begin = end + 1;
    }
    const auto number = [&](std::string_view key) -> std::optional<std::uint32_t> {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return std::uint32_t(std::strtoul(it->second.c_str(), nullptr, 10));
    };
    ShaderResources r{};
    const std::string_view keys[] = {"samplers",
                                     "storage_textures",
                                     "storage_buffers",
                                     "uniform_buffers",
                                     "readwrite_storage_textures",
                                     "readwrite_storage_buffers",
                                     "threads_x",
                                     "threads_y",
                                     "threads_z"};
    std::uint32_t* fields[] = {&r.samplers,       &r.storageTextures,          &r.storageBuffers,
                               &r.uniformBuffers, &r.readWriteStorageTextures, &r.readWriteStorageBuffers,
                               &r.threadCount[0], &r.threadCount[1],           &r.threadCount[2]};
    for (std::size_t i = 0; i < std::size(keys); ++i) {
        const std::optional<std::uint32_t> value = number(keys[i]);
        if (!value) return Error{std::format("{}: '{}' missing", path, keys[i])};
        *fields[i] = *value;
    }
    r.accelerationStructures = number("acceleration_structures").value_or(0);
    const std::string stage = values.count("stage") ? values["stage"] : std::string();
    const ShaderStage s = stage == "vertex"     ? ShaderStage::Vertex
                          : stage == "fragment" ? ShaderStage::Fragment
                                                : ShaderStage::Compute;
    return std::pair{s, r};
}

#if QUENCH_PLATFORM_MACOS || QUENCH_PLATFORM_LINUX || QUENCH_PLATFORM_WINDOWS
// The nearest directory with a CMakeCache.txt at or above the executable's, with a trailing separator.
std::string findBuildDirectory() {
    const char* base = SDL_GetBasePath();
    std::string dir = base ? base : "";
    while (!dir.empty()) {
        if (modifiedTime(dir + "CMakeCache.txt")) return dir;
        dir.pop_back();
        const std::size_t slash = dir.find_last_of("/\\");
        if (slash == std::string::npos) break;
        dir.resize(slash + 1);
    }
    return {};
}
#endif

// The first lines of a compiler's output, on one line.
std::string summary(const std::string& output) {
    std::string out;
    for (const char c : output) {
        if (out.size() >= kMaxLoggedBytes) {
            out += "...";
            break;
        }
        if (c == '\n') {
            if (!out.empty() && !out.ends_with(" | ")) out += " | ";
        } else if (c != '\r') {
            out += c;
        }
    }
    while (out.ends_with(" | ")) out.resize(out.size() - 3);
    return out;
}

std::string commandLine(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& arg : args) out += (out.empty() ? "" : " ") + arg;
    return out;
}

} // namespace

// ------------------------------------------------------------------ pipeline slots

Result<void*> createNative(DeviceImpl& device, const PipelineSlot& slot) {
    const auto current = [&](const ShaderBlob* blob) {
        return device.shaderReload ? device.shaderReload->current(blob) : blob;
    };
    if (slot.compute) {
        ComputePipelineDesc desc = slot.computeDesc;
        desc.shader = current(desc.shader);
        return device.createComputePipeline(desc);
    }
    GraphicsPipelineDesc desc = slot.graphics;
    desc.vertexShader = current(desc.vertexShader);
    desc.fragmentShader = current(desc.fragmentShader);
    return device.createPipeline(desc);
}

Result<void*> addPipeline(DeviceImpl& device, std::unique_ptr<PipelineSlot> slot) {
    const std::lock_guard lock(device.pipelineMutex);
    Result<void*> native = createNative(device, *slot);
    if (!native) return native;
    slot->native = *native;
    if (device.shaderReload) {
        if (slot->compute) {
            device.shaderReload->use(slot->computeDesc.shader);
        } else {
            device.shaderReload->use(slot->graphics.vertexShader);
            device.shaderReload->use(slot->graphics.fragmentShader);
        }
    }
    PipelineSlot* handle = slot.release();
    device.pipelines.insert(handle);
    return static_cast<void*>(handle);
}

void removePipeline(DeviceImpl& device, Kind kind, void* handle) noexcept {
    auto* slot = static_cast<PipelineSlot*>(handle);
    {
        const std::lock_guard lock(device.pipelineMutex);
        device.pipelines.erase(slot);
        device.release(kind, slot->native);
    }
    delete slot;
}

// ------------------------------------------------------------------ ShaderReloader

ShaderReloader::ShaderReloader(DeviceImpl& device, std::string buildDirectory)
    : m_device(device), m_buildDirectory(std::move(buildDirectory)) {}

ShaderReloader::~ShaderReloader() {
    for (Process& p : m_running) {
        SDL_KillProcess(p.process, true);
        SDL_WaitProcess(p.process, true, nullptr);
        SDL_DestroyProcess(p.process);
    }
}

const ShaderBlob* ShaderReloader::current(const ShaderBlob* blob) const {
    const auto it = m_current.find(blob);
    return it == m_current.end() ? blob : &it->second->blob;
}

void ShaderReloader::use(const ShaderBlob* blob) {
    if (blob && blob->manifest && m_known.insert(blob).second) m_pending.push_back(blob);
}

ShaderReloadStatus ShaderReloader::status() const {
    ShaderReloadStatus s = m_status;
    s.enabled = true;
    s.compiling = m_compiling != nullptr ||
                  std::any_of(m_sets.begin(), m_sets.end(), [](const auto& set) { return set.second.dirty; });
    return s;
}

void ShaderReloader::adoptPending() {
    std::vector<const ShaderBlob*> pending;
    {
        const std::lock_guard lock(m_device.pipelineMutex);
        pending.swap(m_pending);
    }
    for (const ShaderBlob* blob : pending) {
        Set* set = loadSet(blob->manifest);
        if (!set) continue;
        const auto entry = std::find_if(set->entries.begin(), set->entries.end(),
                                        [&](const Entry& e) { return e.name == blob->entry; });
        if (entry == set->entries.end()) {
            log::warning("gpu", "shader hot reload: entry '{}' is not in {}", blob->entry, set->manifest);
            continue;
        }
        entry->blobs.push_back(blob);
    }
}

ShaderReloader::Set* ShaderReloader::loadSet(const std::string& manifest) {
    if (const auto it = m_sets.find(manifest); it != m_sets.end()) return &it->second;
    if (m_badManifests.count(manifest)) return nullptr;
    const auto bad = [&](const std::string& why) -> Set* {
        log::warning("gpu", "shader hot reload: {}{}: {}", m_buildDirectory, manifest, why);
        m_badManifests.insert(manifest);
        return nullptr;
    };
    Result<std::vector<std::uint8_t>> bytes = readFile(m_buildDirectory + manifest);
    if (!bytes) return bad(bytes.error());
    Result<json::Value> doc = json::parse(std::string(bytes->begin(), bytes->end()));
    if (!doc) return bad(doc.error());
    Set set;
    set.manifest = manifest;
    set.name = (*doc)["set"].stringOr("");
    set.source = (*doc)["source"].stringOr("");
    set.output = (*doc)["output"].stringOr("");
    if (set.name.empty() || set.source.empty() || set.output.empty()) return bad("no set, source or output");
    const ShaderFormat format = m_device.caps.shaderFormat;
    const char* formatKey = format == ShaderFormat::Metallib ? "metallib" : format == ShaderFormat::Dxil ? "dxil" : "";
    std::vector<std::string> depfiles;
    for (const json::Value& e : (*doc)["entries"].array) {
        Entry entry;
        entry.name = e["entry"].stringOr("");
        const auto addCommands = [&](const json::Value& commands) {
            for (const json::Value& command : commands.array) {
                std::vector<std::string> args;
                for (const json::Value& arg : command.array) args.push_back(arg.stringOr(""));
                if (!args.empty()) entry.commands.push_back(std::move(args));
            }
        };
        addCommands(e["compile"]);
        if (*formatKey) {
            if (!e.has(formatKey))
                return bad(std::format("entry '{}' has no {} commands", entry.name, shaderFormatName(format)));
            addCommands(e[formatKey]);
        }
        if (entry.name.empty() || entry.commands.empty()) return bad("an entry without a name or commands");
        depfiles.push_back(e["depfile"].stringOr(""));
        set.entries.push_back(std::move(entry));
    }
    Set& added = m_sets.emplace(manifest, std::move(set)).first->second;
    watchFiles(added, depfiles);
    log::info("gpu", "shader hot reload: watching '{}' ({} entries, {} files)", added.name, added.entries.size(),
              added.files.size());
    return &added;
}

void ShaderReloader::watchFiles(Set& set, const std::vector<std::string>& depfiles) {
    std::map<std::string, SDL_Time> files;
    std::vector<std::string> names{set.source};
    for (const std::string& depfile : depfiles)
        for (std::string& file : readDepfile(depfile)) names.push_back(std::move(file));
    for (const std::string& name : names) {
        if (files.count(name)) continue;
        const auto known = set.files.find(name);
        if (known != set.files.end())
            files[name] = known->second;
        else if (const std::optional<SDL_Time> time = modifiedTime(name))
            files[name] = *time;
    }
    set.files = std::move(files);
}

bool ShaderReloader::changed(Set& set) {
    bool any = false;
    for (auto& [name, seen] : set.files) {
        const std::optional<SDL_Time> time = modifiedTime(name);
        if (time && *time != seen) {
            seen = *time;
            any = true;
        }
    }
    return any;
}

std::vector<std::string> ShaderReloader::arguments(const Set& set, const std::vector<std::string>& command) const {
    const std::string_view keys[] = {"{source}", "{output}"};
    const std::string* values[] = {&set.source, &set.output};
    std::vector<std::string> args;
    for (std::string arg : command) {
        for (std::size_t k = 0; k < std::size(keys); ++k) {
            for (std::size_t at = arg.find(keys[k]); at != std::string::npos;
                 at = arg.find(keys[k], at + values[k]->size()))
                arg.replace(at, keys[k].size(), *values[k]);
        }
        args.push_back(std::move(arg));
    }
    return args;
}

void ShaderReloader::poll() {
    adoptPending();
    if (m_compiling) {
        advance();
        if (m_compiling) return;
    }
    const Uint64 now = SDL_GetTicksNS();
    if (now - m_lastCheck >= kCheckIntervalNs) {
        m_lastCheck = now;
        for (auto& [manifest, set] : m_sets)
            if (changed(set)) set.dirty = true;
    }
    for (auto& [manifest, set] : m_sets) {
        if (!set.dirty) continue;
        start(set);
        break;
    }
}

void ShaderReloader::start(Set& set) {
    set.dirty = false;
    if (!SDL_CreateDirectory(set.output.c_str())) {
        log::error("gpu", "shader hot reload: cannot create {}: {}", set.output, SDL_GetError());
        return;
    }
    m_compiling = &set;
    m_failed = false;
    m_queue.clear();
    for (std::size_t i = set.entries.size(); i-- > 0;) m_queue.push_back(i);
    log::info("gpu", "shader hot reload: compiling '{}' ({} entries)", set.name, set.entries.size());
    advance();
}

bool ShaderReloader::launch(Process& process) {
    const std::vector<std::string> args =
        arguments(*m_compiling, m_compiling->entries[process.entry].commands[process.command]);
    std::vector<const char*> argv;
    for (const std::string& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    process.process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    process.output.clear();
    if (!process.process) {
        fail(std::format("'{}' entry {}: cannot start {}: {}", m_compiling->name,
                         m_compiling->entries[process.entry].name, args.front(), SDL_GetError()),
             commandLine(args), "");
        return false;
    }
    return true;
}

void ShaderReloader::advance() {
    Set& set = *m_compiling;
    const auto drain = [](Process& p) {
        SDL_IOStream* out = SDL_GetProcessOutput(p.process);
        char buffer[4096];
        while (out) {
            const std::size_t n = SDL_ReadIO(out, buffer, sizeof buffer);
            if (n == 0) break;
            if (p.output.size() < kMaxOutputBytes)
                p.output.append(buffer, std::min(n, kMaxOutputBytes - p.output.size()));
        }
    };
    for (std::size_t i = 0; i < m_running.size();) {
        Process& p = m_running[i];
        drain(p);
        int code = 0;
        if (!SDL_WaitProcess(p.process, false, &code)) {
            ++i;
            continue;
        }
        drain(p);
        SDL_DestroyProcess(p.process);
        p.process = nullptr;
        const Entry& entry = set.entries[p.entry];
        if (code != 0) {
            const std::string command = commandLine(arguments(set, entry.commands[p.command]));
            const std::string output = std::move(p.output);
            m_running.erase(m_running.begin() + std::ptrdiff_t(i));
            fail(std::format("'{}' entry {}: exit code {}", set.name, entry.name, code), command, output);
            return;
        }
        if (++p.command < entry.commands.size()) {
            if (!launch(p)) return;
            ++i;
            continue;
        }
        m_running.erase(m_running.begin() + std::ptrdiff_t(i));
    }
    while (m_running.size() < kMaxProcesses && !m_queue.empty()) {
        Process p;
        p.entry = m_queue.back();
        m_queue.pop_back();
        if (!launch(p)) return;
        m_running.push_back(std::move(p));
    }
    if (m_running.empty() && m_queue.empty()) finish();
}

void ShaderReloader::fail(const std::string& message, const std::string& command, const std::string& output) {
    Set& set = *m_compiling;
    for (Process& p : m_running) {
        if (!p.process) continue;
        SDL_KillProcess(p.process, true);
        SDL_WaitProcess(p.process, true, nullptr);
        SDL_DestroyProcess(p.process);
    }
    m_running.clear();
    m_queue.clear();
    m_compiling = nullptr;
    ++m_status.failures;
    m_status.lastError = message;
    if (!command.empty()) m_status.lastError += "\n" + command;
    if (!output.empty()) m_status.lastError += "\n" + output;
    log::error("gpu", "shader hot reload: {}; the old code of '{}' stays: {}", message, set.name,
               output.empty() ? std::string("no output") : summary(output));
}

void ShaderReloader::finish() {
    Set& set = *m_compiling;
    const ShaderFormat format = m_device.caps.shaderFormat;
    const char* extension = format == ShaderFormat::Spirv      ? "spv"
                            : format == ShaderFormat::Msl      ? "msl"
                            : format == ShaderFormat::Metallib ? "metallib"
                                                               : "dxil";
    std::vector<std::shared_ptr<Reloaded>> reloaded;
    std::vector<std::string> depfiles;
    for (const Entry& entry : set.entries) {
        const std::string base = set.output + "/" + entry.name;
        Result<std::pair<ShaderStage, ShaderResources>> reflect = readReflect(base + ".reflect");
        Result<std::vector<std::uint8_t>> code = reflect ? readFile(base + "." + extension) : Error{reflect.error()};
        if (!code) {
            fail(std::format("'{}' entry {}: {}", set.name, entry.name, code.error()), "", "");
            return;
        }
        auto r = std::make_shared<Reloaded>();
        r->code = std::move(*code);
        const std::size_t size = r->code.size();
        r->code.push_back(0); // MSL is source text: NUL-terminated, the terminator not counted (as embedded)
        const ShaderCode shaderCode{r->code.data(), size};
        r->blob.stage = reflect->first;
        r->blob.resources = reflect->second;
        r->blob.spirv = format == ShaderFormat::Spirv ? shaderCode : ShaderCode{nullptr, 0};
        r->blob.msl = format == ShaderFormat::Msl ? shaderCode : ShaderCode{nullptr, 0};
        r->blob.metallib = format == ShaderFormat::Metallib ? shaderCode : ShaderCode{nullptr, 0};
        r->blob.dxil = format == ShaderFormat::Dxil ? shaderCode : ShaderCode{nullptr, 0};
        reloaded.push_back(std::move(r));
        depfiles.push_back(base + ".d");
    }

    std::uint32_t recreated = 0, kept = 0;
    {
        const std::lock_guard lock(m_device.pipelineMutex);
        std::unordered_set<const ShaderBlob*> originals;
        for (std::size_t i = 0; i < set.entries.size(); ++i) {
            for (const ShaderBlob* original : set.entries[i].blobs) {
                Reloaded& r = *reloaded[i];
                r.blob.entry = original->entry;
                r.blob.manifest = original->manifest;
                m_current[original] = reloaded[i];
                originals.insert(original);
            }
        }
        for (PipelineSlot* slot : m_device.pipelines) {
            const bool uses = slot->compute ? originals.count(slot->computeDesc.shader) != 0
                                            : originals.count(slot->graphics.vertexShader) != 0 ||
                                                  originals.count(slot->graphics.fragmentShader) != 0;
            if (!uses) continue;
            Result<void*> native = createNative(m_device, *slot);
            if (!native) {
                log::error("gpu", "shader hot reload: pipeline '{}' keeps its old code: {}", slot->name,
                           native.error());
                ++kept;
                continue;
            }
            m_device.release(slot->compute ? Kind::ComputePipeline : Kind::GraphicsPipeline, slot->native);
            slot->native = *native;
            ++recreated;
        }
    }
    watchFiles(set, depfiles);
    m_compiling = nullptr;
    ++m_status.reloads;
    m_status.pipelines += recreated;
    log::info("gpu", "shader hot reload: '{}' reloaded, {} pipelines created again{}", set.name, recreated,
              kept ? std::format(", {} kept their old code", kept) : std::string());
}

} // namespace detail

// ------------------------------------------------------------------ Device

Result<void> Device::enableShaderHotReload(std::string_view buildDirectory) {
#if QUENCH_PLATFORM_MACOS || QUENCH_PLATFORM_LINUX || QUENCH_PLATFORM_WINDOWS
    const std::lock_guard lock(m_impl->pipelineMutex);
    if (m_impl->shaderReload) return {};
    std::string dir = buildDirectory.empty() ? detail::findBuildDirectory() : std::string(buildDirectory);
    if (dir.empty()) return Error{"no build directory (CMakeCache.txt) at or above the executable's"};
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    m_impl->shaderReload = std::make_unique<detail::ShaderReloader>(*m_impl, dir);
    for (const detail::PipelineSlot* slot : m_impl->pipelines) {
        if (slot->compute) {
            m_impl->shaderReload->use(slot->computeDesc.shader);
        } else {
            m_impl->shaderReload->use(slot->graphics.vertexShader);
            m_impl->shaderReload->use(slot->graphics.fragmentShader);
        }
    }
    log::info("gpu", "shader hot reload: on, manifests in {}", dir);
    return {};
#else
    (void)buildDirectory;
    return Error{"shader hot reload runs on desktops only"};
#endif
}

void Device::pollShaderHotReload() {
    if (m_impl->shaderReload) m_impl->shaderReload->poll();
}

ShaderReloadStatus Device::shaderReloadStatus() const {
    return m_impl->shaderReload ? m_impl->shaderReload->status() : ShaderReloadStatus{};
}

} // namespace quench::gpu
