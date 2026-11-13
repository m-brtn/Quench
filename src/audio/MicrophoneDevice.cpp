// The microphone on miniaudio's capture device (every build with device I/O except the web, MicrophoneWeb.cpp): the
// system's default input as mono float frames at the device's own rate (miniaudio converts the channels; the
// Microphone resamples on the main thread). The device's callback thread only copies into the ring. Opening and
// closing run as jobs on the engine's job system, one after another: ma_device_init and ma_device_start take a few
// to some tens of milliseconds with Core Audio (more when a Bluetooth headset changes its profile), and on iOS the
// audio session's category changes for the recording first (AudioSessionApple.m), which the documentation says can
// block. The permission comes from MicPermissionApple.m on Apple platforms; elsewhere it is unknown (the device
// is tried).

#include <quench/audio/Device.h>
#include <quench/core/Jobs.h>
#include <quench/core/Log.h>

#include "MicBackend.h"
#include "core/Platform.h"

#if QUENCH_PLATFORM_APPLE
#include "MicPermission.h"
#endif
#if QUENCH_PLATFORM_IOS
#include "AudioSession.h"
#endif

#include <miniaudio.h>

#include <atomic>
#include <cctype>
#include <format>
#include <memory>
#include <span>

namespace quench::audio {
namespace {

using detail::MicOpen;

constexpr std::uint32_t kRingSeconds = 4; // of input the main thread may fall behind before frames are lost

// One opening of the input, shared by its open and close jobs and the main thread.
struct Capture {
    ma_context context{};
    ma_device device{};
    bool contextReady = false;
    bool deviceReady = false;
    bool sessionSwitched = false;
    std::unique_ptr<detail::SampleRing> ring;
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool> stopped{false}; // by the system: the device went away, an interruption
    std::atomic<MicOpen> phase{MicOpen::Opening};
    // Written by the open job before phase is set (release), read after it (acquire).
    std::string error;
    bool noDevice = false;
    std::uint32_t rate = 0;
    MicInfo info;

    ~Capture() {
        if (deviceReady) ma_device_uninit(&device);
        if (contextReady) ma_context_uninit(&context);
    }

    void fail(std::string why, bool none = false) {
        error = std::move(why);
        noDevice = none;
        phase.store(MicOpen::Failed, std::memory_order_release);
    }
};

void openCapture(Capture& c, const MicDeviceDesc& desc, bool null) {
#if QUENCH_PLATFORM_IOS
    if (!null) {
        char text[256] = {};
        if (!quench_audio_session_record(true, int(desc.session), text, sizeof(text)))
            return c.fail(std::format("the audio session cannot record: {}", text));
        c.sessionSwitched = true;
    }
#else
    (void)desc;
#endif
    ma_context_config contextConfig = ma_context_config_init();
#if QUENCH_PLATFORM_IOS
    // The engine keeps the session (Device.cpp, AudioSessionApple.m).
    contextConfig.coreaudio.sessionCategory = ma_ios_session_category_none;
    contextConfig.coreaudio.noAudioSessionActivate = MA_TRUE;
    contextConfig.coreaudio.noAudioSessionDeactivate = MA_TRUE;
#endif
    const ma_backend nullBackend[] = {ma_backend_null};
    ma_result result = ma_context_init(null ? nullBackend : nullptr, null ? 1 : 0, &contextConfig, &c.context);
    if (result != MA_SUCCESS) return c.fail(std::format("no audio backend: {}", ma_result_description(result)));
    c.contextReady = true;
    if (!null) {
        ma_device_info* inputs = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&c.context, nullptr, nullptr, &inputs, &count) == MA_SUCCESS && count == 0)
            return c.fail("no input device", true);
    }

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = 1; // miniaudio averages the device's channels
    config.sampleRate = 0;       // the device's own rate: no resampling on the callback thread
    config.periodSizeInMilliseconds = 20;
    config.noFixedSizedCallback = MA_TRUE;
    config.dataCallback = [](ma_device* device, void*, const void* input, ma_uint32 frames) {
        auto* c = static_cast<Capture*>(device->pUserData);
        const std::uint32_t written = c->ring->write(static_cast<const float*>(input), frames);
        if (written < frames) c->dropped.fetch_add(frames - written, std::memory_order_relaxed);
    };
    // On a system thread: only atomics.
    config.notificationCallback = [](const ma_device_notification* notification) {
        auto* c = static_cast<Capture*>(notification->pDevice->pUserData);
        if (notification->type == ma_device_notification_type_stopped ||
            notification->type == ma_device_notification_type_interruption_began)
            c->stopped.store(true, std::memory_order_release);
    };
    config.pUserData = &c;
    result = ma_device_init(&c.context, &config, &c.device);
    if (result != MA_SUCCESS)
        return c.fail(std::format("cannot open the input: {}", ma_result_description(result)), result == MA_NO_DEVICE);
    c.deviceReady = true;
    c.rate = c.device.sampleRate;
    c.ring = std::make_unique<detail::SampleRing>(1, std::max<std::uint32_t>(c.rate, 8000) * kRingSeconds);
    ma_device_info deviceInfo{};
    if (ma_device_get_info(&c.device, ma_device_type_capture, &deviceInfo) == MA_SUCCESS)
        c.info.device = deviceInfo.name;
    c.info.backend = ma_get_backend_name(c.context.backend);
    c.info.deviceSampleRate = c.rate;
    result = ma_device_start(&c.device);
    if (result != MA_SUCCESS) return c.fail(std::format("cannot start the input: {}", ma_result_description(result)));
    c.phase.store(MicOpen::Open, std::memory_order_release);
}

void closeCapture(Capture& c, const MicDeviceDesc& desc) {
    if (c.deviceReady) ma_device_uninit(&c.device);
    c.deviceReady = false;
    if (c.contextReady) ma_context_uninit(&c.context);
    c.contextReady = false;
#if QUENCH_PLATFORM_IOS
    if (c.sessionSwitched) {
        char text[256] = {};
        if (!quench_audio_session_record(false, int(desc.session), text, sizeof(text)))
            log::warning("audio", "audio session category {} after recording: {}", sessionCategoryName(desc.session),
                         text);
    }
#else
    (void)desc;
#endif
}

class DeviceBackend final : public detail::MicBackend {
public:
    DeviceBackend(JobSystem& jobs, MicDeviceDesc desc) : m_jobs(jobs), m_desc(std::move(desc)) {
        std::string name = m_desc.name;
        for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        m_null = name == "null";
        if (!m_null && !name.empty() && name != "default")
            log::warning("audio",
                         "microphone '{}': only the default input is supported (device selection is not "
                         "there yet)",
                         m_desc.name);
#if QUENCH_PLATFORM_APPLE
        char bundle[1024] = {};
        m_bundleOk = m_null || quench_mic_bundle_declares_usage(bundle, sizeof(bundle));
        if (!m_bundleOk)
            log::error("audio", "microphone: the app bundle {} has no NSMicrophoneUsageDescription in its Info.plist",
                       bundle);
#endif
    }
    ~DeviceBackend() override {
        close();
        m_jobs.wait(m_last);
    }

    MicPermission permission() override {
        if (m_null) return MicPermission::Granted; // miniaudio's null backend records silence, nothing to ask
#if QUENCH_PLATFORM_APPLE
        if (!m_bundleOk) return MicPermission::Unavailable;
        return MicPermission(quench_mic_permission());
#else
        return MicPermission::Unknown; // Windows, Linux: no query here; the device is tried
#endif
    }
    void requestPermission() override {
#if QUENCH_PLATFORM_APPLE
        if (!m_null && m_bundleOk) quench_mic_request_permission();
#endif
    }

    void open() override {
        m_current = std::make_shared<Capture>();
        std::shared_ptr<Capture> c = m_current;
        const Job previous = m_last;
        m_last = m_jobs.run(
            "audio.mic_open", [c, desc = m_desc, null = m_null] { openCapture(*c, desc, null); }, JobPriority::Normal,
            std::span(&previous, 1));
    }
    MicOpen poll(std::string& error, bool& noDevice) override {
        if (m_jobs.workerCount() == 0 && !m_last.done()) m_jobs.runFor(0.0); // no workers: progress on this thread
        if (!m_current) return MicOpen::Closed;
        const MicOpen phase = m_current->phase.load(std::memory_order_acquire);
        if (phase == MicOpen::Failed) {
            error = m_current->error;
            noDevice = m_current->noDevice;
        } else if (phase == MicOpen::Open && m_current->stopped.load(std::memory_order_acquire)) {
            error = "the input stopped (the device went away or the system took it)";
            return MicOpen::Failed;
        }
        return phase;
    }
    detail::SampleRing* ring() override {
        return m_current && m_current->phase.load(std::memory_order_acquire) == MicOpen::Open ? m_current->ring.get()
                                                                                              : nullptr;
    }
    std::uint32_t sampleRate() const override { return m_current ? m_current->rate : 0; }
    MicInfo info() const override { return m_current ? m_current->info : MicInfo{}; }
    std::uint64_t dropped() const override {
        return m_current ? m_current->dropped.load(std::memory_order_relaxed) : 0;
    }
    void close() override {
        if (!m_current) return;
        std::shared_ptr<Capture> c = std::move(m_current);
        const Job previous = m_last;
        m_last = m_jobs.run(
            "audio.mic_close", [c, desc = m_desc] { closeCapture(*c, desc); }, JobPriority::Normal,
            std::span(&previous, 1));
    }

private:
    JobSystem& m_jobs;
    MicDeviceDesc m_desc;
    bool m_null = false;
    bool m_bundleOk = true;
    std::shared_ptr<Capture> m_current; // the opening the main thread looks at; null while closed
    Job m_last;                         // the last open or close job: the next one runs after it
};

} // namespace

std::unique_ptr<Microphone> makeMicrophone(JobSystem& jobs, const MicDeviceDesc& desc) {
    return std::make_unique<Microphone>(std::make_unique<DeviceBackend>(jobs, desc));
}

} // namespace quench::audio
