#include <quench/audio/Microphone.h>

#include "MicBackend.h"

#include <quench/core/Log.h>

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace quench::audio {
namespace {

std::string& usageText() {
    static std::string text;
    return text;
}

constexpr std::uint32_t kFakeRate = 48000;
constexpr std::uint32_t kChunk = 4096;       // frames read from the ring at a time
constexpr double kLevelHalfLife = 0.1;       // seconds for the meter to fall by half
constexpr std::uint32_t kResamplerOrder = 8; // miniaudio's low-pass order (its most): about -40 dB an octave above

// miniaudio's linear resampler with its low-pass filter, mono float. Equal rates copy.
class Resampler {
public:
    Resampler() = default;
    ~Resampler() { reset(); }
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    bool init(std::uint32_t in, std::uint32_t out) {
        reset();
        m_in = in;
        m_out = out;
        if (in == out) return true;
        ma_resampler_config config = ma_resampler_config_init(ma_format_f32, 1, in, out, ma_resample_algorithm_linear);
        config.linear.lpfOrder = kResamplerOrder;
        if (ma_resampler_init(&config, nullptr, &m_resampler) != MA_SUCCESS) return false;
        m_ready = true;
        return true;
    }
    void reset() {
        if (m_ready) ma_resampler_uninit(&m_resampler, nullptr);
        m_ready = false;
    }
    // Appends the resampled frames of `in` to `out`.
    void process(std::span<const float> in, std::vector<float>& out) {
        if (!m_ready) {
            out.insert(out.end(), in.begin(), in.end());
            return;
        }
        std::size_t done = 0;
        while (done < in.size()) {
            const std::size_t room = (in.size() - done) * m_out / m_in + 16;
            const std::size_t base = out.size();
            out.resize(base + room);
            ma_uint64 frameIn = in.size() - done, frameOut = room;
            if (ma_resampler_process_pcm_frames(&m_resampler, in.data() + done, &frameIn, out.data() + base,
                                                &frameOut) != MA_SUCCESS) {
                out.resize(base);
                return;
            }
            out.resize(base + frameOut);
            done += frameIn;
            if (frameIn == 0 && frameOut == 0) break;
        }
    }
    // The frames still inside the filter: silence through it.
    void flush(std::vector<float>& out) {
        if (!m_ready) return;
        const std::vector<float> zeros(ma_resampler_get_input_latency(&m_resampler) + 1, 0.0f);
        process(zeros, out);
    }

private:
    ma_resampler m_resampler{};
    bool m_ready = false;
    std::uint32_t m_in = 0, m_out = 0;
};

std::vector<float> resampleAll(std::span<const float> mono, std::uint32_t in, std::uint32_t out) {
    std::vector<float> result;
    Resampler resampler;
    if (!resampler.init(in, out)) return result;
    result.reserve(mono.size() * out / std::max<std::uint32_t>(in, 1) + 64);
    resampler.process(mono, result);
    resampler.flush(result);
    // The flush adds the filter's delay at the end; the length follows the input's.
    result.resize(std::min(result.size(), std::size_t(std::llround(double(mono.size()) * out / in))));
    return result;
}

// The fake source: samples fed by the game or a scenario, at the pace of update()'s time, then silence.
class FakeBackend final : public detail::MicBackend {
public:
    bool fake() const override { return true; }
    MicPermission permission() override {
        // A request is answered in the update after the one that saw it asking (as a prompt would, a frame later).
        if (m_permission == MicPermission::Asking && ++m_askingPolls > 1) m_permission = MicPermission::Granted;
        return m_permission;
    }
    void requestPermission() override {
        if (m_permission != MicPermission::NotAsked) return;
        m_permission = MicPermission::Asking;
        m_askingPolls = 0;
    }
    void setPermission(MicPermission p) { m_permission = p; }

    void open() override {
        m_open = true;
        m_carry = 0.0;
        if (!m_ring) m_ring = std::make_unique<detail::SampleRing>(1, kFakeRate * 4);
        // Nothing of an earlier recording stays in the ring.
        std::vector<float> discard(kChunk);
        while (m_ring->read(discard.data(), kChunk) > 0) {}
    }
    detail::MicOpen poll(std::string&, bool&) override {
        return m_open ? detail::MicOpen::Open : detail::MicOpen::Closed;
    }
    void pump(double dt) override {
        if (!m_open || dt <= 0.0) return;
        const double exact = dt * kFakeRate + m_carry;
        auto frames = std::uint32_t(exact);
        m_carry = exact - frames;
        while (frames > 0) {
            std::uint32_t granted = 0;
            float* out = m_ring->writeSpan(frames, granted);
            if (granted == 0) {
                m_dropped += frames;
                break;
            }
            const std::size_t fromQueue = std::min<std::size_t>(granted, m_queue.size() - m_read);
            std::copy_n(m_queue.begin() + std::ptrdiff_t(m_read), fromQueue, out);
            std::fill(out + fromQueue, out + granted, 0.0f);
            m_read += fromQueue;
            m_ring->commit(granted);
            frames -= granted;
        }
        if (m_read == m_queue.size()) {
            m_queue.clear();
            m_read = 0;
        }
    }
    detail::SampleRing* ring() override { return m_ring.get(); }
    std::uint32_t sampleRate() const override { return kFakeRate; }
    MicInfo info() const override { return {.device = "fake", .backend = "fake", .deviceSampleRate = kFakeRate}; }
    std::uint64_t dropped() const override { return m_dropped; }
    void close() override { m_open = false; }

    void feed(std::vector<float> mono) {
        if (m_queue.empty())
            m_queue = std::move(mono);
        else
            m_queue.insert(m_queue.end(), mono.begin(), mono.end());
    }
    void clear() {
        m_queue.clear();
        m_read = 0;
    }
    double queued() const { return double(m_queue.size() - m_read) / kFakeRate; }

private:
    MicPermission m_permission = MicPermission::NotAsked;
    int m_askingPolls = 0;
    bool m_open = false;
    double m_carry = 0.0;
    std::unique_ptr<detail::SampleRing> m_ring;
    std::vector<float> m_queue; // mono at kFakeRate
    std::size_t m_read = 0;
    std::uint64_t m_dropped = 0;
};

std::string undeclaredError() {
    return "the game does not declare the microphone: build it with quench_add_game(... MICROPHONE \"<the reason "
           "shown to the player>\") (docs/audio.md, \"Microphone\")";
}

} // namespace

const char* micPermissionName(MicPermission permission) {
    switch (permission) {
    case MicPermission::Unknown: return "unknown";
    case MicPermission::NotAsked: return "not asked";
    case MicPermission::Asking: return "asking";
    case MicPermission::Granted: return "granted";
    case MicPermission::Denied: return "denied";
    case MicPermission::Unavailable: return "unavailable";
    }
    return "?";
}

const char* micStateName(MicState state) {
    switch (state) {
    case MicState::Idle: return "idle";
    case MicState::Opening: return "opening";
    case MicState::Recording: return "recording";
    case MicState::Finished: return "finished";
    case MicState::Failed: return "failed";
    }
    return "?";
}

std::vector<std::int16_t> MicRecording::pcm16() const {
    std::vector<std::int16_t> out(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float s = std::isnan(samples[i]) ? 0.0f : std::clamp(samples[i], -1.0f, 1.0f);
        out[i] = std::int16_t(std::lround(s * 32767.0f));
    }
    return out;
}

fs::Bytes MicRecording::wav() const { return encodeWav(samples, 1, sampleRate, WavSampleFormat::Pcm16); }

void declareMicrophoneUsage(std::string_view reason) { usageText() = std::string(reason); }

const std::string& microphoneUsage() { return usageText(); }

struct Microphone::Impl {
    std::unique_ptr<detail::MicBackend> backend;
    FakeBackend* fake = nullptr; // backend, when it is the fake source
    MicPermission permission = MicPermission::Unknown;
    bool undeclared = false; // a request without a declared usage: Unavailable
    MicState state = MicState::Idle;
    MicDesc desc;
    std::string error;
    MicInfo info;
    Resampler resampler;
    std::vector<float> recorded;
    std::uint64_t maxFrames = 0;
    bool limit = false;
    std::uint64_t droppedAtOpen = 0;
    float level = 0.0f;
    std::vector<float> chunk = std::vector<float>(kChunk);
    std::optional<MicRecording> finished;
    bool warnedFake = false;
    // The streaming mode.
    std::vector<float> stream;  // samples not taken yet
    std::vector<float> scratch; // the resampler's output of one chunk
    std::uint64_t streamed = 0;
    std::uint64_t streamDropped = 0;
    bool muted = false;

    // Moves the resampler's output into the stream (silence while muted), the oldest dropped beyond the buffer.
    void pushStream() {
        if (scratch.empty()) return;
        if (muted) std::fill(scratch.begin(), scratch.end(), 0.0f);
        stream.insert(stream.end(), scratch.begin(), scratch.end());
        streamed += scratch.size();
        scratch.clear();
        const auto max = std::size_t(std::max(0.1, desc.streamBufferSeconds) * desc.sampleRate);
        if (stream.size() > max) {
            const std::size_t drop = stream.size() - max;
            stream.erase(stream.begin(), stream.begin() + std::ptrdiff_t(drop));
            streamDropped += drop;
        }
    }

    void readPermission() {
        const MicPermission now =
            undeclared && microphoneUsage().empty() ? MicPermission::Unavailable : backend->permission();
        if (now != permission)
            log::info("audio", "microphone permission: {}{}", micPermissionName(now), fake ? " (fake source)" : "");
        permission = now;
    }

    // Takes what the ring holds into the recording; true when the limit ended it.
    bool drain() {
        detail::SampleRing* ring = backend->ring();
        if (!ring) return false;
        const double rate = double(backend->sampleRate());
        while (true) {
            const std::uint32_t n = ring->read(chunk.data(), kChunk);
            if (n == 0) return false;
            float peak = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(chunk[i]));
            const float decay = float(std::pow(0.5, double(n) / rate / kLevelHalfLife));
            level = std::min(1.0f, std::max(peak, level * decay));
            if (desc.stream) {
                resampler.process(std::span(chunk.data(), n), scratch);
                pushStream();
                continue;
            }
            resampler.process(std::span(chunk.data(), n), recorded);
            if (recorded.size() >= maxFrames) {
                recorded.resize(maxFrames);
                limit = true;
                return true;
            }
        }
    }

    void finish(std::string why) {
        if (desc.stream) {
            resampler.flush(scratch);
            pushStream();
        } else if (!limit) {
            resampler.flush(recorded);
            if (recorded.size() > maxFrames) {
                recorded.resize(maxFrames);
                limit = true;
            }
        }
        const std::uint64_t dropped = backend->dropped() - droppedAtOpen;
        backend->close();
        MicRecording r{.sampleRate = desc.sampleRate,
                       .samples = std::move(recorded),
                       .limitReached = limit,
                       .error = why,
                       .droppedFrames = dropped};
        recorded = {};
        if (desc.stream)
            log::info("audio", "microphone: streamed {:.3f} s at {} Hz{}{}{}", double(streamed) / desc.sampleRate,
                      desc.sampleRate, dropped ? std::format(", {} input frames lost", dropped) : "",
                      streamDropped ? std::format(", {} samples not taken in time", streamDropped) : "",
                      why.empty() ? "" : ", ended: " + why);
        else
            log::info("audio", "microphone: recorded {:.3f} s at {} Hz{}{}{}", r.seconds(), r.sampleRate,
                      limit ? " (the limit)" : "", dropped ? std::format(", {} input frames lost", dropped) : "",
                      why.empty() ? "" : ", ended: " + why);
        error = std::move(why);
        finished = std::move(r);
        state = MicState::Finished;
        level = 0.0f;
        resampler.reset();
    }
};

Microphone::Microphone() : Microphone(std::make_unique<FakeBackend>()) {}

Microphone::Microphone(std::unique_ptr<detail::MicBackend> backend) : m_impl(std::make_unique<Impl>()) {
    m_impl->backend = std::move(backend);
    m_impl->fake = m_impl->backend->fake() ? static_cast<FakeBackend*>(m_impl->backend.get()) : nullptr;
    m_impl->permission = m_impl->backend->permission();
}

Microphone::~Microphone() {
    if (active()) m_impl->backend->close();
}

MicPermission Microphone::permission() const { return m_impl->permission; }

void Microphone::requestPermission() {
    Impl& s = *m_impl;
    if (microphoneUsage().empty()) {
        s.undeclared = true;
        s.error = undeclaredError();
        log::error("audio", "microphone: {}", s.error);
        s.readPermission();
        return;
    }
    s.undeclared = false;
    s.backend->requestPermission();
    s.readPermission();
}

Result<void> Microphone::start(const MicDesc& desc) {
    Impl& s = *m_impl;
    if (microphoneUsage().empty()) return Error{undeclaredError()};
    if (desc.sampleRate < 8000 || desc.sampleRate > 48000 ||
        (!desc.stream && (!(desc.maxSeconds > 0.0) || desc.maxSeconds > 600.0)))
        return Error{std::format("microphone: a rate of 8000 .. 48000 Hz and a limit of up to 600 s (not {} Hz, {} s)",
                                 desc.sampleRate, desc.maxSeconds)};
    if (desc.stream && !(desc.streamBufferSeconds > 0.0 && desc.streamBufferSeconds <= 60.0))
        return Error{std::format("microphone: a stream buffer of up to 60 s (not {} s)", desc.streamBufferSeconds)};
    if (active()) return Error{std::string("microphone: a recording is under way (") + micStateName(s.state) + ")"};
    s.readPermission();
    if (s.permission != MicPermission::Granted && s.permission != MicPermission::Unknown)
        return Error{std::format("microphone: the permission is {}{}", micPermissionName(s.permission),
                                 s.permission == MicPermission::NotAsked ? " (requestPermission() first)" : "")};
    s.finished.reset();
    s.recorded.clear();
    s.desc = desc;
    s.maxFrames = std::uint64_t(std::llround(desc.maxSeconds * desc.sampleRate));
    s.limit = false;
    s.level = 0.0f;
    s.error.clear();
    s.stream.clear();
    s.scratch.clear();
    s.streamed = 0;
    s.streamDropped = 0;
    s.state = MicState::Opening;
    s.backend->open();
    return {};
}

void Microphone::stop() {
    Impl& s = *m_impl;
    if (s.state == MicState::Opening) {
        s.droppedAtOpen = s.backend->dropped();
        s.finish("");
    } else if (s.state == MicState::Recording) {
        s.backend->pump(0.0);
        s.drain();
        s.finish("");
    }
}

void Microphone::cancel() {
    Impl& s = *m_impl;
    if (active()) s.backend->close();
    s.recorded.clear();
    s.stream.clear();
    s.scratch.clear();
    s.finished.reset();
    s.resampler.reset();
    s.level = 0.0f;
    s.state = MicState::Idle;
}

void Microphone::update(double dt) {
    Impl& s = *m_impl;
    s.readPermission();
    if (!active()) return;
    std::string why;
    bool noDevice = false;
    const detail::MicOpen open = s.backend->poll(why, noDevice);
    if (s.state == MicState::Opening) {
        if (open == detail::MicOpen::Failed || open == detail::MicOpen::Closed) {
            s.backend->close();
            s.error = why.empty() ? std::string("the input closed") : why;
            if (noDevice) {
                s.undeclared = false;
                s.permission = MicPermission::Unavailable;
            }
            s.state = MicState::Failed;
            log::warning("audio", "microphone: cannot record: {}", s.error);
            return;
        }
        if (open != detail::MicOpen::Open) return;
        s.info = s.backend->info();
        if (!s.resampler.init(s.backend->sampleRate(), s.desc.sampleRate)) {
            s.backend->close();
            s.error = std::format("no resampler from {} to {} Hz", s.backend->sampleRate(), s.desc.sampleRate);
            s.state = MicState::Failed;
            return;
        }
        s.droppedAtOpen = s.backend->dropped();
        s.state = MicState::Recording;
        if (s.desc.stream)
            log::info("audio", "microphone: streaming from '{}' ({}, {} Hz) at {} Hz", s.info.device, s.info.backend,
                      s.info.deviceSampleRate, s.desc.sampleRate);
        else
            log::info("audio", "microphone: recording from '{}' ({}, {} Hz) at {} Hz, up to {} s", s.info.device,
                      s.info.backend, s.info.deviceSampleRate, s.desc.sampleRate, s.desc.maxSeconds);
    }
    if (open == detail::MicOpen::Failed) {
        s.drain();
        s.finish(why.empty() ? std::string("the input stopped") : why);
        return;
    }
    s.backend->pump(dt);
    if (s.drain()) s.finish("");
}

MicState Microphone::state() const { return m_impl->state; }

float Microphone::level() const { return m_impl->state == MicState::Recording ? m_impl->level : 0.0f; }

double Microphone::seconds() const {
    const Impl& s = *m_impl;
    if (s.desc.stream) return streamedSeconds();
    if (s.finished) return s.finished->seconds();
    return s.desc.sampleRate ? double(s.recorded.size()) / s.desc.sampleRate : 0.0;
}

bool Microphone::limitReached() const { return m_impl->finished ? m_impl->finished->limitReached : m_impl->limit; }

const std::string& Microphone::error() const { return m_impl->error; }

std::optional<MicRecording> Microphone::takeRecording() {
    Impl& s = *m_impl;
    if (s.state != MicState::Finished || !s.finished) return std::nullopt;
    std::optional<MicRecording> r = std::move(s.finished);
    s.finished.reset();
    s.state = MicState::Idle;
    return r;
}

const MicInfo& Microphone::info() const { return m_impl->info; }

bool Microphone::streaming() const { return m_impl->desc.stream && active(); }

std::size_t Microphone::takeStream(std::vector<float>& out) {
    Impl& s = *m_impl;
    const std::size_t n = s.stream.size();
    out.insert(out.end(), s.stream.begin(), s.stream.end());
    s.stream.clear();
    return n;
}

void Microphone::setStreamMuted(bool muted) {
    if (m_impl->muted != muted) log::info("audio", "microphone: stream {}", muted ? "muted" : "unmuted");
    m_impl->muted = muted;
}

bool Microphone::streamMuted() const { return m_impl->muted; }

double Microphone::streamedSeconds() const {
    const Impl& s = *m_impl;
    return s.desc.sampleRate ? double(s.streamed) / s.desc.sampleRate : 0.0;
}

std::uint64_t Microphone::streamDropped() const { return m_impl->streamDropped; }

bool Microphone::fake() const { return m_impl->fake != nullptr; }

void Microphone::feed(std::span<const float> samples, std::uint32_t channels, std::uint32_t sampleRate) {
    Impl& s = *m_impl;
    if (!s.fake) {
        if (!s.warnedFake) log::warning("audio", "microphone: feed() on a device's microphone does nothing");
        s.warnedFake = true;
        return;
    }
    if (channels == 0 || sampleRate == 0 || samples.empty()) return;
    std::vector<float> mono(samples.size() / channels);
    for (std::size_t f = 0; f < mono.size(); ++f) {
        float sum = 0.0f;
        for (std::uint32_t c = 0; c < channels; ++c) sum += samples[f * channels + c];
        mono[f] = sum / float(channels);
    }
    s.fake->feed(sampleRate == kFakeRate ? std::move(mono) : resampleAll(mono, sampleRate, kFakeRate));
}

void Microphone::feed(const SoundData& sound) { feed(sound.samples, sound.format.channels, sound.format.sampleRate); }

void Microphone::clearFeed() {
    if (m_impl->fake) m_impl->fake->clear();
}

double Microphone::queuedSeconds() const { return m_impl->fake ? m_impl->fake->queued() : 0.0; }

void Microphone::setFakePermission(MicPermission permission) {
    Impl& s = *m_impl;
    if (!s.fake) {
        if (!s.warnedFake)
            log::warning("audio", "microphone: setFakePermission() on a device's microphone does nothing");
        s.warnedFake = true;
        return;
    }
    s.fake->setPermission(permission);
    s.readPermission();
}

} // namespace quench::audio
