// The microphone in a browser (web builds of quench_audio_device): getUserMedia directly, not miniaudio's Web Audio
// capture, whose getUserMedia failure reaches C only as a generic error (a denial, no device and an insecure page
// look the same there) and whose permission state it does not report. The stream feeds its own AudioContext at the
// browser's rate through a ScriptProcessorNode (as miniaudio's playback does in this single-threaded build), whose
// callback copies each block into a queue in JavaScript (Module.quenchMic); pump() takes the queue into the ring on
// the main thread, so nothing calls into the program from a browser callback. The browser's echo cancellation, noise
// suppression and gain control are asked off (the same raw input as on the other platforms).
//
// The permission: navigator.permissions.query({name: "microphone"}) where the browser has it (granted, denied,
// prompt: not asked), else unknown; getUserMedia needs a secure context (https, or http on localhost or 127.0.0.1)
// and is missing elsewhere (Unavailable). A browser shows its prompt at getUserMedia (requestPermission() and the
// opening); an AudioContext starts only after a user gesture on the page (a key, a click, a touch) unless the
// browser allows audio without one: the opening waits for it.
//
// No arrow functions or strict comparisons (=== and !==) in the JavaScript below: clang-format would put spaces into
// them (src/app/SdlFileSystem.cpp).

#include <quench/audio/Device.h>
#include <quench/core/Log.h>

#include "MicBackend.h"

#include <emscripten/emscripten.h>

#include <memory>
#include <vector>

EM_JS_DEPS(quench_mic, "$stringToUTF8,$lengthBytesUTF8");

// Permission codes as audio::MicPermission: 0 unknown, 1 not asked, 2 asking, 3 granted, 4 denied, 5 unavailable.
// Phases as detail::MicOpen: 0 closed, 1 opening, 2 open, 3 failed.
EM_JS(void, quench_mic_web_init, (), {
    if (Module["quenchMic"]) return;
    var m = {
        permission : 0,
        asking : false,
        phase : 0,
        serial : 0,
        error : "",
        noDevice : false,
        rate : 0,
        label : "",
        queue : [],
        queued : 0,
        dropped : 0,
        hinted : false
    };
    Module["quenchMic"] = m;
    if (typeof window == "undefined" || !window.isSecureContext || !navigator.mediaDevices ||
        !navigator.mediaDevices.getUserMedia) {
        m.permission = 5;
        m.error = "no getUserMedia here (a page needs https, or http on localhost)";
        return;
    }
    function fromState(state) { return state == "granted" ? 3 : state == "denied" ? 4 : 1; }
    if (navigator.permissions && navigator.permissions.query) {
        try {
            navigator.permissions.query({name : "microphone"})
                .then(function(status) {
                    if (!m.asking && m.permission != 5) m.permission = fromState(status.state);
                    status.onchange = function() {
                        if (!m.asking && m.permission != 5) m.permission = fromState(status.state);
                    };
                })
                .catch(function(){});
        } catch (e) {
            // The browser does not know the "microphone" permission name: unknown, the device is tried.
        }
    }
});

EM_JS(int, quench_mic_web_permission, (), { return Module["quenchMic"].permission; });

// Classifies a getUserMedia failure: 1 denied, 2 no device, 0 another failure (the device is busy, ...).
EM_JS(void, quench_mic_web_define, (), {
    var m = Module["quenchMic"];
    if (m.constraints) return;
    m.constraints = {
        audio : {echoCancellation : false, noiseSuppression : false, autoGainControl : false, channelCount : 1},
        video : false
    };
    m.classify = function(error) {
        var name = error && error.name ? error.name : "";
        m.error = "getUserMedia: " + (name ? name + ": " : "") + (error && error.message ? error.message : error);
        if (name == "NotAllowedError") {
            m.permission = 4;
            return 1;
        }
        if (name == "NotFoundError" || name == "OverconstrainedError" || name == "SecurityError") {
            m.permission = 5;
            m.noDevice = name != "SecurityError";
            return 2;
        }
        return 0;
    };
    m.stopStream = function(stream) {
        if (stream) stream.getTracks().forEach(function(track) { track.stop(); });
    };
});

EM_JS(void, quench_mic_web_request, (), {
    var m = Module["quenchMic"];
    if (m.permission == 4 || m.permission == 5 || m.asking) return;
    var before = m.permission;
    m.asking = true;
    m.permission = 2;
    navigator.mediaDevices.getUserMedia(m.constraints)
        .then(function(stream) {
            m.asking = false;
            m.permission = 3;
            m.stopStream(stream); // asked again when a recording opens (a browser that remembers does not prompt)
        })
        .catch(function(error) {
            m.asking = false;
            if (m.classify(error) == 0) m.permission = before == 2 ? 0 : before;
        });
});

EM_JS(void, quench_mic_web_open, (), {
    var m = Module["quenchMic"];
    var serial = ++m.serial;
    m.phase = 1;
    m.error = "";
    m.noDevice = false;
    m.queue = [];
    m.queued = 0;
    m.dropped = 0;
    function current() { return m.serial == serial && m.phase == 1; }
    navigator.mediaDevices.getUserMedia(m.constraints)
        .then(function(stream) {
            if (!current()) {
                m.stopStream(stream);
                return;
            }
            m.permission = 3;
            var Context = window.AudioContext || window.webkitAudioContext;
            var context = new Context();
            var source = context.createMediaStreamSource(stream);
            var node = context.createScriptProcessor(2048, 1, 1);
            var silent = context.createGain();
            silent.gain.value = 0;
            node.onaudioprocess = function(event) {
                if (m.serial != serial || m.phase != 2) return;
                var block = event.inputBuffer.getChannelData(0);
                if (m.queued + block.length > m.rate * 5) {
                    m.dropped += block.length;
                    return;
                }
                m.queue.push(new Float32Array(block));
                m.queued += block.length;
            };
            source.connect(node);
            node.connect(silent); // a processor runs only while connected to the output
            silent.connect(context.destination);
            m.stream = stream;
            m.context = context;
            m.nodes = [ source, node, silent ];
            m.label = stream.getAudioTracks().length ? stream.getAudioTracks()[0].label : "";
            function running() {
                if (!current()) return;
                m.rate = context.sampleRate;
                m.phase = 2;
            }
            if (context.state == "running") {
                running();
            } else {
                context.onstatechange = function() {
                    if (context.state == "running") running();
                };
                context.resume().catch(function(){});
            }
        })
        .catch(function(error) {
            if (!current()) return;
            m.classify(error);
            m.phase = 3;
        });
});

EM_JS(int, quench_mic_web_phase, (), {
    var m = Module["quenchMic"];
    if (m.phase == 1 && m.context && m.context.state == "suspended") m.context.resume().catch(function(){});
    return m.phase;
});

EM_JS(bool, quench_mic_web_waiting_for_gesture, (), {
    var m = Module["quenchMic"];
    return m.phase == 1 && !!m.context && m.context.state == "suspended";
});

EM_JS(int, quench_mic_web_rate, (), { return Module["quenchMic"].rate; });

EM_JS(bool, quench_mic_web_no_device, (), { return Module["quenchMic"].noDevice; });

EM_JS(double, quench_mic_web_dropped, (), { return Module["quenchMic"].dropped; });

// Copies the browser's error (which = 0) or the input's name (1) into `out`.
EM_JS(void, quench_mic_web_text, (int which, char* out, int size), {
    var m = Module["quenchMic"];
    stringToUTF8(which == 0 ? m.error : m.label, out, size);
});

// Takes up to `max` queued frames into `out`; returns how many.
EM_JS(int, quench_mic_web_take, (float* out, int max), {
    var m = Module["quenchMic"];
    var done = 0;
    while (done < max && m.queue.length > 0) {
        var block = m.queue[0];
        var n = Math.min(block.length, max - done);
        HEAPF32.set(block.subarray(0, n), (out >> 2) + done);
        done += n;
        if (n == block.length) {
            m.queue.shift();
        } else {
            m.queue[0] = block.subarray(n);
        }
    }
    m.queued -= done;
    return done;
});

EM_JS(void, quench_mic_web_close, (), {
    var m = Module["quenchMic"];
    ++m.serial;
    m.phase = 0;
    m.queue = [];
    m.queued = 0;
    if (m.nodes) m.nodes.forEach(function(node) { node.disconnect(); });
    m.nodes = null;
    m.stopStream(m.stream);
    m.stream = null;
    if (m.context) m.context.close().catch(function(){});
    m.context = null;
});

namespace quench::audio {
namespace {

using detail::MicOpen;

class WebBackend final : public detail::MicBackend {
public:
    WebBackend() {
        quench_mic_web_init();
        quench_mic_web_define();
    }
    ~WebBackend() override { close(); }

    MicPermission permission() override { return MicPermission(quench_mic_web_permission()); }
    void requestPermission() override { quench_mic_web_request(); }

    void open() override {
        m_open = true;
        m_hinted = false;
        m_ring.reset();
        quench_mic_web_open();
    }
    MicOpen poll(std::string& error, bool& noDevice) override {
        if (!m_open) return MicOpen::Closed;
        const auto phase = MicOpen(quench_mic_web_phase());
        if (phase == MicOpen::Failed) {
            error = text(0);
            noDevice = quench_mic_web_no_device();
        } else if (phase == MicOpen::Open && !m_ring) {
            m_rate = std::uint32_t(quench_mic_web_rate());
            m_ring = std::make_unique<detail::SampleRing>(1, m_rate * 4);
            m_info = {.device = text(1), .backend = "Web Audio", .deviceSampleRate = m_rate};
        } else if (phase == MicOpen::Opening && !m_hinted && quench_mic_web_waiting_for_gesture()) {
            m_hinted = true;
            log::info("audio", "microphone: the browser starts the input after a user gesture on the page (a key, a "
                               "click, a touch)");
        }
        return phase;
    }
    void pump(double) override {
        if (!m_ring) return;
        while (true) {
            std::uint32_t granted = 0;
            float* out = m_ring->writeSpan(m_ring->capacity(), granted);
            if (granted == 0) break;
            const int n = quench_mic_web_take(out, int(granted));
            m_ring->commit(std::uint32_t(n));
            if (std::uint32_t(n) < granted) break;
        }
    }
    detail::SampleRing* ring() override { return m_ring.get(); }
    std::uint32_t sampleRate() const override { return m_rate; }
    MicInfo info() const override { return m_info; }
    std::uint64_t dropped() const override { return std::uint64_t(quench_mic_web_dropped()); }
    void close() override {
        if (!m_open) return;
        m_open = false;
        quench_mic_web_close();
    }

private:
    static std::string text(int which) {
        std::vector<char> out(512);
        quench_mic_web_text(which, out.data(), int(out.size()));
        return out.data();
    }

    bool m_open = false;
    bool m_hinted = false;
    std::unique_ptr<detail::SampleRing> m_ring;
    std::uint32_t m_rate = 0;
    MicInfo m_info;
};

} // namespace

std::unique_ptr<Microphone> makeMicrophone(JobSystem& jobs, const MicDeviceDesc& desc) {
    (void)jobs;
    if (!desc.name.empty() && desc.name != "default")
        log::warning("audio", "microphone '{}': a browser has only its default input here", desc.name);
    return std::make_unique<Microphone>(std::make_unique<WebBackend>());
}

} // namespace quench::audio
