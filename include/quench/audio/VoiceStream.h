#pragma once

// Streamed playback of a voice that arrives in pieces over the network (docs/audio.md, "Streamed playback"): a
// generator for the mixer (AudioSystem::playGenerator, on the bus the game names) that plays mono PCM as it is
// pushed, with a small jitter buffer. Made for assistant::LiveSession's audio, usable for any streamed speech.
//
//   - It starts (and starts again after running dry) once prebufferSeconds are queued, or once what is queued has
//     waited that long (the last short piece of a sentence is not held back).
//   - Running dry while playing (an underrun) fades the last sample to zero over fadeSeconds instead of stepping to
//     silence, and a start fades in over the same time: no clicks either way.
//   - flush() drops everything queued at once (an interruption: the player talked over the station), fading out
//     what is playing over fadeSeconds; what is pushed after the flush plays normally.
//   - level(), audible(), active() and playedSeconds() are for a UI, a gate on the microphone and captions.
//
// The sound never ends by itself (generate() always fills its frames); stop it through the AudioSystem. Threads:
// push(), flush() and the queries are for the game thread; generate() runs on the audio thread (lock-free, no
// allocation).

#include <quench/audio/AudioSystem.h>

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace quench::audio {

struct VoiceStreamDesc {
    std::uint32_t sampleRate = 24000; // of the pushed samples (the mixer resamples to its rate)
    double prebufferSeconds = 0.08;   // the jitter buffer: queued before playing starts
    double fadeSeconds = 0.004;       // the fades at underruns, starts and flushes
    // Room for audio pushed ahead of playback: a server may send speech faster than real time.
    double capacitySeconds = 60.0;
    float audibleLevel = 0.003f; // level() above this counts as audible (about -50 dBFS)
};

class VoiceStream final : public Generator {
public:
    explicit VoiceStream(const VoiceStreamDesc& desc = {});
    ~VoiceStream() override;

    // Game thread: appends mono samples (-1 .. 1); returns how many fit (the rest is dropped and counted).
    std::uint32_t push(std::span<const float> samples);
    // Game thread: drops what is queued, fading out what plays.
    void flush();

    // Seconds queued and not played yet.
    double queuedSeconds() const;
    // Audio is queued or playing (the game's "the station speaks"; a gate for a microphone on loudspeakers).
    bool active() const;
    // The output is above VoiceStreamDesc::audibleLevel now.
    bool audible() const;
    // A smoothed peak of what was played, 0 .. 1 (falls by half in about 0.1 s).
    float level() const;
    // Samples of pushed audio played so far (not counting the silence while waiting), and as seconds.
    std::uint64_t playedFrames() const { return m_played.load(std::memory_order_relaxed); }
    double playedSeconds() const;
    // Times it ran dry while playing (the end of each utterance counts once).
    std::uint64_t underruns() const { return m_underruns.load(std::memory_order_relaxed); }
    // Samples push() could not keep (the queue was full).
    std::uint64_t droppedFrames() const { return m_droppedPush; }
    std::uint64_t flushes() const { return m_flushes; }
    const VoiceStreamDesc& desc() const { return m_desc; }

    std::uint32_t generate(float* out, std::uint32_t frames) override;

private:
    enum class Mode : std::uint8_t { Waiting, Playing };

    std::uint32_t take(float* out, std::uint32_t frames, std::uint64_t limit);

    VoiceStreamDesc m_desc;
    std::vector<float> m_ring;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_prebuffer = 0;
    std::uint32_t m_fade = 1;
    std::atomic<std::uint64_t> m_write{0}, m_read{0};
    std::atomic<std::uint64_t> m_flushTo{0}; // the write position at the last flush()
    std::atomic<std::uint64_t> m_played{0};
    std::atomic<std::uint64_t> m_underruns{0};
    std::atomic<std::uint32_t> m_levelBits{0}; // the float level
    std::atomic<bool> m_playing{false};
    std::uint64_t m_droppedPush = 0; // game thread
    std::uint64_t m_flushes = 0;     // game thread
    // Audio thread.
    Mode m_mode = Mode::Waiting;
    std::uint32_t m_waited = 0; // frames waited with something queued
    std::uint32_t m_rampIn = 0; // frames of the fade-in still to go
    float m_last = 0.0f;        // the last sample played
    float m_level = 0.0f;
};

} // namespace quench::audio
