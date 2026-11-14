// The Apple WebSocket backend's C++ side (macOS and iOS): NSURLSessionWebSocketTask in AppleWebSocket.m calls the
// functions below from its delegate queue (a background thread the system owns); they only append to a locked queue
// that poll() takes on the main thread. Events carry the connection's generation: those of a connection replaced or
// aborted meanwhile are dropped.

#include "net/AppleErrors.h"
#include "net/WsBackend.h"

#include <format>
#include <mutex>
#include <string>
#include <utility>

extern "C" {
// AppleWebSocket.m
void* quench_ws_apple_open(void* sink);
void quench_ws_apple_close_session(void* session);
int quench_ws_apple_connect(void* session, std::uint64_t generation, const char* url, const char* const* names,
                            const char* const* values, std::size_t headerCount, const char* protocols,
                            std::int64_t maxMessageBytes, double timeoutSeconds);
void quench_ws_apple_send(void* session, std::uint64_t generation, int binary, const void* data, std::size_t size);
void quench_ws_apple_ping(void* session, std::uint64_t generation);
void quench_ws_apple_close(void* session, std::uint64_t generation, int code, const void* reason, std::size_t size);
void quench_ws_apple_abort(void* session, std::uint64_t generation);

// Called by AppleWebSocket.m (its delegate queue).
void quench_ws_apple_opened(void* sink, std::uint64_t generation, const char* protocol);
void quench_ws_apple_message(void* sink, std::uint64_t generation, int binary, const void* data, std::size_t size);
void quench_ws_apple_pong(void* sink, std::uint64_t generation);
void quench_ws_apple_closed(void* sink, std::uint64_t generation, int code, const void* reason, std::size_t size);
void quench_ws_apple_failed(void* sink, std::uint64_t generation, int domain, long code, int status,
                            const char* message);
void quench_ws_apple_sink_release(void* sink);
}

namespace quench::net::detail {
namespace {

struct WsSink {
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, WsEvent>> events;

    void push(std::uint64_t generation, WsEvent event) {
        std::lock_guard lock(mutex);
        events.emplace_back(generation, std::move(event));
    }
};

class AppleWsBackend final : public WsBackend {
public:
    AppleWsBackend() : m_sink(std::make_shared<WsSink>()) {
        // The session's delegate keeps this reference until the session is invalid (quench_ws_apple_sink_release).
        m_session = quench_ws_apple_open(new std::shared_ptr<WsSink>(m_sink));
    }
    ~AppleWsBackend() override { quench_ws_apple_close_session(m_session); }

    const char* name() const override { return "nsurlsession"; }

    Result<void> connect(const WsConnect& request, bool, double) override {
        abort();
        ++m_generation;
        std::vector<const char*> names, values;
        for (const Header& h : request.headers) {
            names.push_back(h.name.c_str());
            values.push_back(h.value.c_str());
        }
        std::string protocols;
        for (const std::string& p : request.protocols) protocols += (protocols.empty() ? "" : ", ") + p;
        const int status = quench_ws_apple_connect(m_session, m_generation, request.url.c_str(), names.data(),
                                                   values.data(), names.size(), protocols.c_str(),
                                                   std::int64_t(request.maxMessageBytes), request.connectTimeout);
        if (status != 0) return Error{"NSURLSession refused the WebSocket (a URL Foundation cannot parse)"};
        m_live = true;
        return {};
    }

    void send(WsMessage message) override {
        if (!m_live) return;
        quench_ws_apple_send(m_session, m_generation, message.kind == WsKind::Binary ? 1 : 0, message.data.data(),
                             message.data.size());
    }
    void ping() override {
        if (m_live) quench_ws_apple_ping(m_session, m_generation);
    }
    void close(int code, const std::string& reason) override {
        if (m_live) quench_ws_apple_close(m_session, m_generation, code, reason.data(), reason.size());
    }
    void abort() override {
        if (!m_live) return;
        m_live = false;
        quench_ws_apple_abort(m_session, m_generation);
    }

    void poll(double, std::vector<WsEvent>& events) override {
        std::vector<std::pair<std::uint64_t, WsEvent>> taken;
        {
            std::lock_guard lock(m_sink->mutex);
            taken.swap(m_sink->events);
        }
        for (auto& [generation, event] : taken) {
            if (generation != m_generation || !m_live) continue;
            if (event.type == WsEvent::Type::Closed || event.type == WsEvent::Type::Failed) m_live = false;
            events.push_back(std::move(event));
        }
    }

private:
    std::shared_ptr<WsSink> m_sink;
    void* m_session = nullptr;
    std::uint64_t m_generation = 0;
    bool m_live = false;
};

WsSink& sinkOf(void* sink) { return **static_cast<std::shared_ptr<WsSink>*>(sink); }

} // namespace

std::unique_ptr<WsBackend> createPlatformWsBackend() { return std::make_unique<AppleWsBackend>(); }

} // namespace quench::net::detail

using namespace quench::net;
using quench::net::detail::WsEvent;

extern "C" void quench_ws_apple_opened(void* sink, std::uint64_t generation, const char* protocol) {
    detail::sinkOf(sink).push(generation, {.type = WsEvent::Type::Open, .protocol = protocol ? protocol : ""});
}

extern "C" void quench_ws_apple_message(void* sink, std::uint64_t generation, int binary, const void* data,
                                        std::size_t size) {
    WsEvent e{.type = WsEvent::Type::Message};
    e.message.kind = binary ? WsKind::Binary : WsKind::Text;
    e.message.data.assign(static_cast<const char*>(data), size);
    detail::sinkOf(sink).push(generation, std::move(e));
}

extern "C" void quench_ws_apple_pong(void* sink, std::uint64_t generation) {
    detail::sinkOf(sink).push(generation, {.type = WsEvent::Type::Pong});
}

extern "C" void quench_ws_apple_closed(void* sink, std::uint64_t generation, int code, const void* reason,
                                       std::size_t size) {
    detail::sinkOf(sink).push(
        generation,
        {.type = WsEvent::Type::Closed, .code = code, .reason = std::string(static_cast<const char*>(reason), size)});
}

// domain: 0 NSURLErrorDomain (code), 1 another domain, 2 the message size limit, 3 a refused upgrade (status).
extern "C" void quench_ws_apple_failed(void* sink, std::uint64_t generation, int domain, long code, int status,
                                       const char* message) {
    ErrorKind error = ErrorKind::Failed;
    if (domain == 0) error = detail::kindOfUrlError(code);
    if (domain == 2) error = ErrorKind::TooLarge;
    std::string text = message ? message : "";
    if (domain == 3)
        text = std::format("the server refused the upgrade with HTTP {}", status);
    else if (domain <= 1)
        text = std::format("{} (code {})", text, code);
    detail::sinkOf(sink).push(
        generation, {.type = WsEvent::Type::Failed, .error = error, .text = std::move(text), .status = status});
}

extern "C" void quench_ws_apple_sink_release(void* sink) { delete static_cast<std::shared_ptr<detail::WsSink>*>(sink); }
