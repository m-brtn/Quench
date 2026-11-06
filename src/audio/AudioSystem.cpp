#include <quench/audio/AudioSystem.h>

#include "Decoder.h"
#include "Mixer.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace quench::audio {
namespace {

constexpr std::uint32_t kOfflineChunk = 1024; // frames per render() in mixOffline
constexpr double kStatsWindow = 0.5;          // seconds of audio per mixMs / load measurement

// A streamed file: opened and refilled on jobs (one at a time per stream), read by the audio thread through `feed`.
struct Stream {
    std::string path;
    bool loop = false;
    double bufferSeconds = 1.0;
    std::unique_ptr<detail::Decoder> decoder; // used by one job at a time
    std::unique_ptr<detail::StreamFeed> feed; // created when the file is opened
    Job job;                                  // the open or refill job in flight
    std::string error;                        // set by a job; read once the job is done
    std::atomic<bool> opened{false};
    std::atomic<bool> failed{false};
    bool started = false;     // the open was submitted (game thread)
    bool errorLogged = false; // game thread
    bool justLooped = false;  // job side: the last read was right after a seek to the start

    // On a job (or the game thread offline): opens the file and fills the ring.
    void open(std::uint32_t mixRate) {
        Result<std::unique_ptr<fs::Reader>> reader = fs::openAsset(path);
        if (!reader) return fail(reader.error());
        Result<std::unique_ptr<detail::Decoder>> opened = detail::Decoder::fromReader(std::move(reader).value(), path);
        if (!opened) return fail(opened.error());
        decoder = std::move(opened).value();
        const Format format = decoder->format();
        const auto capacity =
            std::max<std::uint32_t>(4096, std::uint32_t(bufferSeconds * std::max(format.sampleRate, mixRate) + 0.5));
        feed = std::make_unique<detail::StreamFeed>(format.channels, capacity);
        fill();
        if (feed->ring.available() == 0 && !failed.load(std::memory_order_relaxed))
            return fail(path + ": no audio frames");
        this->opened.store(true, std::memory_order_release);
    }

    // Decodes until the ring is full or the file ends (and, looping, starts over from the beginning).
    void fill() {
        if (!feed || feed->ended.load(std::memory_order_relaxed)) return;
        detail::SampleRing& ring = feed->ring;
        while (ring.writable() > 0) {
            std::uint32_t granted = 0;
            float* dst = ring.writeSpan(ring.writable(), granted);
            Result<std::uint32_t> got = decoder->read(dst, granted);
            if (!got) {
                error = got.error();
                failed.store(true, std::memory_order_release);
                feed->ended.store(true, std::memory_order_release);
                return;
            }
            if (*got > 0) {
                ring.commit(*got);
                justLooped = false;
                continue;
            }
            if (!loop || justLooped) {
                // The end (a looping file that gives no frames from its start ends too).
                feed->ended.store(true, std::memory_order_release);
                return;
            }
            if (Result<void> seek = decoder->seekToStart(); !seek) {
                error = seek.error();
                failed.store(true, std::memory_order_release);
                feed->ended.store(true, std::memory_order_release);
                return;
            }
            justLooped = true;
        }
    }

    void fail(const std::string& message) {
        error = message;
        failed.store(true, std::memory_order_release);
    }

    bool needsRefill() const {
        return opened.load(std::memory_order_acquire) && !feed->ended.load(std::memory_order_relaxed) &&
               feed->ring.writable() >= feed->ring.capacity() / 2;
    }
};

detail::VoiceParams voiceParams(const PlayParams& p, std::uint32_t busCount) {
    detail::VoiceParams v;
    v.volume = std::max(p.volume, 0.0f);
    v.pitch = std::clamp(p.pitch, 1.0f / 16.0f, 16.0f);
    v.pan = std::clamp(p.pan, -1.0f, 1.0f);
    v.priority = p.priority;
    v.bus = p.bus.index < busCount ? p.bus.index : 0;
    v.loop = p.loop;
    v.paused = p.paused;
    v.spatial = p.spatial;
    v.emitter = p.emitter;
    return v;
}

} // namespace

const char* endReasonName(EndReason reason) {
    switch (reason) {
    case EndReason::Finished: return "finished";
    case EndReason::Stopped: return "stopped";
    case EndReason::Stolen: return "stolen";
    case EndReason::Rejected: return "rejected";
    case EndReason::Dropped: return "dropped";
    case EndReason::Failed: return "failed";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------- PcmStream

PcmStream::PcmStream(Format format, double capacitySeconds) : Generator(format) {
    QUENCH_ASSERT(format.channels >= 1 && format.channels <= 2, "PcmStream: 1 or 2 channels");
    m_capacity = std::max<std::uint32_t>(64, std::uint32_t(capacitySeconds * format.sampleRate));
    m_ring.resize(std::size_t(m_capacity) * format.channels);
}

PcmStream::~PcmStream() = default;

std::uint32_t PcmStream::writable() const {
    return m_capacity - std::uint32_t(m_write.load(std::memory_order_relaxed) - m_read.load(std::memory_order_acquire));
}

std::uint32_t PcmStream::queued() const {
    return std::uint32_t(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
}

std::uint32_t PcmStream::push(std::span<const float> samples) {
    const std::uint32_t channels = format().channels;
    const std::uint64_t write = m_write.load(std::memory_order_relaxed);
    const auto n = std::min(std::uint32_t(samples.size() / channels), writable());
    for (std::uint32_t done = 0; done < n;) {
        const auto offset = std::uint32_t((write + done) % m_capacity);
        const std::uint32_t piece = std::min(n - done, m_capacity - offset);
        std::memcpy(m_ring.data() + std::size_t(offset) * channels, samples.data() + std::size_t(done) * channels,
                    std::size_t(piece) * channels * sizeof(float));
        done += piece;
    }
    m_write.store(write + n, std::memory_order_release);
    return n;
}

void PcmStream::finish() { m_finished.store(true, std::memory_order_release); }

std::uint32_t PcmStream::generate(float* out, std::uint32_t frames) {
    const std::uint32_t channels = format().channels;
    std::uint32_t done = 0;
    auto take = [&] {
        const std::uint64_t read = m_read.load(std::memory_order_relaxed);
        const auto n = std::min(frames - done, std::uint32_t(m_write.load(std::memory_order_acquire) - read));
        for (std::uint32_t copied = 0; copied < n;) {
            const auto offset = std::uint32_t((read + copied) % m_capacity);
            const std::uint32_t piece = std::min(n - copied, m_capacity - offset);
            std::memcpy(out + std::size_t(done + copied) * channels, m_ring.data() + std::size_t(offset) * channels,
                        std::size_t(piece) * channels * sizeof(float));
            copied += piece;
        }
        m_read.store(read + n, std::memory_order_release);
        done += n;
    };
    take();
    if (done == frames) return frames;
    if (m_finished.load(std::memory_order_acquire)) {
        take(); // frames pushed just before finish()
        return done;
    }
    m_underruns.fetch_add(1, std::memory_order_relaxed);
    std::memset(out + std::size_t(done) * channels, 0, std::size_t(frames - done) * channels * sizeof(float));
    return frames;
}

// ---------------------------------------------------------------------------------------------------- AudioSystem

struct AudioSystem::Slot {
    std::uint32_t generation = 1;
    enum class State : std::uint8_t { Free, Pending, Sent } state = State::Free;
    detail::SourceKind kind = detail::SourceKind::Sound;
    assets::AssetRef<SoundAsset> asset;
    std::shared_ptr<const SoundData> data;
    std::shared_ptr<Stream> stream;
    std::shared_ptr<Generator> generator;
    detail::VoiceParams params;
    float fadeIn = 0.0f;
    double requestedAt = 0.0; // audio time of the play() call
    bool dirty = false;
    SoundHandle crossfadeFrom; // music: fades out when this one starts
    float crossfade = 0.0f;
};

struct AudioSystem::Impl {
    explicit Impl(const detail::MixerConfig& config) : mixer(config) {}

    detail::Mixer mixer;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::vector<std::uint32_t> pending;
    std::vector<std::uint32_t> dirty;
    std::deque<detail::Command> outbox;           // commands that did not fit into the queue yet
    std::vector<std::shared_ptr<Stream>> streams; // open or opening, owned by a slot
    std::vector<std::shared_ptr<Stream>> retired; // their sound ended while a job was running
    std::vector<BusInfo> buses;
    std::vector<bool> busDirty;
    bool listenerDirty = true;
    std::function<void(SoundHandle, EndReason)> onEnd;

    double carry = 0.0; // fractional frames of mixOffline
    std::vector<float> offlineBuffer;

    // The monitored mix: a ring of the newest frames (game thread).
    std::vector<float> history;
    std::size_t historyFrames = 0; // capacity
    std::size_t historyWrite = 0;  // total frames written
    std::vector<float> tapBuffer;

    std::uint64_t played = 0, dropped = 0, failed = 0, refused = 0;
    std::uint64_t lastFrames = 0, lastCalls = 0, lastBusyNs = 0;
    double mixMs = 0.0, load = 0.0;
    std::uint64_t underrunsLogged = 0;
    log::Throttle refuseThrottle{5.0};
    log::Throttle underrunThrottle{5.0};

    double audioTime(std::uint32_t rate) const {
        return double(mixer.stats().frames.load(std::memory_order_relaxed)) / rate;
    }

    void send(const detail::Command& command) { outbox.push_back(command); }

    void flush() {
        while (!outbox.empty() && mixer.commands().push(outbox.front())) outbox.pop_front();
    }

    void drainTap() {
        detail::SampleRing* tap = mixer.monitor();
        if (!tap) return;
        for (;;) {
            const std::uint32_t got = tap->read(tapBuffer.data(), std::uint32_t(tapBuffer.size() / kMixChannels));
            if (got == 0) break;
            if (historyFrames == 0) continue;
            for (std::size_t done = 0; done < got;) {
                const std::size_t at = historyWrite % historyFrames;
                const std::size_t n = std::min(got - done, historyFrames - at);
                std::memcpy(history.data() + at * kMixChannels, tapBuffer.data() + done * kMixChannels,
                            n * kMixChannels * sizeof(float));
                historyWrite += n;
                done += n;
            }
        }
    }
};

AudioSystem::AudioSystem(JobSystem& jobs, assets::AssetManager* assets, const AudioSettings& settings)
    : m_jobs(jobs), m_assets(assets), m_settings(settings) {
    m_settings.sampleRate = std::clamp<std::uint32_t>(m_settings.sampleRate, 8000, 192000);
    m_settings.maxVoices = std::clamp<std::uint32_t>(m_settings.maxVoices, 1, 1024);
    m_settings.maxSounds = std::clamp<std::uint32_t>(m_settings.maxSounds, m_settings.maxVoices, 65536);

    std::vector<BusInfo> buses;
    buses.push_back({.name = "master"});
    for (const BusDesc& desc : m_settings.buses) {
        const auto named = [&](std::string_view name) {
            return std::find_if(buses.begin(), buses.end(), [&](const BusInfo& b) { return b.name == name; });
        };
        if (desc.name.empty() || named(desc.name) != buses.end()) {
            log::warning("audio", "bus '{}' skipped: empty or declared twice", desc.name);
            continue;
        }
        if (buses.size() == kMaxBuses) {
            log::warning("audio", "bus '{}' skipped: at most {} buses", desc.name, kMaxBuses);
            continue;
        }
        auto parent = named(desc.parent);
        if (parent == buses.end()) {
            log::warning("audio", "bus '{}': no parent bus '{}' declared before it, using master", desc.name,
                         desc.parent);
            parent = buses.begin();
        }
        const auto parentIndex = std::uint8_t(parent - buses.begin());
        buses.push_back({.name = desc.name, .parent = BusId{parentIndex}, .volume = std::max(desc.volume, 0.0f)});
    }

    detail::MixerConfig config;
    config.sampleRate = m_settings.sampleRate;
    config.maxVoices = m_settings.maxVoices;
    config.maxSounds = m_settings.maxSounds;
    config.monitorFrames = m_settings.sampleRate / 2;
    for (const BusInfo& bus : buses) {
        config.busParents.push_back(bus.parent.index);
        config.busVolumes.push_back(bus.volume);
    }
    m_impl = std::make_unique<Impl>(config);
    m_impl->buses = std::move(buses);
    m_impl->busDirty.assign(m_impl->buses.size(), false);
    m_impl->slots.resize(m_settings.maxSounds);
    for (std::uint32_t i = m_settings.maxSounds; i-- > 0;) m_impl->freeSlots.push_back(i);
    m_impl->offlineBuffer.resize(std::size_t(kOfflineChunk) * kMixChannels);
    m_impl->tapBuffer.resize(std::size_t(4096) * kMixChannels);

    if (m_assets) {
        m_assets->registerType<SoundAsset>(
            "sound", [](std::string_view) { return std::unique_ptr<SoundAsset>(new SoundAsset()); },
            m_settings.soundBudget);
    }
}

AudioSystem::~AudioSystem() {
    for (const auto& list : {&m_impl->streams, &m_impl->retired})
        for (const std::shared_ptr<Stream>& stream : *list)
            if (stream->job.valid()) m_jobs.wait(stream->job);
    m_impl->slots.clear(); // asset references go before the type
    m_impl->streams.clear();
    m_impl->retired.clear();
    if (m_assets) m_assets->unregisterType<SoundAsset>();
}

// ---- handles

AudioSystem::Slot* AudioSystem::find(SoundHandle sound) {
    if (sound.slot >= m_impl->slots.size()) return nullptr;
    Slot& slot = m_impl->slots[sound.slot];
    return slot.state != Slot::State::Free && slot.generation == sound.generation ? &slot : nullptr;
}

const AudioSystem::Slot* AudioSystem::find(SoundHandle sound) const {
    return const_cast<AudioSystem*>(this)->find(sound);
}

SoundHandle AudioSystem::allocate(const PlayParams& params) {
    Impl& s = *m_impl;
    if (s.freeSlots.empty()) {
        ++s.refused;
        if (s.refuseThrottle.allow())
            log::warning("audio", "play refused: all {} sound handles in use (AudioSettings::maxSounds)",
                         m_settings.maxSounds);
        return {};
    }
    const std::uint32_t index = s.freeSlots.back();
    s.freeSlots.pop_back();
    Slot& slot = s.slots[index];
    slot.state = Slot::State::Pending;
    slot.params = voiceParams(params, std::uint32_t(s.buses.size()));
    slot.fadeIn = std::max(params.fadeIn, 0.0f);
    slot.requestedAt = s.audioTime(m_settings.sampleRate);
    slot.dirty = false;
    slot.crossfadeFrom = {};
    s.pending.push_back(index);
    return {index, slot.generation};
}

void AudioSystem::endSlot(std::uint32_t index, EndReason reason) {
    Impl& s = *m_impl;
    Slot& slot = s.slots[index];
    const SoundHandle handle{index, slot.generation};
    if (slot.stream) {
        std::erase(s.streams, slot.stream);
        if (slot.stream->job.valid() && !slot.stream->job.done()) s.retired.push_back(slot.stream);
    }
    slot.asset = {};
    slot.data.reset();
    slot.stream.reset();
    slot.generator.reset();
    slot.state = Slot::State::Free;
    slot.dirty = false;
    ++slot.generation;
    s.freeSlots.push_back(index);
    if (reason == EndReason::Dropped) ++s.dropped;
    if (reason == EndReason::Failed) ++s.failed;
    if (handle == m_music) m_music = {};
    if (s.onEnd) s.onEnd(handle, reason);
}

// ---- sounds

assets::AssetRef<SoundAsset> AudioSystem::load(std::string_view path, float priority) {
    if (!m_assets) {
        log::error("audio", "load '{}': the AudioSystem has no asset manager", path);
        return {};
    }
    return m_assets->load<SoundAsset>(path, priority);
}

SoundHandle AudioSystem::play(const assets::AssetRef<SoundAsset>& sound, const PlayParams& params) {
    if (!sound) return {};
    const SoundHandle handle = allocate(params);
    if (Slot* slot = find(handle)) {
        slot->kind = detail::SourceKind::Sound;
        slot->asset = sound;
    }
    return handle;
}

SoundHandle AudioSystem::play(std::string_view path, const PlayParams& params) { return play(load(path), params); }

SoundHandle AudioSystem::play(std::shared_ptr<const SoundData> data, const PlayParams& params) {
    if (!data || data->frames() == 0 || data->format.channels < 1 || data->format.channels > 2 ||
        data->format.sampleRate == 0) {
        log::error("audio", "play: empty sound data, or not 1 or 2 channels");
        return {};
    }
    const SoundHandle handle = allocate(params);
    if (Slot* slot = find(handle)) {
        slot->kind = detail::SourceKind::Sound;
        slot->data = std::move(data);
    }
    return handle;
}

SoundHandle AudioSystem::playStream(std::string_view path, const PlayParams& params) {
    const SoundHandle handle = allocate(params);
    if (Slot* slot = find(handle)) {
        slot->kind = detail::SourceKind::Stream;
        slot->stream = std::make_shared<Stream>();
        slot->stream->path = std::string(path);
        slot->stream->loop = params.loop;
        slot->stream->bufferSeconds = std::max(m_settings.streamBufferSeconds, 0.1);
        m_impl->streams.push_back(slot->stream);
    }
    return handle;
}

SoundHandle AudioSystem::playGenerator(std::shared_ptr<Generator> generator, const PlayParams& params) {
    if (!generator || generator->format().channels < 1 || generator->format().channels > 2 ||
        generator->format().sampleRate == 0) {
        log::error("audio", "playGenerator: no generator, or not 1 or 2 channels");
        return {};
    }
    const SoundHandle handle = allocate(params);
    if (Slot* slot = find(handle)) {
        slot->kind = detail::SourceKind::Generator;
        slot->generator = std::move(generator);
    }
    return handle;
}

void AudioSystem::stop(SoundHandle sound, float fade) {
    Slot* slot = find(sound);
    if (!slot) return;
    if (slot->state == Slot::State::Pending) {
        endSlot(sound.slot, EndReason::Stopped);
        return;
    }
    m_impl->send(detail::StopCommand{sound.slot, sound.generation, std::max(fade, 0.0f)});
}

void AudioSystem::stopAll(float fade) {
    for (std::uint32_t i = 0; i < m_impl->slots.size(); ++i)
        if (m_impl->slots[i].state != Slot::State::Free) stop({i, m_impl->slots[i].generation}, fade);
}

AudioSystem::Slot* AudioSystem::edit(SoundHandle sound) {
    Slot* slot = find(sound);
    if (slot && slot->state == Slot::State::Sent && !slot->dirty) {
        slot->dirty = true;
        m_impl->dirty.push_back(sound.slot);
    }
    return slot;
}

void AudioSystem::setVolume(SoundHandle sound, float volume) {
    if (Slot* slot = edit(sound)) slot->params.volume = std::max(volume, 0.0f);
}

void AudioSystem::setPitch(SoundHandle sound, float pitch) {
    if (Slot* slot = edit(sound)) slot->params.pitch = std::clamp(pitch, 1.0f / 16.0f, 16.0f);
}

void AudioSystem::setPan(SoundHandle sound, float pan) {
    if (Slot* slot = edit(sound)) slot->params.pan = std::clamp(pan, -1.0f, 1.0f);
}

void AudioSystem::setPaused(SoundHandle sound, bool paused) {
    if (Slot* slot = edit(sound)) slot->params.paused = paused;
}

void AudioSystem::setEmitter(SoundHandle sound, const Emitter& emitter) {
    if (Slot* slot = edit(sound)) slot->params.emitter = emitter;
}

void AudioSystem::setPosition(SoundHandle sound, glm::vec3 position, glm::vec3 velocity) {
    if (Slot* slot = edit(sound)) {
        slot->params.emitter.position = position;
        slot->params.emitter.velocity = velocity;
    }
}

SoundState AudioSystem::state(SoundHandle sound) const {
    const Slot* slot = find(sound);
    if (!slot) return SoundState::Ended;
    return slot->state == Slot::State::Pending ? SoundState::Pending : SoundState::Playing;
}

void AudioSystem::setEndCallback(std::function<void(SoundHandle, EndReason)> callback) {
    m_impl->onEnd = std::move(callback);
}

// ---- music

SoundHandle AudioSystem::playMusic(std::string_view path, const MusicParams& params) {
    PlayParams play;
    play.bus = params.bus ? *params.bus : findBus("music").value_or(BusId{});
    play.volume = params.volume;
    play.loop = params.loop;
    play.priority = 1e30f;
    SoundHandle previous = m_music;
    if (const Slot* waiting = find(previous); waiting && waiting->state == Slot::State::Pending) {
        // A track that never started: the new one crossfades from what is playing instead.
        const SoundHandle older = waiting->crossfadeFrom;
        endSlot(previous.slot, EndReason::Stopped);
        previous = older;
    }
    play.fadeIn = alive(previous) ? std::max(params.crossfade, 0.0f) : 0.0f;
    const SoundHandle handle = playStream(path, play);
    if (Slot* slot = find(handle)) {
        slot->crossfadeFrom = previous;
        slot->crossfade = params.crossfade;
    }
    m_music = handle;
    return handle;
}

void AudioSystem::stopMusic(float fade) {
    stop(m_music, fade);
    m_music = {};
}

// ---- buses

std::optional<BusId> AudioSystem::findBus(std::string_view name) const {
    for (std::size_t i = 0; i < m_impl->buses.size(); ++i)
        if (m_impl->buses[i].name == name) return BusId{std::uint8_t(i)};
    return std::nullopt;
}

BusId AudioSystem::bus(std::string_view name) const {
    if (std::optional<BusId> id = findBus(name)) return *id;
    log::warning("audio", "no bus '{}': using master", name);
    return {};
}

std::uint32_t AudioSystem::busCount() const { return std::uint32_t(m_impl->buses.size()); }

const BusInfo& AudioSystem::busInfo(BusId bus) const {
    return m_impl->buses[bus.index < m_impl->buses.size() ? bus.index : 0];
}

void AudioSystem::setBusVolume(BusId bus, float volume) {
    if (bus.index >= m_impl->buses.size()) return;
    m_impl->buses[bus.index].volume = std::max(volume, 0.0f);
    m_impl->busDirty[bus.index] = true;
}

void AudioSystem::setBusMuted(BusId bus, bool muted) {
    if (bus.index >= m_impl->buses.size()) return;
    m_impl->buses[bus.index].muted = muted;
    m_impl->busDirty[bus.index] = true;
}

void AudioSystem::setBusPaused(BusId bus, bool paused) {
    if (bus.index >= m_impl->buses.size()) return;
    m_impl->buses[bus.index].paused = paused;
    m_impl->busDirty[bus.index] = true;
}

void AudioSystem::sendBus(std::uint32_t index) {
    const BusInfo& bus = m_impl->buses[index];
    m_impl->send(detail::BusCommand{std::uint8_t(index), bus.volume, bus.muted, bus.paused});
}

void AudioSystem::setListener(const Listener& listener) {
    m_listener = listener;
    m_impl->listenerDirty = true;
}

// ---- frame

void AudioSystem::startPending() {
    Impl& s = *m_impl;
    const double now = s.audioTime(m_settings.sampleRate);
    std::vector<std::uint32_t> waiting;
    std::vector<std::uint32_t> list = std::move(s.pending);
    s.pending.clear();
    for (std::uint32_t index : list) {
        Slot& slot = s.slots[index];
        if (slot.state != Slot::State::Pending) continue;
        detail::PlayCommand play;
        play.slot = index;
        play.generation = slot.generation;
        play.kind = slot.kind;
        play.params = slot.params;
        play.fadeIn = slot.fadeIn;
        bool ready = false;
        switch (slot.kind) {
        case detail::SourceKind::Sound:
            if (!slot.data && slot.asset) {
                if (slot.asset.state() == assets::AssetState::Failed) {
                    endSlot(index, EndReason::Failed); // the asset manager logged why
                    continue;
                }
                if (slot.asset.ready()) slot.data = slot.asset->data();
            }
            if (slot.data) {
                play.sound = slot.data.get();
                play.format = slot.data->format;
                ready = true;
            } else if (!slot.params.loop && now - slot.requestedAt > m_settings.maxStartDelay) {
                log::debug("audio", "'{}' dropped: still loading after {:.2f} s",
                           slot.asset ? slot.asset->path() : std::string(), m_settings.maxStartDelay);
                endSlot(index, EndReason::Dropped);
                continue;
            }
            break;
        case detail::SourceKind::Stream: {
            Stream& stream = *slot.stream;
            if (!stream.started) {
                stream.started = true;
                if (m_offline) {
                    stream.open(m_settings.sampleRate);
                } else {
                    stream.job = m_jobs.run([st = slot.stream, rate = m_settings.sampleRate] { st->open(rate); },
                                            JobPriority::High);
                }
            }
            if (stream.failed.load(std::memory_order_acquire) && (!stream.job.valid() || stream.job.done())) {
                if (!stream.errorLogged) log::error("audio", "stream {}", stream.error);
                stream.errorLogged = true;
                endSlot(index, EndReason::Failed);
                continue;
            }
            if (stream.opened.load(std::memory_order_acquire)) {
                play.stream = stream.feed.get();
                play.format = stream.decoder->format();
                ready = true;
            }
            break;
        }
        case detail::SourceKind::Generator:
            play.generator = slot.generator.get();
            play.format = slot.generator->format();
            ready = true;
            break;
        }
        if (!ready) {
            waiting.push_back(index);
            continue;
        }
        if (Slot* from = find(slot.crossfadeFrom)) {
            if (from->state == Slot::State::Pending)
                endSlot(slot.crossfadeFrom.slot, EndReason::Stopped);
            else
                s.send(detail::StopCommand{slot.crossfadeFrom.slot, slot.crossfadeFrom.generation, slot.crossfade});
        }
        slot.crossfadeFrom = {};
        play.params = slot.params; // after the crossfade bookkeeping: the slot may have been edited meanwhile
        s.send(play);
        slot.state = Slot::State::Sent;
        ++s.played;
    }
    // Plays issued by end callbacks during this pass are already in s.pending.
    s.pending.insert(s.pending.end(), waiting.begin(), waiting.end());
}

void AudioSystem::refillStreams() {
    Impl& s = *m_impl;
    for (const std::shared_ptr<Stream>& stream : s.streams) {
        if (stream->job.valid() && !stream->job.done()) continue;
        if (stream->failed.load(std::memory_order_acquire) && !stream->errorLogged &&
            stream->opened.load(std::memory_order_acquire)) {
            log::error("audio", "stream {}", stream->error);
            stream->errorLogged = true;
        }
        if (!stream->needsRefill()) continue;
        if (m_offline)
            stream->fill();
        else
            stream->job = m_jobs.run([st = stream] { st->fill(); }, JobPriority::High);
    }
    std::erase_if(s.retired, [](const std::shared_ptr<Stream>& stream) { return stream->job.done(); });
}

void AudioSystem::update() {
    Impl& s = *m_impl;
    detail::EndEvent event;
    while (s.mixer.events().pop(event)) {
        if (event.slot >= s.slots.size()) continue;
        Slot& slot = s.slots[event.slot];
        if (slot.state == Slot::State::Sent && slot.generation == event.generation) endSlot(event.slot, event.reason);
    }

    startPending();
    for (std::uint32_t index : s.dirty) {
        Slot& slot = s.slots[index];
        if (slot.state != Slot::State::Sent || !slot.dirty) continue;
        slot.dirty = false;
        s.send(detail::ParamsCommand{index, slot.generation, slot.params});
    }
    s.dirty.clear();
    for (std::uint32_t b = 0; b < s.buses.size(); ++b) {
        if (!s.busDirty[b]) continue;
        s.busDirty[b] = false;
        sendBus(b);
    }
    if (s.listenerDirty) {
        s.listenerDirty = false;
        s.send(detail::ListenerCommand{m_listener, m_settings.speedOfSound, m_settings.dopplerScale});
    }
    s.flush();
    refillStreams();
    if (m_jobs.workerCount() == 0 && !m_offline && !s.streams.empty()) m_jobs.runFor(0.002);
    s.drainTap();

    // Statistics over windows of audio time; bus meters.
    const detail::Mixer::Stats& mixed = s.mixer.stats();
    const std::uint64_t frames = mixed.frames.load(std::memory_order_relaxed);
    if (frames - s.lastFrames >= std::uint64_t(kStatsWindow * m_settings.sampleRate)) {
        const std::uint64_t calls = mixed.calls.load(std::memory_order_relaxed);
        const std::uint64_t busy = mixed.busyNs.load(std::memory_order_relaxed);
        const double seconds = double(frames - s.lastFrames) / m_settings.sampleRate;
        s.mixMs = calls > s.lastCalls ? double(busy - s.lastBusyNs) * 1e-6 / double(calls - s.lastCalls) : 0.0;
        s.load = double(busy - s.lastBusyNs) * 1e-9 / seconds;
        s.lastFrames = frames;
        s.lastCalls = calls;
        s.lastBusyNs = busy;
    }
    for (std::size_t b = 0; b < s.buses.size(); ++b)
        for (int c = 0; c < 2; ++c) s.buses[b].peak[c] = mixed.meters[b][c].load(std::memory_order_relaxed);
    const std::uint64_t underruns = mixed.underruns.load(std::memory_order_relaxed);
    if (underruns > s.underrunsLogged && s.underrunThrottle.allow()) {
        log::warning("audio", "{} stream underruns (decoding fell behind the mix)", underruns - s.underrunsLogged);
        s.underrunsLogged = underruns;
    }
}

void AudioSystem::render(float* out, std::uint32_t frames) { m_impl->mixer.render(out, frames); }

std::uint32_t AudioSystem::mixOffline(double seconds, std::vector<float>* out) {
    Impl& s = *m_impl;
    s.carry += std::max(seconds, 0.0) * m_settings.sampleRate;
    const auto frames = std::uint32_t(s.carry);
    s.carry -= frames;
    for (std::uint32_t done = 0; done < frames;) {
        const std::uint32_t n = std::min(kOfflineChunk, frames - done);
        for (const std::shared_ptr<Stream>& stream : s.streams) {
            if (stream->job.valid()) m_jobs.wait(stream->job);
            if (stream->needsRefill()) stream->fill();
        }
        s.mixer.render(s.offlineBuffer.data(), n);
        if (out) out->insert(out->end(), s.offlineBuffer.begin(), s.offlineBuffer.begin() + n * kMixChannels);
        s.drainTap();
        done += n;
    }
    return frames;
}

void AudioSystem::setMonitor(double seconds) {
    Impl& s = *m_impl;
    s.historyFrames = std::size_t(std::max(seconds, 0.0) * m_settings.sampleRate);
    s.history.assign(s.historyFrames * kMixChannels, 0.0f);
    s.historyWrite = 0;
    s.mixer.setMonitorEnabled(s.historyFrames > 0);
}

std::vector<float> AudioSystem::monitored(double seconds) const {
    const Impl& s = *m_impl;
    const std::size_t have = std::min(s.historyWrite, s.historyFrames);
    const std::size_t want = std::min(have, std::size_t(std::max(seconds, 0.0) * m_settings.sampleRate));
    std::vector<float> result(want * kMixChannels);
    for (std::size_t f = 0; f < want; ++f) {
        const std::size_t at = ((s.historyWrite - want + f) % s.historyFrames) * kMixChannels;
        result[f * kMixChannels] = s.history[at];
        result[f * kMixChannels + 1] = s.history[at + 1];
    }
    return result;
}

AudioStats AudioSystem::stats() const {
    const Impl& s = *m_impl;
    const detail::Mixer::Stats& mixed = s.mixer.stats();
    AudioStats stats;
    stats.voices = mixed.voices.load(std::memory_order_relaxed);
    stats.maxVoices = m_settings.maxVoices;
    stats.sounds = std::uint32_t(s.slots.size() - s.freeSlots.size());
    stats.pending = std::uint32_t(s.pending.size());
    stats.streams = std::uint32_t(s.streams.size());
    stats.played = s.played;
    stats.stolen = mixed.stolen.load(std::memory_order_relaxed);
    stats.rejected = mixed.rejected.load(std::memory_order_relaxed);
    stats.dropped = s.dropped;
    stats.failed = s.failed;
    stats.refused = s.refused;
    stats.underruns = mixed.underruns.load(std::memory_order_relaxed);
    stats.clipped = mixed.clipped.load(std::memory_order_relaxed);
    stats.framesMixed = mixed.frames.load(std::memory_order_relaxed);
    stats.mixMs = s.mixMs;
    stats.load = s.load;
    return stats;
}

} // namespace quench::audio
