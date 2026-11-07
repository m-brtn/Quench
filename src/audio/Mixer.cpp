#include "Mixer.h"

#include <extras/nodes/ma_reverb_node/verblib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>

namespace quench::audio::detail {
namespace {

constexpr double kFracOne = 4294967296.0; // 2^32
constexpr float kFracScale = float(1.0 / kFracOne);

float fadeCurve(float fade) { return std::sin(fade * float(std::numbers::pi / 2.0)); }

// -100 dB. verblib's tail does not decay to exact zeros by itself (an impulse's was still not zero after 10 s).
constexpr float kReverbSilence = 1e-5f;

bool quiet(const float* samples, std::size_t count) {
    return std::all_of(samples, samples + count, [](float s) { return std::abs(s) < kReverbSilence; });
}

void configure(verblib& state, const ReverbDesc& desc) {
    verblib_set_room_size(&state, std::clamp(desc.roomSize, 0.0f, 1.0f));
    verblib_set_damping(&state, std::clamp(desc.damping, 0.0f, 1.0f));
    verblib_set_width(&state, std::clamp(desc.width, 0.0f, 1.0f));
}

} // namespace

struct Mixer::Reverb {
    verblib state; // wet only (verblib's dry level starts at 0)
    ReverbDesc desc;
    bool valid = false;
    // Idle: the delay lines are zeros and the input has been quiet since, so the output would be zeros. A reverb
    // becomes idle once its input and output have been quiet for twice its longest delay path; its delay lines are
    // then cleared (verblib_initialize, which allocates nothing).
    bool idle = true;
    std::uint32_t quietFrames = 0;
    std::uint32_t idleFrames = 0;
};

Mixer::Mixer(const MixerConfig& config)
    : m_config(config), m_commands(std::size_t(config.maxSounds) * 2 + 256),
      m_events(std::size_t(config.maxSounds) + 64) {
    m_voices.resize(std::size_t(config.maxVoices) * 2);
    m_free.reserve(m_voices.size());
    for (std::uint32_t i = std::uint32_t(m_voices.size()); i-- > 0;) m_free.push_back(i);
    m_active.reserve(m_voices.size());
    m_slotVoice.assign(config.maxSounds, -1);
    m_buses.resize(config.busParents.size());
    for (std::size_t b = 0; b < m_buses.size(); ++b) {
        m_buses[b].parent = b == 0 ? 0 : config.busParents[b];
        m_buses[b].volume = b < config.busVolumes.size() ? config.busVolumes[b] : 1.0f;
        m_buses[b].gain = m_buses[b].volume;
        if (b < config.busReverbs.size() && config.busReverbs[b]) {
            auto reverb = std::make_unique<Reverb>();
            reverb->desc = *config.busReverbs[b];
            reverb->valid = verblib_initialize(&reverb->state, config.sampleRate, kMixChannels) != 0;
            if (reverb->valid) configure(reverb->state, reverb->desc);
            // The longest comb and the four allpasses in series (verblib's tunings are for 44100 Hz), twice.
            constexpr double kLongestPath = verblib_combtuningR8 + verblib_allpasstuningR1 + verblib_allpasstuningR2 +
                                            verblib_allpasstuningR3 + verblib_allpasstuningR4;
            reverb->idleFrames = std::uint32_t(2.0 * kLongestPath * double(config.sampleRate) / 44100.0);
            m_buses[b].reverb = reverb.get();
            m_reverbs.push_back(std::move(reverb));
        }
    }
    m_busBuffers.resize(m_buses.size() * kBlockFrames * kMixChannels);
    const auto maxSourceFrames = std::size_t(std::ceil(kBlockFrames * kMaxStep)) + 2;
    m_scratch.resize(maxSourceFrames * 2);
    if (config.monitorFrames > 0) m_monitor = std::make_unique<SampleRing>(kMixChannels, config.monitorFrames);
    if (config.hrtfVoices > 0) {
        m_hrtf = std::make_unique<Hrtf>(config.sampleRate);
        m_hrtfFilters.reserve(config.hrtfVoices);
        for (std::uint32_t i = 0; i < config.hrtfVoices; ++i) m_hrtfFilters.emplace_back(*m_hrtf, kBlockFrames);
        m_hrtfFree.reserve(config.hrtfVoices);
        for (std::uint32_t i = config.hrtfVoices; i-- > 0;) m_hrtfFree.push_back(i);
        m_hrtfPanned.resize(std::size_t(kBlockFrames) * kMixChannels);
    }
    m_hrtfOn = config.hrtf;
}

Mixer::~Mixer() = default;

void Mixer::render(float* out, std::uint32_t frames) {
    const auto start = std::chrono::steady_clock::now();
    Command command;
    while (m_commands.pop(command)) std::visit([this](const auto& c) { apply(c); }, command);

    for (std::uint32_t done = 0; done < frames;) {
        const std::uint32_t n = std::min(kBlockFrames, frames - done);
        mixBlock(out + std::size_t(done) * kMixChannels, n);
        done += n;
    }
    if (m_monitor && m_monitorEnabled.load(std::memory_order_relaxed)) m_monitor->write(out, frames);

    for (std::size_t b = 0; b < m_buses.size(); ++b)
        for (int c = 0; c < 2; ++c) m_stats.meters[b][c].store(m_buses[b].meter[c], std::memory_order_relaxed);
    m_stats.voices.store(std::uint32_t(m_active.size()), std::memory_order_relaxed);
    m_stats.hrtfVoices.store(std::uint32_t(m_hrtfFilters.size() - m_hrtfFree.size()), std::memory_order_relaxed);
    m_stats.frames.fetch_add(frames, std::memory_order_relaxed);
    m_stats.calls.fetch_add(1, std::memory_order_relaxed);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    m_stats.busyNs.fetch_add(std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                             std::memory_order_relaxed);
}

Mixer::Voice* Mixer::voiceOf(std::uint32_t slot, std::uint32_t generation) {
    if (slot >= m_slotVoice.size() || m_slotVoice[slot] < 0) return nullptr;
    Voice& voice = m_voices[std::size_t(m_slotVoice[slot])];
    return voice.generation == generation ? &voice : nullptr;
}

void Mixer::apply(const PlayCommand& c) {
    if (c.slot >= m_slotVoice.size()) return;
    if (m_slotVoice[c.slot] >= 0) release(std::uint32_t(m_slotVoice[c.slot]), EndReason::Stopped);

    // The voice limit counts sounds that are not fading out.
    std::uint32_t playing = 0;
    Voice* victim = nullptr;
    for (std::uint32_t index : m_active) {
        Voice& v = m_voices[index];
        if (v.stopping) continue;
        ++playing;
        if (v.params.priority > c.params.priority) continue;
        if (!victim || v.params.priority < victim->params.priority ||
            (v.params.priority == victim->params.priority &&
             (v.audibility < victim->audibility || (v.audibility == victim->audibility && v.order < victim->order))))
            victim = &v;
    }
    if (playing >= m_config.maxVoices) {
        if (!victim) {
            m_events.push({c.slot, c.generation, EndReason::Rejected});
            m_stats.rejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        beginStop(*victim, kMinStopFade, EndReason::Stolen);
        m_stats.stolen.fetch_add(1, std::memory_order_relaxed);
    }
    if (m_free.empty()) {
        // Every voice is in use, half of them fading out: cut the oldest fade short.
        std::uint32_t oldest = ~0u;
        for (std::uint32_t index : m_active)
            if (m_voices[index].stopping && (oldest == ~0u || m_voices[index].order < m_voices[oldest].order))
                oldest = index;
        if (oldest != ~0u) release(oldest, m_voices[oldest].stopReason);
    }
    if (m_free.empty()) {
        m_events.push({c.slot, c.generation, EndReason::Rejected});
        m_stats.rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::uint32_t index = m_free.back();
    m_free.pop_back();
    Voice& v = m_voices[index];
    v = Voice{};
    v.slot = c.slot;
    v.generation = c.generation;
    v.kind = c.kind;
    v.sound = c.sound;
    v.stream = c.stream;
    v.generator = c.generator;
    v.channels = std::clamp<std::uint32_t>(c.format.channels, 1, 2);
    v.sourceRate = double(std::max<std::uint32_t>(c.format.sampleRate, 1));
    v.params = c.params;
    v.order = m_order++;
    if (c.fadeIn > 0.0f) {
        v.fade = 0.0f;
        v.fadeRate = 1.0f / (c.fadeIn * float(m_config.sampleRate));
    }
    v.pauseGain = c.params.paused ? 0.0f : 1.0f;
    m_active.push_back(index);
    m_slotVoice[c.slot] = std::int32_t(index);
}

void Mixer::apply(const StopCommand& c) {
    if (Voice* v = voiceOf(c.slot, c.generation)) beginStop(*v, c.fade, EndReason::Stopped);
}

void Mixer::apply(const ParamsCommand& c) {
    if (Voice* v = voiceOf(c.slot, c.generation)) v->params = c.params;
}

void Mixer::apply(const BusCommand& c) {
    if (c.index >= m_buses.size()) return;
    Bus& bus = m_buses[c.index];
    bus.volume = c.volume;
    bus.muted = c.muted;
    bus.paused = c.paused;
}

void Mixer::apply(const ListenerCommand& c) {
    m_listener = c.listener;
    m_speedOfSound = c.speedOfSound;
    m_dopplerScale = c.dopplerScale;
}

void Mixer::apply(const ReverbCommand& c) {
    if (c.index >= m_buses.size() || !m_buses[c.index].reverb || !m_buses[c.index].reverb->valid) return;
    Reverb& reverb = *m_buses[c.index].reverb;
    reverb.desc = c.reverb;
    configure(reverb.state, reverb.desc);
}

void Mixer::apply(const HrtfCommand& c) { m_hrtfOn = c.enabled; }

void Mixer::processReverb(Reverb& reverb, float* samples, std::uint32_t frames) {
    const std::size_t count = std::size_t(frames) * kMixChannels;
    const bool quietInput = quiet(samples, count);
    if (!reverb.valid || (reverb.idle && quietInput)) {
        std::fill(samples, samples + count, 0.0f);
        return;
    }
    reverb.idle = false;
    verblib_process(&reverb.state, samples, samples, frames);
    reverb.quietFrames = quietInput && quiet(samples, count) ? reverb.quietFrames + frames : 0;
    if (reverb.quietFrames >= reverb.idleFrames) {
        verblib_initialize(&reverb.state, m_config.sampleRate, kMixChannels);
        configure(reverb.state, reverb.desc);
        reverb.idle = true;
        reverb.quietFrames = 0;
    }
}

void Mixer::beginStop(Voice& voice, float fade, EndReason reason) {
    const float rate = -1.0f / (std::max(fade, kMinStopFade) * float(m_config.sampleRate));
    if (voice.stopping && voice.fadeRate <= rate) return; // already fading out at least as fast
    if (!voice.stopping) voice.stopReason = reason;
    voice.stopping = true;
    voice.fadeRate = rate;
}

void Mixer::release(std::uint32_t voiceIndex, EndReason reason) {
    Voice& v = m_voices[voiceIndex];
    m_events.push({v.slot, v.generation, reason});
    m_slotVoice[v.slot] = -1;
    if (v.hrtf >= 0) m_hrtfFree.push_back(std::uint32_t(v.hrtf));
    const auto it = std::find(m_active.begin(), m_active.end(), voiceIndex);
    if (it != m_active.end()) {
        *it = m_active.back();
        m_active.pop_back();
    }
    v = Voice{};
    m_free.push_back(voiceIndex);
}

void Mixer::mixBlock(float* out, std::uint32_t frames) {
    const auto rate = float(m_config.sampleRate);
    const float smoothing = std::exp(-float(frames) / (kSmoothingSeconds * rate));
    const float meterDecay = std::exp(-float(frames) / (0.3f * rate));

    std::uint8_t busPaused[kMaxBuses] = {};
    for (std::size_t b = 0; b < m_buses.size(); ++b)
        busPaused[b] = m_buses[b].paused || (b > 0 && busPaused[m_buses[b].parent]);
    std::fill(m_busBuffers.begin(), m_busBuffers.end(), 0.0f);

    for (std::size_t i = 0; i < m_active.size();) {
        const std::uint32_t index = m_active[i];
        Voice& v = m_voices[index];
        if (mixVoice(v, frames, busPaused)) {
            ++i;
            continue;
        }
        release(index, v.stopReason); // moves the last active voice to i
    }

    // Children into parents (a parent has a lower index), then the master bus into the output.
    for (std::size_t b = m_buses.size(); b-- > 0;) {
        Bus& bus = m_buses[b];
        const float target = bus.muted ? 0.0f : bus.volume;
        const float g0 = bus.gain;
        float g1 = target + (g0 - target) * smoothing;
        if (std::abs(g1 - target) < 1e-5f) g1 = target;
        if (bus.reverb) processReverb(*bus.reverb, m_busBuffers.data() + b * kBlockFrames * kMixChannels, frames);
        const float* src = m_busBuffers.data() + b * kBlockFrames * kMixChannels;
        float* dst = b == 0 ? out : m_busBuffers.data() + std::size_t(bus.parent) * kBlockFrames * kMixChannels;
        const float dg = (g1 - g0) / float(frames);
        float peak[2] = {0.0f, 0.0f};
        float g = g0;
        for (std::uint32_t f = 0; f < frames; ++f) {
            g += dg;
            const float l = src[2 * f] * g, r = src[2 * f + 1] * g;
            peak[0] = std::max(peak[0], std::abs(l));
            peak[1] = std::max(peak[1], std::abs(r));
            if (b == 0) {
                dst[2 * f] = l;
                dst[2 * f + 1] = r;
            } else {
                dst[2 * f] += l;
                dst[2 * f + 1] += r;
            }
        }
        bus.gain = g1;
        for (int c = 0; c < 2; ++c) bus.meter[c] = std::max(peak[c], bus.meter[c] * meterDecay);
    }

    std::uint64_t clipped = 0;
    for (std::uint32_t s = 0; s < frames * kMixChannels; ++s) {
        if (std::abs(out[s]) > 1.0f) {
            out[s] = std::clamp(out[s], -1.0f, 1.0f);
            ++clipped;
        } else if (std::isnan(out[s])) {
            out[s] = 0.0f;
            ++clipped;
        }
    }
    if (clipped) m_stats.clipped.fetch_add(clipped, std::memory_order_relaxed);
}

bool Mixer::mixVoice(Voice& v, std::uint32_t frames, const std::uint8_t* busPaused) {
    const auto rate = float(m_config.sampleRate);
    const bool paused = v.params.paused || busPaused[v.params.bus];
    const float pauseStep = float(frames) / (kPauseRampSeconds * rate);
    const float p0 = v.pauseGain;
    const float p1 = paused ? std::max(0.0f, p0 - pauseStep) : std::min(1.0f, p0 + pauseStep);
    if (paused && p0 <= 0.0f) {
        // Frozen in place; a stop while frozen ends it at once (it is silent).
        v.audibility = 0.0f;
        return !v.stopping;
    }

    float volume = v.params.volume;
    double pitch = v.params.pitch;
    glm::vec2 pan;
    if (v.params.spatial) {
        const SpatialResult s = spatialize(m_listener, v.params.emitter, m_speedOfSound, m_dopplerScale);
        volume *= s.gain;
        pitch *= s.pitch;
        pan = panGains(s.pan);
    } else {
        pan = v.channels == 1 ? panGains(v.params.pan) : balanceGains(v.params.pan);
    }
    const float smoothing = std::exp(-float(frames) / (kSmoothingSeconds * rate));

    // The HRTF's share of a spatial voice, smoothed so that switching it on or off does not click. A voice takes a
    // free filter when it needs one (a new voice starts with its full share) and gives it back once its share is 0;
    // without one it stays panned.
    HrtfFilter* hrtf = nullptr;
    HrtfDirection heard;
    float m0 = 0.0f, m1 = 0.0f, hrtfGain1 = 0.0f;
    if (m_hrtf && (v.hrtf >= 0 || (m_hrtfOn && v.params.spatial))) {
        if (v.params.spatial) heard = hrtfDirection(m_listener, v.params.emitter);
        const float share = m_hrtfOn ? heard.weight : 0.0f;
        if (share > 0.0f && v.hrtf < 0 && !m_hrtfFree.empty()) {
            v.hrtf = std::int32_t(m_hrtfFree.back());
            m_hrtfFree.pop_back();
            m_hrtfFilters[std::size_t(v.hrtf)].reset();
            v.hrtfMix = v.gainSet ? 0.0f : share;
            v.hrtfGain = volume;
        }
        if (v.hrtf >= 0) {
            hrtf = &m_hrtfFilters[std::size_t(v.hrtf)];
            m0 = v.hrtfMix;
            m1 = share + (m0 - share) * smoothing;
            if (std::abs(m1 - share) < 1e-4f) m1 = share;
            hrtfGain1 = volume + (v.hrtfGain - volume) * smoothing;
            if (std::abs(hrtfGain1 - volume) < 1e-6f) hrtfGain1 = volume;
        }
    }

    const float target[2] = {volume * pan.x, volume * pan.y};
    if (!v.gainSet) {
        v.gain[0] = target[0];
        v.gain[1] = target[1];
        v.gainSet = true;
    }
    float gain1[2];
    for (int c = 0; c < 2; ++c) {
        gain1[c] = target[c] + (v.gain[c] - target[c]) * smoothing;
        if (std::abs(gain1[c] - target[c]) < 1e-6f) gain1[c] = target[c];
    }
    const double targetStep = std::clamp(v.sourceRate / double(m_config.sampleRate) * pitch, kMinStep, kMaxStep);
    v.step = v.stepSet ? targetStep + (v.step - targetStep) * double(smoothing) : targetStep;
    v.stepSet = true;
    const auto step = std::uint64_t(v.step * kFracOne + 0.5);

    const float f0 = v.fade;
    const float f1 = std::clamp(f0 + v.fadeRate * float(frames), 0.0f, 1.0f);
    if (f1 >= 1.0f && v.fadeRate > 0.0f) v.fadeRate = 0.0f;
    const float fade0 = fadeCurve(f0) * p0, fade1 = fadeCurve(f1) * p1;
    const float start[2] = {v.gain[0] * fade0, v.gain[1] * fade0};
    const float end[2] = {gain1[0] * fade1, gain1[1] * fade1};

    const std::uint32_t channels = v.channels;
    if (!v.primed) {
        float first[4] = {};
        pull(v, first, 2);
        for (std::uint32_t c = 0; c < channels; ++c) {
            v.x0[c] = first[c];
            v.x1[c] = first[channels + c];
        }
        v.primed = true;
    }
    const auto need = std::uint32_t((std::uint64_t(v.frac) + std::uint64_t(frames) * step) >> 32);
    if (need > 0) pull(v, m_scratch.data(), need);

    float* bus = m_busBuffers.data() + std::size_t(v.params.bus) * kBlockFrames * kMixChannels;
    // The send level, smoothed like the gains; no send while it is and stays 0.
    const float s0 = v.send;
    float s1 = v.params.sendLevel + (s0 - v.params.sendLevel) * smoothing;
    if (std::abs(s1 - v.params.sendLevel) < 1e-6f) s1 = v.params.sendLevel;
    float* send = s0 > 0.0f || s1 > 0.0f
                      ? m_busBuffers.data() + std::size_t(v.params.sendBus) * kBlockFrames * kMixChannels
                      : nullptr;
    const float* src = m_scratch.data();
    const bool downmix = v.params.spatial && channels == 2;
    const float inv = 1.0f / float(frames);
    std::uint32_t frac = v.frac;
    float x0[2] = {v.x0[0], v.x0[1]}, x1[2] = {v.x1[0], v.x1[1]};
    // A voice through the HRTF is panned into a block of its own first, which the filter then splits. The send stays
    // panned: the reverb does not need the head's filters.
    float* panned = bus;
    if (hrtf) {
        panned = m_hrtfPanned.data();
        std::fill(panned, panned + std::size_t(frames) * kMixChannels, 0.0f);
    }
    for (std::uint32_t f = 0; f < frames; ++f) {
        const float t = float(frac) * kFracScale;
        float l = x0[0] + (x1[0] - x0[0]) * t;
        float r = l;
        if (channels == 2) {
            r = x0[1] + (x1[1] - x0[1]) * t;
            if (downmix) l = r = 0.5f * (l + r);
        }
        const float w = float(f + 1) * inv;
        const float outL = l * (start[0] + (end[0] - start[0]) * w);
        const float outR = r * (start[1] + (end[1] - start[1]) * w);
        panned[2 * f] += outL;
        panned[2 * f + 1] += outR;
        if (send) {
            const float level = s0 + (s1 - s0) * w;
            send[2 * f] += outL * level;
            send[2 * f + 1] += outR * level;
        }
        const std::uint64_t acc = std::uint64_t(frac) + step;
        frac = std::uint32_t(acc);
        for (auto advance = std::uint32_t(acc >> 32); advance > 0; --advance) {
            x0[0] = x1[0];
            x0[1] = x1[1];
            x1[0] = src[0];
            x1[1] = channels == 2 ? src[1] : 0.0f;
            src += channels;
        }
    }
    v.frac = frac;
    for (int c = 0; c < 2; ++c) {
        v.x0[c] = x0[c];
        v.x1[c] = x1[c];
        v.gain[c] = gain1[c];
    }
    if (hrtf) {
        const HrtfShare share{.left = {start[0], end[0]},
                              .right = {start[1], end[1]},
                              .mix = {m0, m1},
                              .gain = {v.hrtfGain * fade0, hrtfGain1 * fade1}};
        hrtf->mix(panned, frames, share, heard.direction, bus);
        v.hrtfMix = m1;
        v.hrtfGain = hrtfGain1;
        if (m1 <= 0.0f) {
            m_hrtfFree.push_back(std::uint32_t(v.hrtf));
            v.hrtf = -1;
        }
    }
    v.fade = f1;
    v.pauseGain = p1;
    v.send = s1;
    v.audibility = std::max(end[0], end[1]);

    if (v.stopping && (f1 <= 0.0f || (paused && p1 <= 0.0f))) return false;
    if (v.sourceDone) {
        if (!v.stopping) v.stopReason = EndReason::Finished;
        return false;
    }
    return true;
}

std::uint32_t Mixer::pull(Voice& v, float* out, std::uint32_t frames) {
    const std::uint32_t channels = v.channels;
    std::uint32_t done = 0;
    switch (v.kind) {
    case SourceKind::Sound: {
        const std::uint64_t total = v.sound ? v.sound->frames() : 0;
        while (done < frames) {
            if (v.position >= total) {
                if (!v.params.loop || total == 0) break;
                v.position = 0;
            }
            const auto n = std::uint32_t(std::min<std::uint64_t>(frames - done, total - v.position));
            std::memcpy(out + std::size_t(done) * channels, v.sound->samples.data() + v.position * channels,
                        std::size_t(n) * channels * sizeof(float));
            v.position += n;
            done += n;
        }
        if (done < frames) v.sourceDone = true;
        break;
    }
    case SourceKind::Stream: {
        SampleRing& ring = v.stream->ring;
        done = ring.read(out, frames);
        if (done < frames) {
            if (v.stream->ended.load(std::memory_order_acquire)) {
                done += ring.read(out + std::size_t(done) * channels, frames - done);
                if (done < frames) v.sourceDone = true;
            } else {
                m_stats.underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }
        break;
    }
    case SourceKind::Generator:
        done = std::min(v.generator ? v.generator->generate(out, frames) : 0, frames);
        if (done < frames) v.sourceDone = true;
        break;
    }
    if (done < frames)
        std::memset(out + std::size_t(done) * channels, 0, std::size_t(frames - done) * channels * sizeof(float));
    return done;
}

} // namespace quench::audio::detail
