#pragma once

// The audio thread's side of the AudioSystem: voices, buses and the mix. Everything is allocated in the constructor;
// render() only reads commands, mixes and writes events and statistics.

#include "Hrtf.h"
#include "Queue.h"

#include <quench/audio/AudioSystem.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace quench::audio::detail {

enum class SourceKind : std::uint8_t { Sound, Stream, Generator };

// A stream's data as the audio thread sees it: the ring the decode jobs fill, and whether they reached the end.
struct StreamFeed {
    explicit StreamFeed(std::uint32_t channels, std::uint32_t capacityFrames) : ring(channels, capacityFrames) {}
    SampleRing ring;
    std::atomic<bool> ended{false}; // no more frames will be committed
};

struct VoiceParams {
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.0f;
    float priority = 0.0f;
    std::uint8_t bus = 0;
    bool loop = false;
    bool paused = false;
    bool spatial = false;
    Emitter emitter;
    std::uint8_t sendBus = 0;
    float sendLevel = 0.0f;
};

struct PlayCommand {
    std::uint32_t slot = 0, generation = 0;
    SourceKind kind = SourceKind::Sound;
    const SoundData* sound = nullptr;
    StreamFeed* stream = nullptr;
    Generator* generator = nullptr;
    Format format;
    VoiceParams params;
    float fadeIn = 0.0f;
};
struct StopCommand {
    std::uint32_t slot = 0, generation = 0;
    float fade = 0.0f;
};
struct ParamsCommand {
    std::uint32_t slot = 0, generation = 0;
    VoiceParams params;
};
struct BusCommand {
    std::uint8_t index = 0;
    float volume = 1.0f;
    bool muted = false;
    bool paused = false;
};
struct ListenerCommand {
    Listener listener;
    float speedOfSound = 343.0f;
    float dopplerScale = 1.0f;
};
struct ReverbCommand {
    std::uint8_t index = 0; // the bus
    ReverbDesc reverb;
};
struct HrtfCommand {
    bool enabled = false;
};
using Command =
    std::variant<PlayCommand, StopCommand, ParamsCommand, BusCommand, ListenerCommand, ReverbCommand, HrtfCommand>;

struct EndEvent {
    std::uint32_t slot = 0, generation = 0;
    EndReason reason = EndReason::Finished;
};

struct MixerConfig {
    std::uint32_t sampleRate = 48000;
    std::uint32_t maxVoices = 32;
    std::uint32_t maxSounds = 256;
    std::vector<std::uint8_t> busParents; // per bus; bus 0 is the master (its entry is ignored)
    std::vector<float> busVolumes;
    std::vector<std::optional<ReverbDesc>> busReverbs; // per bus
    std::uint32_t monitorFrames = 0;                   // capacity of the output tap (0: none)
    std::uint32_t hrtfVoices = 0;                      // HRTF filters (0: no HRTF table)
    bool hrtf = false;
};

// verblib's sample rates (its buffers are sized for up to 4 x 44100 Hz).
inline constexpr std::uint32_t kMinReverbRate = 22050, kMaxReverbRate = 176400;

inline constexpr std::uint32_t kBlockFrames = 64;
inline constexpr float kMinStopFade = 0.003f; // seconds
inline constexpr float kSmoothingSeconds = 0.010f;
inline constexpr float kPauseRampSeconds = 0.005f;
inline constexpr double kMinStep = 1.0 / 16.0, kMaxStep = 16.0; // source frames per output frame

class Mixer {
public:
    explicit Mixer(const MixerConfig& config);
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    SpscQueue<Command>& commands() { return m_commands; }
    SpscQueue<EndEvent>& events() { return m_events; }
    SampleRing* monitor() { return m_monitor.get(); }
    // Set by the game thread before the audio thread starts or while it is stopped.
    void setMonitorEnabled(bool enabled) { m_monitorEnabled.store(enabled, std::memory_order_relaxed); }

    void render(float* out, std::uint32_t frames);

    // Statistics, written by render() and read by the game thread.
    struct Stats {
        std::atomic<std::uint32_t> voices{0}, hrtfVoices{0};
        std::atomic<std::uint64_t> stolen{0}, rejected{0}, underruns{0}, clipped{0};
        std::atomic<std::uint64_t> frames{0}, calls{0}, busyNs{0};
        std::array<std::array<std::atomic<float>, 2>, kMaxBuses> meters{};
    };
    const Stats& stats() const { return m_stats; }

private:
    struct Voice {
        std::uint32_t slot = 0, generation = 0;
        SourceKind kind = SourceKind::Sound;
        const SoundData* sound = nullptr;
        StreamFeed* stream = nullptr;
        Generator* generator = nullptr;
        std::uint32_t channels = 1;
        double sourceRate = 48000.0;
        VoiceParams params;
        std::uint64_t position = 0; // next source frame to read (sounds)
        std::uint64_t order = 0;    // start order, for stealing the oldest
        std::uint32_t frac = 0;     // position between x0 and x1, in 1/2^32
        float x0[2] = {}, x1[2] = {};
        bool primed = false;
        bool sourceDone = false;
        double step = 1.0; // smoothed source frames per output frame
        bool stepSet = false;
        float gain[2] = {};
        bool gainSet = false;
        float fade = 1.0f;     // 0..1 on an equal-power curve
        float fadeRate = 0.0f; // per frame; negative while fading out
        bool stopping = false;
        EndReason stopReason = EndReason::Stopped;
        float pauseGain = 1.0f;
        float audibility = 0.0f;
        float send = 0.0f;      // smoothed send level
        std::int32_t hrtf = -1; // index into m_hrtfFilters, or -1: panned only
        float hrtfMix = 0.0f;   // smoothed share of the HRTF in its sound
        float hrtfGain = 0.0f;  // smoothed volume of the HRTF's input
    };
    struct Reverb; // verblib's state, about 400 KB

    void apply(const PlayCommand& c);
    void apply(const StopCommand& c);
    void apply(const ParamsCommand& c);
    void apply(const BusCommand& c);
    void apply(const ListenerCommand& c);
    void apply(const ReverbCommand& c);
    void apply(const HrtfCommand& c);
    void processReverb(Reverb& reverb, float* samples, std::uint32_t frames);
    Voice* voiceOf(std::uint32_t slot, std::uint32_t generation);
    void beginStop(Voice& voice, float fade, EndReason reason);
    void release(std::uint32_t voiceIndex, EndReason reason);
    void mixBlock(float* out, std::uint32_t frames);
    // Returns false when the voice ended.
    bool mixVoice(Voice& voice, std::uint32_t frames, const std::uint8_t* busPaused);
    std::uint32_t pull(Voice& voice, float* out, std::uint32_t frames);

    MixerConfig m_config;
    SpscQueue<Command> m_commands;
    SpscQueue<EndEvent> m_events;
    std::unique_ptr<SampleRing> m_monitor;
    std::atomic<bool> m_monitorEnabled{false};

    std::vector<Voice> m_voices;           // 2 x maxVoices: those fading out after a steal or stop do not count
    std::vector<std::uint32_t> m_active;   // indices into m_voices, in start order
    std::vector<std::uint32_t> m_free;     // free indices into m_voices
    std::vector<std::int32_t> m_slotVoice; // slot -> index into m_voices, or -1
    std::uint64_t m_order = 0;

    struct Bus {
        std::uint8_t parent = 0;
        float volume = 1.0f;
        bool muted = false;
        bool paused = false;
        float gain = 1.0f; // smoothed
        float meter[2] = {};
        Reverb* reverb = nullptr; // in m_reverbs
    };
    std::vector<Bus> m_buses;
    std::vector<std::unique_ptr<Reverb>> m_reverbs;
    std::vector<float> m_busBuffers; // per bus kBlockFrames stereo frames
    std::vector<float> m_scratch;    // source frames of one voice for one block
    std::unique_ptr<Hrtf> m_hrtf;    // null without HRTF voices
    std::vector<HrtfFilter> m_hrtfFilters;
    std::vector<std::uint32_t> m_hrtfFree; // indices into m_hrtfFilters
    std::vector<float> m_hrtfPanned;       // one voice's block before the HRTF splits it
    bool m_hrtfOn = false;
    Listener m_listener;
    float m_speedOfSound = 343.0f;
    float m_dopplerScale = 1.0f;
    Stats m_stats;
};

} // namespace quench::audio::detail
