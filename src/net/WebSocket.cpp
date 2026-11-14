// quench::net::WebSocket: one connection's state on the main thread, the rules, the limits, the timeouts and the log
// (quench/net/WebSocket.h). The backends only move messages (WsBackend.h).

#include <quench/net/WebSocket.h>

#include "net/Backend.h"
#include "net/WsBackend.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace quench::net {
namespace {

constexpr const char* kLog = "net";

bool isToken(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        if (!ok) return false;
    }
    return true;
}

bool isHeaderValue(std::string_view text) {
    return std::none_of(text.begin(), text.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

// The headers the WebSocket handshake itself sets: a game's own would break it.
bool isHandshakeHeader(std::string_view name) {
    std::string n(name);
    for (char& c : n) c = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    return n == "upgrade" || n == "connection" || n == "sec-websocket-key" || n == "sec-websocket-version" ||
           n == "sec-websocket-extensions" || n == "sec-websocket-accept" || n == "host";
}

std::string wsLogUrl(std::string_view url) { return logUrl(url); }

} // namespace

const char* wsStateName(WsState state) {
    switch (state) {
    case WsState::Idle: return "idle";
    case WsState::Connecting: return "connecting";
    case WsState::Open: return "open";
    case WsState::Closing: return "closing";
    case WsState::Closed: return "closed";
    case WsState::Failed: return "failed";
    }
    return "?";
}

Result<void> checkWebSocketUrl(std::string_view url, bool allowInsecure) {
    const std::optional<detail::Url> parsed = detail::parseUrl(url);
    if (!parsed) return Error{std::format("'{}' is not an absolute ws or wss URL", logUrl(url))};
    if (parsed->userInfo) return Error{"a URL with user:password@ is not allowed (send credentials in a header)"};
    if (parsed->scheme == "wss") return {};
    if (parsed->scheme == "ws") {
        if (allowInsecure || detail::isLoopbackHost(parsed->host)) return {};
        return Error{std::format("plain ws to {} is not allowed: wss only, except ws://localhost, ws://127.0.0.1 "
                                 "and ws://[::1]",
                                 parsed->host)};
    }
    return Error{std::format("the scheme '{}' is not allowed for a WebSocket (wss only)", parsed->scheme)};
}

WebSocket::WebSocket(const WebSocketDesc& desc) : m_desc(desc) {
    if (m_desc.fake)
        m_backend = detail::createFakeWsBackend(*m_desc.fake);
    else if (m_desc.noBackend)
        m_backend = detail::createNoneWsBackend();
    else
        m_backend = detail::createPlatformWsBackend();
    if (!m_desc.clock) {
        const auto origin = std::chrono::steady_clock::now();
        m_desc.clock = [origin] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
        };
    }
}

WebSocket::~WebSocket() {
    if (active()) m_backend->abort();
}

double WebSocket::now() const { return m_desc.clock(); }
const char* WebSocket::backendName() const { return m_backend->name(); }
bool WebSocket::available() const { return m_backend->available(); }

bool WebSocket::active() const {
    return m_state == WsState::Connecting || m_state == WsState::Open || m_state == WsState::Closing;
}

Result<void> WebSocket::connect(WsConnect request, WsHandlers handlers) {
    const std::string where = wsLogUrl(request.url);
    if (!m_backend->available()) {
        log::warning(kLog, "ws {}: no WebSocket backend on this platform", where);
        return Error{"no WebSocket backend on this platform"};
    }
    if (active() || m_endPending)
        return Error{std::format("a WebSocket connection is under way ({})", wsStateName(m_state))};
    if (Result<void> url = checkWebSocketUrl(request.url, m_desc.allowInsecure); !url) {
        log::warning(kLog, "ws {}: refused: {}", where, url.error());
        return url;
    }
    for (const Header& h : request.headers) {
        if (!isToken(h.name)) return Error{std::format("'{}' is not a header name", h.name)};
        if (!isHeaderValue(h.value)) return Error{std::format("the value of the header {} has a line break", h.name)};
        if (isHandshakeHeader(h.name))
            return Error{std::format("the header {} belongs to the WebSocket handshake", h.name)};
    }
    for (const std::string& p : request.protocols)
        if (!isToken(p)) return Error{std::format("'{}' is not a WebSocket protocol name", p)};
    if (request.maxMessageBytes == 0) return Error{"a message limit of 0 bytes"};
    const double t = now();
    if (Result<void> started = m_backend->connect(request, m_desc.allowInsecure, t); !started) {
        const std::string message = detail::redactText(started.error(), request.url);
        log::warning(kLog, "ws {}: not started: {}", where, message);
        return Error{message};
    }
    m_request = std::move(request);
    m_handlers = std::move(handlers);
    m_where = where;
    m_state = WsState::Connecting;
    m_end = {};
    m_protocol.clear();
    m_pings.clear();
    m_start = t;
    m_closeStart = 0.0;
    ++m_generation;
    log::info(kLog, "ws {}: connecting", m_where);
    return {};
}

Result<void> WebSocket::sendText(std::string text) {
    if (m_state != WsState::Open) return Error{std::format("the WebSocket is not open ({})", wsStateName(m_state))};
    m_end.messagesOut += 1;
    m_end.bytesOut += text.size();
    m_backend->send({.kind = WsKind::Text, .data = std::move(text)});
    return {};
}

Result<void> WebSocket::sendBinary(std::span<const std::uint8_t> bytes) {
    if (m_state != WsState::Open) return Error{std::format("the WebSocket is not open ({})", wsStateName(m_state))};
    m_end.messagesOut += 1;
    m_end.bytesOut += bytes.size();
    m_backend->send(
        {.kind = WsKind::Binary, .data = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())});
    return {};
}

Result<void> WebSocket::ping() {
    if (m_state != WsState::Open) return Error{std::format("the WebSocket is not open ({})", wsStateName(m_state))};
    m_pings.push_back(now());
    m_backend->ping();
    return {};
}

void WebSocket::close(int code, std::string reason) {
    if (m_state != WsState::Open && m_state != WsState::Connecting) return;
    // A close frame's reason is at most 123 bytes (RFC 6455, 5.5); cut on a UTF-8 boundary.
    if (reason.size() > 123) {
        std::size_t n = 123;
        while (n > 0 && (static_cast<unsigned char>(reason[n]) & 0xC0) == 0x80) --n;
        reason.resize(n);
    }
    if (m_state == WsState::Connecting) {
        m_backend->abort();
        finish(ErrorKind::None, {}, code, reason, false, 0);
        return;
    }
    m_end.closeCode = code;
    m_end.closeReason = reason;
    m_state = WsState::Closing;
    m_closeStart = now();
    m_backend->close(code, reason);
}

void WebSocket::abort() {
    if (!active()) return;
    m_backend->abort();
    finish(ErrorKind::Cancelled, "aborted", 0, {}, false, 0);
}

void WebSocket::finish(ErrorKind error, std::string message, int code, std::string reason, bool byServer, int status) {
    QUENCH_ASSERT(active(), "a WebSocket connection ended twice");
    m_state = error == ErrorKind::None ? WsState::Closed : WsState::Failed;
    m_end.error = error;
    m_end.message = detail::redactText(std::move(message), m_request.url);
    m_end.closeCode = code;
    m_end.closeReason = std::move(reason);
    m_end.byServer = byServer;
    m_end.status = status;
    m_end.seconds = std::max(0.0, now() - m_start);
    m_endPending = true;
}

void WebSocket::update() {
    QUENCH_ASSERT(!m_updating, "WebSocket::update() called from one of its handlers");
    m_updating = true;
    const double t = now();
    std::vector<detail::WsEvent> events;
    m_backend->poll(t, events);
    const std::uint64_t generation = m_generation;

    for (detail::WsEvent& event : events) {
        // A handler may have ended this connection or started the next one meanwhile.
        if (!active() || generation != m_generation) break;
        switch (event.type) {
        case detail::WsEvent::Type::Open:
            if (m_state != WsState::Connecting) break;
            m_state = WsState::Open;
            m_protocol = std::move(event.protocol);
            log::info(kLog, "ws {}: open in {} ms{}", m_where, (long long)((t - m_start) * 1000.0 + 0.5),
                      m_protocol.empty() ? "" : ", protocol " + m_protocol);
            if (m_handlers.onOpen) m_handlers.onOpen();
            break;
        case detail::WsEvent::Type::Message:
            if (event.message.data.size() > m_request.maxMessageBytes) {
                m_backend->abort();
                finish(ErrorKind::TooLarge,
                       std::format("a message of {} bytes is over the limit of {}", event.message.data.size(),
                                   m_request.maxMessageBytes),
                       kWsMessageTooBig, {}, false, 0);
                break;
            }
            m_end.messagesIn += 1;
            m_end.bytesIn += event.message.data.size();
            if (m_handlers.onMessage) m_handlers.onMessage(event.message);
            break;
        case detail::WsEvent::Type::Pong:
            if (!m_pings.empty()) {
                const double rtt = std::max(0.0, t - m_pings.front());
                m_pings.erase(m_pings.begin());
                if (m_handlers.onPong) m_handlers.onPong(rtt);
            }
            break;
        case detail::WsEvent::Type::Closed:
            // The server's close frame, first or as the answer to ours.
            finish(ErrorKind::None, {}, event.code, std::move(event.reason), m_state != WsState::Closing, 0);
            break;
        case detail::WsEvent::Type::Failed:
            m_backend->abort();
            finish(event.error, std::move(event.text), 0, {}, true, event.status);
            break;
        }
    }

    if (active() && generation == m_generation) {
        if (m_state == WsState::Connecting && m_request.connectTimeout > 0.0 &&
            t - m_start >= m_request.connectTimeout) {
            m_backend->abort();
            finish(ErrorKind::Timeout, std::format("not open within {} s", m_request.connectTimeout), 0, {}, false, 0);
        } else if (m_state == WsState::Closing && t - m_closeStart >= m_request.closeTimeout) {
            // Our close frame went out; the server did not answer: the connection is dropped, still a close.
            m_backend->abort();
            const int code = m_end.closeCode;
            std::string reason = m_end.closeReason;
            finish(ErrorKind::None, std::format("no close from the server within {} s", m_request.closeTimeout), code,
                   std::move(reason), false, 0);
        } else if (m_state == WsState::Open && m_request.pongTimeout > 0.0 && !m_pings.empty() &&
                   t - m_pings.front() >= m_request.pongTimeout) {
            m_backend->abort();
            finish(ErrorKind::Timeout, std::format("no pong within {} s", m_request.pongTimeout), 0, {}, false, 0);
        }
    }

    if (m_endPending) {
        m_endPending = false;
        const WsEnd& e = m_end;
        const long long ms = (long long)(e.seconds * 1000.0 + 0.5);
        if (e.clean())
            log::info(kLog, "ws {}: closed by the {} with {}{}, {} messages in ({} bytes), {} out ({} bytes), {} ms",
                      m_where, e.byServer ? "server" : "client", e.closeCode,
                      e.closeReason.empty() ? "" : " \"" + e.closeReason + "\"", e.messagesIn, e.bytesIn, e.messagesOut,
                      e.bytesOut, ms);
        else if (e.error == ErrorKind::Cancelled)
            log::info(kLog, "ws {}: aborted after {} messages in, {} out, {} ms", m_where, e.messagesIn, e.messagesOut,
                      ms);
        else
            log::warning(kLog, "ws {}: {} ({}){}, {} messages in, {} out, {} ms", m_where, errorKindName(e.error),
                         e.message, e.status ? std::format(", HTTP {}", e.status) : "", e.messagesIn, e.messagesOut,
                         ms);
        // A handler may connect again: the end is copied first.
        const WsEnd copy = m_end;
        WsHandlers handlers = std::move(m_handlers);
        m_handlers = {};
        m_updating = false;
        if (handlers.onEnd) handlers.onEnd(copy);
        return;
    }
    m_updating = false;
}

} // namespace quench::net
