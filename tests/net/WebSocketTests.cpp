// net::WebSocket with the fake server (every build, the Node.js one included): the URL rule, the handshake's
// answers, messages both ways in order by the fake's clock, scripted delays, closes by either side, timeouts, the
// message limit, pings, abort and a reconnect, and the log's redaction.

#include "Test.h"

#include <quench/core/Log.h>
#include <quench/net/FakeWebSocket.h>
#include <quench/net/WebSocket.h>

#include <string>
#include <vector>

using namespace quench;
using namespace quench::net;

namespace {

const std::string kUrl = "wss://api.example.com/v1/live";

struct Rig {
    FakeWebSocketServer server;
    double time = 0.0;
    WebSocket ws{WebSocketDesc{.fake = &server, .clock = [this] { return time; }}};

    void advance(double seconds) {
        time += seconds;
        ws.update();
    }
};

struct Seen {
    int opens = 0;
    std::vector<WsMessage> messages;
    std::vector<double> times;
    std::vector<double> pongs;
    int ends = 0;
    WsEnd end;

    WsHandlers handlers(const double* clock) {
        return {.onOpen = [this] { ++opens; },
                .onMessage =
                    [this, clock](const WsMessage& m) {
                        messages.push_back(m);
                        times.push_back(*clock);
                    },
                .onPong = [this](double rtt) { pongs.push_back(rtt); },
                .onEnd =
                    [this](const WsEnd& e) {
                        ++ends;
                        end = e;
                    }};
    }
};

struct LogCapture {
    std::string all;
    int id;
    LogCapture() {
        id = log::addSink([this](log::Level, std::string_view category, std::string_view message) {
            all += std::string(category) + ": " + std::string(message) + "\n";
        });
    }
    ~LogCapture() { log::removeSink(id); }
};

} // namespace

QUENCH_TEST(ws_url_rule) {
    CHECK(checkWebSocketUrl("wss://api.openai.com/v1/live/sessions"), "wss");
    CHECK(checkWebSocketUrl("ws://127.0.0.1:8080/ws"), "ws to the loopback address");
    CHECK(checkWebSocketUrl("ws://localhost/ws"), "ws to localhost");
    CHECK(checkWebSocketUrl("ws://[::1]:9000/"), "ws to [::1]");
    CHECK(!checkWebSocketUrl("ws://example.com/ws"), "plain ws elsewhere");
    CHECK(checkWebSocketUrl("ws://example.com/ws", true), "allowInsecure");
    CHECK(!checkWebSocketUrl("https://example.com/"), "https is not a WebSocket URL");
    CHECK(!checkWebSocketUrl("wss://user:pass@example.com/"), "credentials in the URL");
    CHECK(!checkWebSocketUrl("not a url"), "not a URL");
}

QUENCH_TEST(ws_refused_at_connect) {
    Rig rig;
    Seen seen;
    CHECK(!rig.ws.connect({.url = "ws://example.com/"}, seen.handlers(&rig.time)), "the rule");
    CHECK(!rig.ws.connect({.url = kUrl, .headers = {{"Authorization", "a\r\nb"}}}, seen.handlers(&rig.time)),
          "a line break in a value");
    CHECK(!rig.ws.connect({.url = kUrl, .headers = {{"Upgrade", "h2c"}}}, seen.handlers(&rig.time)),
          "a handshake header");
    CHECK(!rig.ws.connect({.url = kUrl, .protocols = {"two words"}}, seen.handlers(&rig.time)), "a bad protocol");
    CHECK(rig.ws.state() == WsState::Idle, "still idle");
    rig.advance(0.1);
    CHECK(seen.ends == 0, "no handler for a refused connect");
    CHECK(!rig.ws.sendText("x"), "nothing to send to");
}

QUENCH_TEST(ws_open_and_messages) {
    Rig rig;
    rig.server.answer(kUrl, {.delay = 0.2, .protocol = "quench.test"});
    rig.server.onMessage([](FakeWsPeer& peer, const WsMessage& m) { peer.sendText("echo:" + m.data, 0.05); });
    Seen seen;
    REQUIRE(rig.ws.connect({.url = kUrl, .headers = {{"Authorization", "Bearer k"}}, .protocols = {"quench.test"}},
                           seen.handlers(&rig.time)),
            "connect");
    CHECK(rig.ws.state() == WsState::Connecting && rig.ws.active(), "connecting");
    rig.advance(0.1);
    CHECK(seen.opens == 0, "not before the answer's delay");
    rig.advance(0.1);
    REQUIRE(seen.opens == 1 && rig.ws.open(), "open after 0.2 s");
    CHECK(rig.ws.protocol() == "quench.test", "the protocol chosen");
    REQUIRE(rig.server.last(), "a peer");
    const FakeWsPeer& peer = *rig.server.last();
    CHECK(peer.url() == kUrl && peer.headers().size() == 1 && peer.protocols().size() == 1, "the upgrade request");
    CHECK(rig.ws.sendText("a") && rig.ws.sendText("b"), "send");
    const std::uint8_t bytes[] = {1, 2, 3};
    CHECK(rig.ws.sendBinary(bytes), "binary");
    rig.advance(0.01);
    REQUIRE(peer.received().size() == 3, "the server got three: %zu", peer.received().size());
    CHECK(peer.received()[2].kind == WsKind::Binary && peer.received()[2].data.size() == 3, "binary as binary");
    CHECK(seen.messages.empty(), "the echoes wait 50 ms each");
    rig.advance(0.05);
    CHECK(seen.messages.size() == 1, "the first echo after 50 ms: %zu", seen.messages.size());
    rig.advance(0.05);
    rig.advance(0.05);
    REQUIRE(seen.messages.size() == 3, "all three: %zu", seen.messages.size());
    CHECK(seen.messages[0].data == "echo:a" && seen.messages[1].data == "echo:b", "in order");
    CHECK(rig.server.connected() == 1, "one open connection");
}

QUENCH_TEST(ws_scripted_delays_chain) {
    Rig rig;
    rig.server.answer(kUrl);
    rig.server.onOpen([](FakeWsPeer& peer) {
        peer.sendText("one");
        peer.sendText("two", 0.3);
        peer.sendText("three", 0.2);
        peer.close(4000, "done", 0.1);
    });
    Seen seen;
    REQUIRE(rig.ws.connect({.url = kUrl}, seen.handlers(&rig.time)), "connect");
    for (int i = 0; i < 100 && seen.ends == 0; ++i) rig.advance(0.01);
    REQUIRE(seen.messages.size() == 3 && seen.ends == 1, "three and the end");
    CHECK(seen.times[1] - seen.times[0] > 0.29 && seen.times[1] - seen.times[0] < 0.32, "0.3 s apart: %.3f",
          seen.times[1] - seen.times[0]);
    CHECK(seen.times[2] - seen.times[1] > 0.19 && seen.times[2] - seen.times[1] < 0.22, "0.2 s after: %.3f",
          seen.times[2] - seen.times[1]);
    CHECK(seen.end.clean() && seen.end.byServer && seen.end.closeCode == 4000 && seen.end.closeReason == "done",
          "the server's close");
    CHECK(rig.ws.state() == WsState::Closed && rig.server.connected() == 0, "closed");
    CHECK(seen.end.messagesIn == 3 && seen.end.bytesIn == 11, "counted: %llu, %llu bytes",
          (unsigned long long)seen.end.messagesIn, (unsigned long long)seen.end.bytesIn);
}

QUENCH_TEST(ws_client_close) {
    Rig rig;
    rig.server.answer(kUrl);
    Seen seen;
    REQUIRE(rig.ws.connect({.url = kUrl}, seen.handlers(&rig.time)), "connect");
    rig.advance(0.01);
    REQUIRE(rig.ws.open(), "open");
    rig.ws.close(kWsNormalClosure, "bye");
    CHECK(rig.ws.state() == WsState::Closing, "closing");
    CHECK(!rig.ws.sendText("late"), "nothing goes out while closing");
    rig.advance(0.01);
    REQUIRE(seen.ends == 1, "ended");
    CHECK(seen.end.clean() && !seen.end.byServer && seen.end.closeCode == 1000, "a clean close by the client");
    const FakeWsPeer& peer = *rig.server.last();
    CHECK(peer.clientCloseCode() && *peer.clientCloseCode() == 1000 && peer.clientCloseReason() == "bye",
          "the server saw the close frame");
}

QUENCH_TEST(ws_close_timeout) {
    Rig rig;
    rig.server.answer(kUrl);
    rig.server.onOpen([](FakeWsPeer& peer) { peer.setAnswerClose(false); });
    Seen seen;
    REQUIRE(rig.ws.connect({.url = kUrl, .closeTimeout = 2.0}, seen.handlers(&rig.time)), "connect");
    rig.advance(0.01);
    rig.ws.close(4001, "idle");
    rig.advance(1.0);
    CHECK(seen.ends == 0, "waits for the server's close");
    rig.advance(1.05);
    REQUIRE(seen.ends == 1, "ends after the close timeout");
    CHECK(seen.end.clean() && seen.end.closeCode == 4001 && !seen.end.message.empty(), "still a close: '%s'",
          seen.end.message.c_str());
    CHECK(rig.server.last()->aborted(), "the connection was dropped");
}

QUENCH_TEST(ws_server_failures) {
    {
        Rig rig;
        rig.server.answer(kUrl, {.status = 401, .message = "invalid key"});
        Seen seen;
        REQUIRE(rig.ws.connect({.url = kUrl}, seen.handlers(&rig.time)), "connect");
        rig.advance(0.01);
        CHECK(seen.ends == 1 && seen.opens == 0 && seen.end.status == 401 && !seen.end.clean(), "refused with 401");
        CHECK(rig.ws.state() == WsState::Failed, "failed");
    }
    {
        Rig rig;
        Seen seen;
        REQUIRE(rig.ws.connect({.url = kUrl}, seen.handlers(&rig.time)), "connect");
        rig.advance(0.01);
        CHECK(seen.end.status == 404, "no server answers 404: %d", seen.end.status);
    }
    {
        Rig rig;
        rig.server.answer(kUrl, {.delay = 0.1, .error = ErrorKind::Tls, .message = "bad certificate"});
        Seen seen;
        REQUIRE(rig.ws.connect({.url = kUrl}, seen.handlers(&rig.time)), "connect");
        rig.advance(0.2);
        CHECK(seen.end.error == ErrorKind::Tls && seen.end.message == "bad certificate", "TLS");
    }
    {
        Rig rig;
        rig.server.answer(kUrl);
        rig.server.onOpen([](FakeWsPeer& peer) {
            peer.sendText("hi");
            peer.fail(ErrorKind::NoNetwork, "connection lost", 0.5);
        });
        Seen seen;
        REQUIRE(rig.ws.connect({.url = kUrl}, seen.handlers(&rig.time)), "connect");
        for (int i = 0; i < 100 && seen.ends == 0; ++i) rig.advance(0.01);
        CHECK(seen.messages.size() == 1 && seen.end.error == ErrorKind::NoNetwork && seen.end.byServer, "lost");
    }
    {
        Rig rig;
        rig.server.answer(kUrl, {.never = true});
        Seen seen;
        REQUIRE(rig.ws.connect({.url = kUrl, .connectTimeout = 3.0}, seen.handlers(&rig.time)), "connect");
        rig.advance(2.9);
        CHECK(seen.ends == 0, "waits");
        rig.advance(0.2);
        CHECK(seen.end.error == ErrorKind::Timeout, "a connect timeout: %s", errorKindName(seen.end.error));
    }
}

QUENCH_TEST(ws_message_limit) {
    Rig rig;
    rig.server.answer(kUrl);
    rig.server.onOpen([](FakeWsPeer& peer) {
        peer.sendText(std::string(100, 'a'));
        peer.sendText(std::string(101, 'b'));
        peer.sendText("never");
    });
    Seen seen;
    REQUIRE(rig.ws.connect({.url = kUrl, .maxMessageBytes = 100}, seen.handlers(&rig.time)), "connect");
    rig.advance(0.01);
    CHECK(seen.messages.size() == 1, "the one at the limit: %zu", seen.messages.size());
    CHECK(seen.end.error == ErrorKind::TooLarge && seen.end.closeCode == kWsMessageTooBig, "TooLarge, 1009");
}

QUENCH_TEST(ws_ping_pong) {
    Rig rig;
    rig.server.answer(kUrl);
    Seen seen;
    REQUIRE(rig.ws.connect({.url = kUrl, .pongTimeout = 1.0}, seen.handlers(&rig.time)), "connect");
    rig.advance(0.01);
    CHECK(rig.ws.ping(), "ping");
    rig.advance(0.03);
    CHECK(seen.pongs.size() == 1 && rig.server.last()->pings() == 1, "a pong");
    rig.server.last()->setAnswerPings(false);
    CHECK(rig.ws.ping(), "ping");
    rig.advance(0.5);
    CHECK(seen.ends == 0, "waits for the pong");
    rig.advance(0.6);
    CHECK(seen.end.error == ErrorKind::Timeout, "no pong: %s", errorKindName(seen.end.error));
}

QUENCH_TEST(ws_abort_and_reconnect) {
    Rig rig;
    rig.server.answer(kUrl);
    Seen first, second;
    REQUIRE(rig.ws.connect({.url = kUrl}, first.handlers(&rig.time)), "connect");
    rig.advance(0.01);
    CHECK(!rig.ws.connect({.url = kUrl}, second.handlers(&rig.time)), "one connection at a time");
    rig.ws.abort();
    CHECK(rig.ws.state() == WsState::Failed, "failed at once");
    CHECK(!rig.ws.connect({.url = kUrl}, second.handlers(&rig.time)), "not before the end is delivered");
    rig.advance(0.01);
    CHECK(first.ends == 1 && first.end.error == ErrorKind::Cancelled, "Cancelled");
    CHECK(rig.server.peers()[0]->aborted(), "the server saw the connection drop");
    REQUIRE(rig.ws.connect({.url = kUrl}, second.handlers(&rig.time)), "a new connection");
    rig.advance(0.01);
    CHECK(second.opens == 1 && rig.server.peers().size() == 2, "the second one is open");

    rig.server.last()->close(1001, "going away");
    rig.advance(0.01);
    CHECK(second.ends == 1 && second.end.closeCode == 1001, "the server closed it");

    // A connect from onEnd.
    Seen third;
    bool reconnected = false;
    WsHandlers again = third.handlers(&rig.time);
    again.onEnd = [&](const WsEnd&) { reconnected = bool(rig.ws.connect({.url = kUrl}, third.handlers(&rig.time))); };
    REQUIRE(rig.ws.connect({.url = kUrl}, again), "third");
    rig.advance(0.01);
    rig.server.last()->close();
    rig.advance(0.01);
    CHECK(reconnected && rig.ws.active(), "connected again from onEnd");
}

QUENCH_TEST(ws_destructor_calls_nothing) {
    FakeWebSocketServer server;
    server.answer(kUrl);
    double time = 0.0;
    Seen seen;
    {
        WebSocket ws{WebSocketDesc{.fake = &server, .clock = [&] { return time; }}};
        REQUIRE(ws.connect({.url = kUrl}, seen.handlers(&time)), "connect");
        ws.update();
    }
    CHECK(seen.opens == 1 && seen.ends == 0, "no onEnd from the destructor");
    CHECK(server.last()->aborted() && server.connected() == 0, "dropped");
}

QUENCH_TEST(ws_log_redaction) {
    LogCapture log;
    Rig rig;
    const std::string url = kUrl + "?token=secretquery";
    rig.server.answer(url);
    rig.server.onOpen([](FakeWsPeer& peer) { peer.sendText("message-body-text"); });
    Seen seen;
    REQUIRE(rig.ws.connect({.url = url, .headers = {{"Authorization", "Bearer sk-live-secret-value"}}},
                           seen.handlers(&rig.time)),
            "connect");
    rig.advance(0.01);
    CHECK(rig.ws.sendText("client-message-text"), "send");
    rig.ws.close();
    rig.advance(0.01);
    CHECK(seen.ends == 1, "ended");
    CHECK(log.all.find("api.example.com/v1/live") != std::string::npos, "the host and path are logged");
    CHECK(log.all.find("sk-live-secret-value") == std::string::npos, "no header value");
    CHECK(log.all.find("secretquery") == std::string::npos, "no query string");
    CHECK(log.all.find("message-body-text") == std::string::npos &&
              log.all.find("client-message-text") == std::string::npos,
          "no message");
}

QUENCH_TEST(ws_none_backend) {
    WebSocket ws{WebSocketDesc{.noBackend = true}};
    CHECK(!ws.available() && std::string(ws.backendName()) == "none", "none");
    CHECK(!ws.connect({.url = kUrl}), "connect fails at once");
}
