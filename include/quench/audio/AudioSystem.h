#pragma once

// The audio mixer: buses, sounds in memory (one-shots and loops), streamed music with crossfades, sources the game
// computes, and 3D sound (attenuation, panning, Doppler) with a voice limit and priorities. It does not open a device:
// the device (audio::Device, quench_audio_device) or an offline loop (mixOffline: tests, --audio-capture) pulls the
// mix through render().
//
// Threads. Every function except render() is for the game thread (the thread that created the AudioSystem); render()
// runs on the audio thread (the device callback) or on the game thread for offline mixing, never on both at once. The
// game thread sends plain commands to the audio thread through a lock-free queue in update(); nothing is allocated,
// freed, locked or logged on the audio thread. Data a playing sound uses (decoded sounds, stream buffers, generators)
// is kept alive on the game side until the audio thread reports the sound's end. Files are decoded on the job system
// (asset loads, stream refills), which also works without worker threads (the jobs then run in update()).
//
// Mix format: 32-bit float stereo at settings.sampleRate (48 kHz by default). Sounds keep their own rate and are
// resampled per voice (linear interpolation). Volume, pan and pitch changes are smoothed (about 10 ms), stops and
// steals fade out over at least 3 ms, so no parameter change clicks.

#include <quench/assets/AssetManager.h>
#include <quench/audio/Sound.h>
#include <quench/audio/Spatial.h>
#include <quench/core/Jobs.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quench::audio {

inline constexpr std::uint32_t kMixChannels = 2;
inline constexpr std::uint32_t kMaxBuses = 32;

// A mixing bus: its sounds and child buses are summed, scaled by its volume, into its parent. Index 0 is the master
// bus, whose output is the mix.
struct BusId {
    std::uint8_t index = 0;

    friend bool operator==(BusId a, BusId b) = default;
};

struct BusDesc {
    std::string name{};
    std::string parent = "master"; // declared earlier in the list, or "master"
    float volume = 1.0f;
};

struct AudioSettings {
    std::uint32_t sampleRate = 48000;
    std::uint32_t maxVoices = 32;  // sounds mixed at once; more steal by priority or are rejected
    std::uint32_t maxSounds = 256; // handles alive at once (pending, playing, paused)
    // Under the master bus, which always exists. Games name their own (for example "voice", "ambience").
    std::vector<BusDesc> buses = {{"music"}, {"effects"}, {"ui"}};
    double maxStartDelay = 0.25;      // seconds a one-shot may wait for its sound to load before it is dropped
    double streamBufferSeconds = 1.0; // decoded audio kept ahead per stream
    float speedOfSound = 343.0f;      // m/s
    float dopplerScale = 1.0f;        // 0: no Doppler for any sound
    assets::AssetMemory soundBudget = assets::AssetManager::kUnlimited; // unreferenced decoded sounds kept cached
};

// A playing (or pending) sound. Plain handle: stale handles (the sound ended) are ignored by every call.
struct SoundHandle {
    std::uint32_t slot = ~0u;
    std::uint32_t generation = 0;

    bool valid() const { return slot != ~0u; }
    friend bool operator==(SoundHandle a, SoundHandle b) = default;
};

enum class SoundState : std::uint8_t {
    Pending, // waiting for its sound to load or its stream's first data
    Playing, // sent to the mixer (also while paused)
    Ended,   // or never existed
};

enum class EndReason : std::uint8_t {
    Finished, // reached its end
    Stopped,  // stop(), after its fade
    Stolen,   // a sound of at least its priority needed the voice
    Rejected, // every voice was busy with sounds of a higher priority
    Dropped,  // a one-shot whose sound was still loading after maxStartDelay
    Failed,   // the sound failed to load, or the stream to open or decode
};

const char* endReasonName(EndReason reason);

struct PlayParams {
    BusId bus{};           // the master bus by default; see AudioSystem::bus()
    float volume = 1.0f;   // linear gain
    float pitch = 1.0f;    // playback rate: 2 is an octave up (0.0625 .. 16 with Doppler)
    float pan = 0.0f;      // sounds without `spatial`: -1 left .. 1 right
    bool loop = false;     // sounds and streams; ignored by generators
    bool paused = false;   // starts paused (resume with setPaused)
    float priority = 0.0f; // higher keeps its voice; equal or lower may be stolen by a new sound
    float fadeIn = 0.0f;   // seconds, equal-power curve
    bool spatial = false;  // 3D: placed by `emitter` around the listener (stereo sources are mixed down to mono)
    Emitter emitter{};
};

// A source the game computes, pulled on the audio thread: generate() follows the audio thread's rules (no allocation,
// locks, logging, file access or waiting) and must be cheap. It is called only while the sound plays.
class Generator {
public:
    explicit Generator(Format format) : m_format(format) {}
    virtual ~Generator() = default;
    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;

    const Format& format() const { return m_format; }
    // Writes up to `frames` interleaved frames; fewer than asked ends the sound after them.
    virtual std::uint32_t generate(float* out, std::uint32_t frames) = 0;

private:
    Format m_format;
};

// A generator the game feeds from its thread: a single-producer single-consumer ring of PCM (a synthesizer run on
// jobs, decoded network audio). When it runs empty the sound plays silence and counts an underrun.
class PcmStream final : public Generator {
public:
    PcmStream(Format format, double capacitySeconds = 0.5);
    ~PcmStream() override;

    // Game thread: appends interleaved frames; returns the frames that fit.
    std::uint32_t push(std::span<const float> samples);
    // Game thread: frames that fit now.
    std::uint32_t writable() const;
    // Frames queued and not yet played.
    std::uint32_t queued() const;
    // Game thread: the sound ends once the queued frames are played.
    void finish();
    std::uint64_t underruns() const { return m_underruns.load(std::memory_order_relaxed); }

    std::uint32_t generate(float* out, std::uint32_t frames) override;

private:
    std::vector<float> m_ring;
    std::uint32_t m_capacity = 0;                     // frames
    std::atomic<std::uint64_t> m_write{0}, m_read{0}; // frame counters
    std::atomic<bool> m_finished{false};
    std::atomic<std::uint64_t> m_underruns{0};
};

struct MusicParams {
    float crossfade = 1.0f; // seconds: the playing track fades out while the new one fades in
    float volume = 1.0f;
    bool loop = true;
    std::optional<BusId> bus{}; // default: the bus named "music", else the master bus
};

struct BusInfo {
    std::string name;
    BusId parent{};
    float volume = 1.0f;
    bool muted = false;
    bool paused = false;
    float peak[2] = {0.0f, 0.0f}; // output level after the bus volume, with a falling peak hold
};

struct AudioStats {
    std::uint32_t voices = 0; // mixed by the last render() (fading out included)
    std::uint32_t maxVoices = 0;
    std::uint32_t sounds = 0;  // handles alive
    std::uint32_t pending = 0; // waiting for a sound to load or a stream to start
    std::uint32_t streams = 0; // streams open
    std::uint64_t played = 0;  // sounds sent to the mixer
    std::uint64_t stolen = 0;
    std::uint64_t rejected = 0;
    std::uint64_t dropped = 0;
    std::uint64_t failed = 0;
    std::uint64_t refused = 0;   // play() calls with every handle in use
    std::uint64_t underruns = 0; // stream buffers that ran empty during a render (PcmStream counts its own)
    std::uint64_t clipped = 0;   // output samples beyond [-1, 1] (clamped)
    std::uint64_t framesMixed = 0;
    double mixMs = 0.0; // average time of one render() call, over the last half second of audio
    double load = 0.0;  // mixing time per audio time, same window
};

class AudioSystem {
public:
    // `assets` may be null: then load() is unavailable (play SoundData, streams and generators still work).
    AudioSystem(JobSystem& jobs, assets::AssetManager* assets, const AudioSettings& settings = {});
    // Waits for its stream jobs and unregisters the "sound" asset type; the audio thread must have stopped calling
    // render() (close the device first), and every AssetRef<SoundAsset> must be gone.
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    const AudioSettings& settings() const { return m_settings; }
    std::uint32_t sampleRate() const { return m_settings.sampleRate; }

    // ---- sounds
    // A sound file through the asset manager (cached, reference-counted, decoded on a job).
    assets::AssetRef<SoundAsset> load(std::string_view path, float priority = 0.0f);
    // Plays a loaded or loading sound (a loading one starts when it is ready).
    SoundHandle play(const assets::AssetRef<SoundAsset>& sound, const PlayParams& params = {});
    // Loads (or finds) the sound and plays it.
    SoundHandle play(std::string_view path, const PlayParams& params = {});
    SoundHandle play(std::shared_ptr<const SoundData> data, const PlayParams& params = {});
    // Streams a WAV, FLAC, MP3 or Ogg Vorbis file through quench::fs, decoding it on jobs a little ahead of the mix
    // (a loop starts over where the decoder runs out: no length needed, which Vorbis streams do not report). A file
    // that cannot be opened or decoded is logged once and ends the sound.
    SoundHandle playStream(std::string_view path, const PlayParams& params = {});
    SoundHandle playGenerator(std::shared_ptr<Generator> generator, const PlayParams& params = {});

    // Fades out over `fade` seconds (at least 3 ms), then ends.
    void stop(SoundHandle sound, float fade = 0.0f);
    void stopAll(float fade = 0.0f);
    void setVolume(SoundHandle sound, float volume);
    void setPitch(SoundHandle sound, float pitch);
    void setPan(SoundHandle sound, float pan);
    void setPaused(SoundHandle sound, bool paused);
    void setEmitter(SoundHandle sound, const Emitter& emitter);
    void setPosition(SoundHandle sound, glm::vec3 position, glm::vec3 velocity = glm::vec3(0.0f));
    SoundState state(SoundHandle sound) const;
    bool alive(SoundHandle sound) const { return state(sound) != SoundState::Ended; }
    // Called from update() for every sound that ends (handle, reason).
    void setEndCallback(std::function<void(SoundHandle, EndReason)> callback);

    // ---- music: one streamed track at a time, crossfaded
    SoundHandle playMusic(std::string_view path, const MusicParams& params = {});
    void stopMusic(float fade = 1.0f);
    SoundHandle music() const { return m_music; }

    // ---- buses
    std::optional<BusId> findBus(std::string_view name) const;
    // The bus of that name; the master bus (and a logged warning) if there is none.
    BusId bus(std::string_view name) const;
    std::uint32_t busCount() const;
    const BusInfo& busInfo(BusId bus) const;
    void setBusVolume(BusId bus, float volume);
    void setBusMuted(BusId bus, bool muted);
    // Freezes every sound on the bus and its children (a pause menu pauses "effects" while "ui" keeps playing).
    void setBusPaused(BusId bus, bool paused);

    // ---- listener
    void setListener(const Listener& listener);
    const Listener& listener() const { return m_listener; }

    // ---- frame
    // Once per frame on the game thread: collects ended sounds, starts the pending ones whose data is ready, sends the
    // frame's changes to the audio thread and refills streams.
    void update();

    // Audio thread (or offline): mixes `frames` interleaved stereo frames into `out`. Real-time safe.
    void render(float* out, std::uint32_t frames);
    // Offline mixing on the game thread (tests, --no-audio, --audio-capture): mixes `seconds` of audio (whole frames,
    // the remainder carried to the next call), appended to `out` if given. Streams are decoded on this thread as the
    // mix needs them and pending streams start in the next update() without waiting for a job, so the result does
    // not depend on thread timing once the sounds are loaded. Returns the frames mixed.
    std::uint32_t mixOffline(double seconds, std::vector<float>* out = nullptr);
    // Offline mode: update() opens streams on its own thread instead of a job. Set before the first play.
    void setOffline(bool offline) { m_offline = offline; }
    bool offline() const { return m_offline; }

    // Keeps the last `seconds` of the mix on the game thread (0 turns it off); filled by update() from the audio
    // thread, a copy of the mix in every update(). May be changed at any time on the game thread: the history
    // starts empty again.
    void setMonitor(double seconds);
    // The newest `seconds` of the monitored mix (interleaved stereo), fewer if not that much was monitored yet.
    std::vector<float> monitored(double seconds) const;

    AudioStats stats() const;

private:
    struct Impl;
    struct Slot;

    SoundHandle allocate(const PlayParams& params);
    Slot* find(SoundHandle sound);
    const Slot* find(SoundHandle sound) const;
    // find(), and queues the sound's parameters for the audio thread.
    Slot* edit(SoundHandle sound);
    void endSlot(std::uint32_t index, EndReason reason);
    void startPending();
    void refillStreams();
    void sendBus(std::uint32_t index);

    JobSystem& m_jobs;
    assets::AssetManager* m_assets;
    AudioSettings m_settings;
    Listener m_listener;
    SoundHandle m_music;
    bool m_offline = false;
    std::unique_ptr<Impl> m_impl;
};

} // namespace quench::audio
