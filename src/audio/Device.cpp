#include <quench/audio/Device.h>

#include "core/Platform.h"

#if QUENCH_PLATFORM_IOS
#include "AudioSession.h"
#endif

#include <quench/core/Log.h>

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>

namespace quench::audio {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string maError(ma_result result) { return ma_result_description(result); }

} // namespace

const char* sessionCategoryName(SessionCategory category) {
    switch (category) {
    case SessionCategory::Ambient: return "ambient";
    case SessionCategory::SoloAmbient: return "solo ambient";
    case SessionCategory::Playback: return "playback";
    }
    return "?";
}

struct Device::Impl {
    explicit Impl(AudioSystem& a) : audio(a) {}
    ~Impl() {
        if (deviceReady) ma_device_uninit(&device);
        if (contextReady) ma_context_uninit(&context);
#if QUENCH_PLATFORM_IOS
        char error[256] = {};
        if (sessionActive && !quench_audio_session_activate(false, error, sizeof(error)))
            log::warning("audio", "audio session deactivation: {}", error);
#endif
    }

    AudioSystem& audio;
    ma_context context{};
    ma_device device{};
    bool contextReady = false;
    bool deviceReady = false;
    bool usesSession = false; // iOS, except on the null backend
    bool sessionActive = false;
    bool suspended = false;   // by the app (background)
    bool interrupted = false; // main thread's view
    DeviceInfo info;
    std::atomic<bool> interruptionBegan{false}, interruptionEnded{false};
    std::atomic<std::uint64_t> interruptions{0}, reroutes{0};
    std::uint64_t reroutesLogged = 0;
    std::chrono::steady_clock::time_point retryAt{};

    bool activateSession() {
#if QUENCH_PLATFORM_IOS
        if (!usesSession) return true;
        char error[256] = {};
        if (!quench_audio_session_activate(true, error, sizeof(error))) {
            log::warning("audio", "audio session activation: {}", error);
            return false;
        }
        sessionActive = true;
#endif
        return true;
    }

    bool start() {
        if (!activateSession()) return false;
        const ma_result result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            log::warning("audio", "cannot start the output: {}", maError(result));
            return false;
        }
        return true;
    }
};

Device::Device(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Device::~Device() = default;

Result<std::unique_ptr<Device>> Device::open(AudioSystem& audio, const DeviceDesc& desc) {
    auto impl = std::make_unique<Impl>(audio);
    const std::string wanted = lower(desc.name);
    const bool null = wanted == "null";

#if QUENCH_PLATFORM_IOS
    impl->usesSession = !null;
    if (impl->usesSession) {
        char error[256] = {};
        if (!quench_audio_session_configure(int(desc.session), error, sizeof(error)))
            log::warning("audio", "audio session category {}: {}", sessionCategoryName(desc.session), error);
        if (!impl->activateSession()) return Error{"the audio session cannot be activated"};
    }
#endif

    ma_context_config contextConfig = ma_context_config_init();
#if QUENCH_PLATFORM_IOS
    contextConfig.coreaudio.sessionCategory = ma_ios_session_category_none;
    contextConfig.coreaudio.noAudioSessionActivate = MA_TRUE;
    contextConfig.coreaudio.noAudioSessionDeactivate = MA_TRUE;
#endif
    const ma_backend nullBackend[] = {ma_backend_null};
    ma_result result = ma_context_init(null ? nullBackend : nullptr, null ? 1 : 0, &contextConfig, &impl->context);
    if (result != MA_SUCCESS) return Error{std::format("no audio backend: {}", maError(result))};
    impl->contextReady = true;

    ma_device_id id{};
    bool haveId = false;
    if (!null && !wanted.empty() && wanted != "default") {
        ma_device_info* outputs = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&impl->context, &outputs, &count, nullptr, nullptr) == MA_SUCCESS) {
            std::string names;
            for (ma_uint32 i = 0; i < count && !haveId; ++i) {
                if (lower(outputs[i].name).find(wanted) != std::string::npos) {
                    id = outputs[i].id;
                    haveId = true;
                }
                names += std::format("{}'{}'", names.empty() ? "" : ", ", outputs[i].name);
            }
            if (!haveId)
                log::warning("audio", "no output named like '{}' (outputs: {}): using the default", desc.name, names);
        }
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kMixChannels;
    config.playback.pDeviceID = haveId ? &id : nullptr;
    config.sampleRate = audio.sampleRate();
    config.periodSizeInMilliseconds = std::max<std::uint32_t>(desc.periodMs, 1);
    config.noFixedSizedCallback = MA_TRUE;      // the mixer takes any size
    config.noPreSilencedOutputBuffer = MA_TRUE; // render() writes every sample
    config.noClip = MA_TRUE;                    // and clamps them
    config.dataCallback = [](ma_device* device, void* output, const void*, ma_uint32 frames) {
        static_cast<Impl*>(device->pUserData)->audio.render(static_cast<float*>(output), frames);
    };
    // On a system thread (iOS: the notification's): only atomics.
    config.notificationCallback = [](const ma_device_notification* notification) {
        auto* impl = static_cast<Impl*>(notification->pDevice->pUserData);
        switch (notification->type) {
        case ma_device_notification_type_interruption_began:
            impl->interruptions.fetch_add(1, std::memory_order_relaxed);
            impl->interruptionBegan.store(true, std::memory_order_release);
            break;
        case ma_device_notification_type_interruption_ended:
            impl->interruptionEnded.store(true, std::memory_order_release);
            break;
        case ma_device_notification_type_rerouted: impl->reroutes.fetch_add(1, std::memory_order_relaxed); break;
        default: break;
        }
    };
    config.pUserData = impl.get();
    result = ma_device_init(&impl->context, &config, &impl->device);
    if (result != MA_SUCCESS) return Error{std::format("cannot open the output: {}", maError(result))};
    impl->deviceReady = true;

    ma_device_info deviceInfo{};
    if (ma_device_get_info(&impl->device, ma_device_type_playback, &deviceInfo) == MA_SUCCESS)
        impl->info.name = deviceInfo.name;
    impl->info.backend = ma_get_backend_name(impl->context.backend);
    impl->info.sampleRate = impl->device.sampleRate;
    impl->info.deviceSampleRate = impl->device.playback.internalSampleRate;
    impl->info.periodFrames = impl->device.playback.internalPeriodSizeInFrames;

    result = ma_device_start(&impl->device);
    if (result != MA_SUCCESS) return Error{std::format("cannot start the output: {}", maError(result))};
    const std::string session =
        impl->usesSession ? std::format(", session {}", sessionCategoryName(desc.session)) : std::string();
    log::info("audio", "output '{}' ({}): mix {} Hz, device {} Hz, period {} frames{}", impl->info.name,
              impl->info.backend, impl->info.sampleRate, impl->info.deviceSampleRate, impl->info.periodFrames, session);
    return std::unique_ptr<Device>(new Device(std::move(impl)));
}

void Device::update() {
    Impl& s = *m_impl;
    if (s.interruptionBegan.exchange(false, std::memory_order_acq_rel)) {
        s.interrupted = true;
        log::info("audio", "output interrupted (another app or a call took the audio)");
    }
    if (s.interruptionEnded.exchange(false, std::memory_order_acq_rel)) {
        s.interrupted = false;
        if (!s.suspended && s.start()) log::info("audio", "output resumed after the interruption");
    }
    // A start that failed (the session was not available yet) is retried once a second.
    const auto now = std::chrono::steady_clock::now();
    if (!s.suspended && !s.interrupted && !ma_device_is_started(&s.device) && now >= s.retryAt) {
        s.retryAt = now + std::chrono::seconds(1);
        if (s.start()) log::info("audio", "output started again");
    }
    const std::uint64_t reroutes = s.reroutes.load(std::memory_order_relaxed);
    if (reroutes != s.reroutesLogged) {
        s.reroutesLogged = reroutes;
#if QUENCH_PLATFORM_IOS
        char route[256] = {};
        quench_audio_session_route(route, sizeof(route));
        log::info("audio", "output route changed: {}", route);
#else
        log::info("audio", "output route changed");
#endif
    }
}

void Device::suspend() {
    if (m_impl->suspended) return;
    m_impl->suspended = true;
    ma_device_stop(&m_impl->device);
}

void Device::resume() {
    if (!m_impl->suspended) return;
    m_impl->suspended = false;
    m_impl->retryAt = {};
    if (!m_impl->interrupted) m_impl->start();
}

bool Device::running() const { return ma_device_is_started(&m_impl->device); }

const DeviceInfo& Device::info() const { return m_impl->info; }

std::uint64_t Device::interruptions() const { return m_impl->interruptions.load(std::memory_order_relaxed); }

std::uint64_t Device::routeChanges() const { return m_impl->reroutes.load(std::memory_order_relaxed); }

std::vector<std::string> Device::outputNames() {
    std::vector<std::string> names;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) return names;
    ma_device_info* outputs = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&context, &outputs, &count, nullptr, nullptr) == MA_SUCCESS)
        for (ma_uint32 i = 0; i < count; ++i) names.emplace_back(outputs[i].name);
    ma_context_uninit(&context);
    return names;
}

} // namespace quench::audio
