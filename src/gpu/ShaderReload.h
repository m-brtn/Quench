#pragma once

// Shader hot reload (Device::enableShaderHotReload): watches the shader sets of live pipelines, compiles a set whose
// files changed again with the commands of its manifest (cmake/QuenchShaders.cmake) and creates its pipelines again.
// Pipelines live in PipelineSlots (src/gpu/Backend.h) with or without it.

#include "gpu/Backend.h"

#include <quench/core/Result.h>

#include <SDL3/SDL_process.h>
#include <SDL3/SDL_stdinc.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quench::gpu::detail {

// The backend pipeline of a slot from its description, with the reloaded shaders where there are some
// (pipelineMutex held).
Result<void*> createNative(DeviceImpl& device, const PipelineSlot& slot);
// Creates the slot's backend pipeline and registers the slot, which is the resource's native handle.
Result<void*> addPipeline(DeviceImpl& device, std::unique_ptr<PipelineSlot> slot);
// Releases the backend pipeline and the slot (Kind::GraphicsPipeline, Kind::ComputePipeline).
void removePipeline(DeviceImpl& device, Kind kind, void* handle) noexcept;

class ShaderReloader {
public:
    ShaderReloader(DeviceImpl& device, std::string buildDirectory);
    ~ShaderReloader();
    ShaderReloader(const ShaderReloader&) = delete;
    ShaderReloader& operator=(const ShaderReloader&) = delete;

    // The blob to create a pipeline from: the reloaded code of `blob`, or `blob` (pipelineMutex held).
    const ShaderBlob* current(const ShaderBlob* blob) const;
    // A pipeline uses `blob`: its set is watched from the next poll (pipelineMutex held).
    void use(const ShaderBlob* blob);
    void poll();
    ShaderReloadStatus status() const;

private:
    struct Entry {
        std::string name;
        std::vector<std::vector<std::string>> commands; // for the device's shader format, in order
        std::vector<const ShaderBlob*> blobs;           // the embedded blobs of this entry that pipelines use
    };
    struct Set {
        std::string manifest; // relative to the build directory, as in the blobs
        std::string name;
        std::string source;
        std::string output;
        std::vector<Entry> entries;
        std::map<std::string, SDL_Time> files; // the source and its includes, with the time last seen
        bool dirty = false;
    };
    struct Process {
        SDL_Process* process = nullptr;
        std::size_t entry = 0;
        std::size_t command = 0;
        std::string output;
    };
    struct Reloaded {
        ShaderBlob blob{};
        std::vector<std::uint8_t> code;
    };

    void adoptPending();
    Set* loadSet(const std::string& manifest);
    void watchFiles(Set& set, const std::vector<std::string>& depfiles);
    bool changed(Set& set);
    void start(Set& set);
    void advance();
    bool launch(Process& process);
    void fail(const std::string& message, const std::string& command, const std::string& output);
    void finish();
    std::vector<std::string> arguments(const Set& set, const std::vector<std::string>& command) const;

    DeviceImpl& m_device;
    std::string m_buildDirectory; // ends with a separator
    // Guarded by DeviceImpl::pipelineMutex.
    std::vector<const ShaderBlob*> m_pending;
    std::unordered_set<const ShaderBlob*> m_known;
    std::unordered_map<const ShaderBlob*, std::shared_ptr<const Reloaded>> m_current;
    // The poll's (the frame thread's).
    std::map<std::string, Set> m_sets;
    std::unordered_set<std::string> m_badManifests;
    Uint64 m_lastCheck = 0;
    Set* m_compiling = nullptr;
    std::vector<std::size_t> m_queue;
    std::vector<Process> m_running;
    bool m_failed = false;
    ShaderReloadStatus m_status;
};

} // namespace quench::gpu::detail
