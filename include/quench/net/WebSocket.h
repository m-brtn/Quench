#pragma once

#include <quench/core/Result.h>
#include <quench/net/Http.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A WebSocket client (docs/net.md, "WebSocket"): one connection at a time per WebSocket object, to a wss:// URL with
// the game's headers (an Authorization header for an API), whole text and binary messages both ways, pings, a close
// with a code. Nothing blocks: connect() and the sends return at once, and update(), called once a frame on the main
// thread, calls the handlers there in the order things arrived: onOpen, every onMessage, every onPong and, exactly
// once per connect() that returned success, onEnd (a close by either side, an error, abort()).
//
// Rules built in, as for net::Client: wss only, except ws://localhost, ws://127.0.0.1 and ws://[::1]
// (WebSocketDesc::allowInsecure lifts it for tests); no credentials in the URL; the log shows the host and path, the
// close code, the counts and the milliseconds, never a header's value, a message or the query string; a received
// message over WsConnect::maxMessageBytes ends the connection (ErrorKind::TooLarge, close code 1009).
//
// Backends: NSURLSessionWebSocketTask on macOS and iOS, none elsewhere yet (the browser's WebSocket cannot send an
// Authorization header, so it would not reach the APIs this was made for), and a programmable fake for tests
// (quench/net/FakeWebSocket.h). A WebSocket is used from the thread that created it; it must not be destroyed inside
// one of its handlers.
namespace quench::net {

enum class WsState : std::uint8_t {
    Idle,       // nothing connected yet
    Connecting, // connect() was called; the handshake runs
    Open,       // messages go both ways
    Closing,    // close() was sent; waiting for the server's close
    Closed,     // ended by a close handshake (either side): end() says how
    Failed,     // ended by an error, a timeout or abort(): end() says why
};
const char* wsStateName(WsState state); // "idle", "connecting", ...

enum class WsKind : std::uint8_t { Text, Binary };

struct WsMessage {
    WsKind kind = WsKind::Text;
    std::string data; // the UTF-8 text, or the bytes of a binary message

    std::span<const std::uint8_t> bytes() const {
        return {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
    }
};

// Close codes of RFC 6455 (section 7.4.1) a game meets.
inline constexpr int kWsNormalClosure = 1000;
inline constexpr int kWsGoingAway = 1001;
inline constexpr int kWsNoStatus = 1005;        // a close frame without a code
inline constexpr int kWsAbnormalClosure = 1006; // the connection ended without a close frame (never sent)
inline constexpr int kWsMessageTooBig = 1009;

struct WsConnect {
    std::string url; // wss://host/path; ws:// only to the loopback address
    Headers headers; // sent with the upgrade request (Authorization ...); their values are never logged
    std::vector<std::string> protocols; // Sec-WebSocket-Protocol offers; empty: none
    double connectTimeout = 15.0;       // seconds from connect() to the open connection; 0: no limit
    double closeTimeout = 5.0;          // seconds from close() to the server's close; then the connection is dropped
    double pongTimeout = 10.0;          // seconds a ping waits for its pong before the connection fails; 0: no limit
    // A received message over this ends the connection (TooLarge). Set as the Apple backend's maximumMessageSize,
    // whose default Apple does not state.
    std::size_t maxMessageBytes = 4u << 20;
};

// How a connection ended (WsHandlers::onEnd, WebSocket::end()).
struct WsEnd {
    ErrorKind error = ErrorKind::None; // None: a close handshake ended it
    int closeCode = 0;                 // the close frame's code (0 without one)
    std::string closeReason;           // the close frame's reason (UTF-8, at most 123 bytes)
    bool byServer = false;             // the server closed first (or the connection was lost)
    int status = 0;                    // the HTTP status of a refused upgrade (401, 403, 404 ...), else 0
    std::string message;               // why it failed, readable; without the URL's query string
    double seconds = 0.0;              // from connect() to the end
    std::uint64_t messagesIn = 0, messagesOut = 0;
    std::uint64_t bytesIn = 0, bytesOut = 0; // message payloads

    bool clean() const { return error == ErrorKind::None; }
};

// Called on the main thread from WebSocket::update() only.
struct WsHandlers {
    std::function<void()> onOpen;
    std::function<void(const WsMessage& message)> onMessage;
    std::function<void(double seconds)> onPong;  // the round trip of a ping()
    std::function<void(const WsEnd& end)> onEnd; // once per successful connect(), also after abort()
};

class FakeWebSocketServer;

struct WebSocketDesc {
    FakeWebSocketServer* fake = nullptr; // connect to this fake server instead of the platform's backend
    bool noBackend = false;              // no backend: every connect fails as on a platform without one
    bool allowInsecure = false;          // plain ws:// to any host (tests against a server elsewhere)
    std::function<double()> clock;       // seconds, for the timeouts and WsEnd::seconds; default: a steady clock
};

namespace detail {
class WsBackend;
}

class WebSocket {
public:
    explicit WebSocket(const WebSocketDesc& desc = {});
    // Drops a running connection without a close frame and without calling its handlers.
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // Starts a connection: Connecting, then Open (onOpen) or an end (onEnd). Fails at once, and no handler is called,
    // for a URL the rules refuse, a malformed header, a platform without a backend or a connection under way
    // (Connecting, Open, Closing). After an end a new connect() may follow, from a handler too.
    Result<void> connect(WsConnect request, WsHandlers handlers = {});
    // While Open: queues a message for the server. An error (and nothing sent) in any other state.
    Result<void> sendText(std::string text);
    Result<void> sendBinary(std::span<const std::uint8_t> bytes);
    // While Open: a ping; its pong arrives as onPong. NSURLSession sends pings only when asked: an idle connection
    // stays without one unless the game pings.
    Result<void> ping();
    // Sends a close frame (Open, Connecting) and waits up to WsConnect::closeTimeout for the server's; onEnd follows
    // with error None. Nothing in other states.
    void close(int code = kWsNormalClosure, std::string reason = {});
    // Drops the connection at once without a close frame: onEnd (ErrorKind::Cancelled) from the next update().
    void abort();
    // Once a frame on the main thread: delivers what arrived, ends a connection past its timeouts.
    void update();

    WsState state() const { return m_state; }
    bool open() const { return m_state == WsState::Open; }
    // True while a connection is under way (Connecting, Open, Closing).
    bool active() const;
    // The end of the last connection (valid once Closed or Failed).
    const WsEnd& end() const { return m_end; }
    // The Sec-WebSocket-Protocol the server chose (empty: none).
    const std::string& protocol() const { return m_protocol; }
    // "nsurlsession", "fake" or "none".
    const char* backendName() const;
    bool available() const;

private:
    void finish(ErrorKind error, std::string message, int code, std::string reason, bool byServer, int status);
    double now() const;

    WebSocketDesc m_desc;
    std::unique_ptr<detail::WsBackend> m_backend;
    WsConnect m_request;
    WsHandlers m_handlers;
    WsState m_state = WsState::Idle;
    WsEnd m_end;
    std::string m_protocol;
    std::string m_where; // host and path, for the log
    double m_start = 0.0;
    double m_closeStart = 0.0;
    std::vector<double> m_pings; // send times of pings waiting for their pong
    std::uint64_t m_generation = 0;
    bool m_endPending = false; // an end to deliver from the running or next update()
    bool m_updating = false;
};

// The WebSocket rule: an absolute wss:// URL, or ws:// to localhost, 127.0.0.1 or [::1] (any ws:// host with
// allowInsecure), without user:password@.
Result<void> checkWebSocketUrl(std::string_view url, bool allowInsecure = false);

} // namespace quench::net
