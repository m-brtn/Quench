#pragma once

#include <quench/net/WebSocket.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The backend interface of net::WebSocket. One connection at a time per backend: connect() replaces what ran before
// (whose later events are dropped by the backend). One platform backend per build (CMakeLists.txt picks its file):
// AppleWebSocket.cpp with AppleWebSocket.m (NSURLSessionWebSocketTask) or NoneWebSocket.cpp; the fake
// (FakeWebSocket.cpp) is built everywhere. The client (WebSocket.cpp) keeps the rules, limits, timeouts and the log.
namespace quench::net::detail {

struct WsEvent {
    enum class Type : std::uint8_t { Open, Message, Pong, Closed, Failed };
    Type type = Type::Open;
    std::string protocol;                // Open
    WsMessage message;                   // Message
    int code = 0;                        // Closed: the close frame's code
    std::string reason;                  // Closed
    ErrorKind error = ErrorKind::Failed; // Failed
    std::string text;                    // Failed: why
    int status = 0;                      // Failed: the HTTP status of a refused upgrade
};

class WsBackend {
public:
    virtual ~WsBackend() = default;
    virtual const char* name() const = 0;
    virtual bool available() const { return true; }
    // Starts a checked connection (the URL passed checkWebSocketUrl, the headers are well formed).
    virtual Result<void> connect(const WsConnect& request, bool allowInsecure, double now) = 0;
    virtual void send(WsMessage message) = 0;
    virtual void ping() = 0;
    // Sends a close frame; a Closed event follows when the server answers (or the client's timeout ends it).
    virtual void close(int code, const std::string& reason) = 0;
    // Drops the connection without a close frame; nothing of it is reported afterwards.
    virtual void abort() = 0;
    // Appends what arrived since the last call, in order. Main thread.
    virtual void poll(double now, std::vector<WsEvent>& events) = 0;
};

std::unique_ptr<WsBackend> createPlatformWsBackend();
std::unique_ptr<WsBackend> createNoneWsBackend();
std::unique_ptr<WsBackend> createFakeWsBackend(FakeWebSocketServer& server);

} // namespace quench::net::detail
