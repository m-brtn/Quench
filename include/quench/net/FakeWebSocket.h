#pragma once

#include <quench/net/WebSocket.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A programmable WebSocket server for tests of net::WebSocket and of the modules built on it (docs/net.md, "The
// WebSocket fake"): a WebSocket made with WebSocketDesc::fake connects to it instead of the platform's backend,
// through the same rules, limits, timeouts and log. The answer to an upgrade is found by URL; the server's side of a
// connection (FakeWsPeer) records what the client sent and sends scripted messages, closes and failures, each after a
// delay. Time is the client's clock (WebSocketDesc::clock): a test with a manual clock decides when each message
// arrives. Handlers (onOpen, onMessage, onClose) let a test answer the client as a server would.
namespace quench::net {

// How the server answers an upgrade request.
struct FakeWsAnswer {
    int status = 101;                  // 101 accepts; any other status refuses (WsEnd::status)
    double delay = 0.0;                // seconds from connect() to the answer (or the error)
    ErrorKind error = ErrorKind::None; // fail with this instead of answering (NoNetwork, Tls ...)
    std::string message;               // the error's message
    std::string protocol;              // the Sec-WebSocket-Protocol chosen (empty: none)
    bool never = false;                // never answers (a connect timeout)
};

// The server's side of one connection.
class FakeWsPeer {
public:
    struct Impl;
    explicit FakeWsPeer(std::unique_ptr<Impl> impl); // made by the server
    ~FakeWsPeer();
    FakeWsPeer(const FakeWsPeer&) = delete;
    FakeWsPeer& operator=(const FakeWsPeer&) = delete;

    // The upgrade request as the client sent it.
    const std::string& url() const;
    const Headers& headers() const;
    const std::vector<std::string>& protocols() const;
    // Every message the client sent, in order.
    const std::vector<WsMessage>& received() const;
    int pings() const;
    // The client's close frame, once it sent one.
    std::optional<int> clientCloseCode() const;
    const std::string& clientCloseReason() const;
    // Open: not closed, failed or aborted by either side yet.
    bool connected() const;
    bool aborted() const; // the client dropped it without a close frame

    // Each runs `delay` seconds after the previous scheduled action of this peer (after now when none is waiting):
    // a message, a close frame, a lost connection.
    void sendText(std::string text, double delay = 0.0);
    void sendBinary(std::string bytes, double delay = 0.0);
    void close(int code = kWsNormalClosure, std::string reason = {}, double delay = 0.0);
    void fail(ErrorKind error, std::string message, double delay = 0.0);
    // Whether it answers the client's close frame with its own (default true) and pings with pongs (default true).
    void setAnswerClose(bool answer);
    void setAnswerPings(bool answer);

    Impl& impl() { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
};

class FakeWebSocketServer {
public:
    FakeWebSocketServer();
    ~FakeWebSocketServer();
    FakeWebSocketServer(const FakeWebSocketServer&) = delete;
    FakeWebSocketServer& operator=(const FakeWebSocketServer&) = delete;

    // The answer to an upgrade at `url` (exact, or without its query string). Without one the upgrade is refused with
    // 404 ("no fake WebSocket server").
    void answer(std::string url, FakeWsAnswer answer = {});
    // Called when a connection opens, before the client's onOpen.
    void onOpen(std::function<void(FakeWsPeer& peer)> handler);
    // Called for each message the client sends, as it arrives (in the client's update()).
    void onMessage(std::function<void(FakeWsPeer& peer, const WsMessage& message)> handler);
    // Called when the client sends its close frame or aborts.
    void onClose(std::function<void(FakeWsPeer& peer)> handler);

    // Every connection made, in order; the last one or nullptr.
    std::vector<FakeWsPeer*> peers() const;
    FakeWsPeer* last() const;
    // Connections open now.
    int connected() const;
    // The client clock's time at the fake's last poll.
    double now() const;

    struct Impl;
    Impl& impl() { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace quench::net
