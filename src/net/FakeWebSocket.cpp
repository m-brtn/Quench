// The fake WebSocket server and its backend (quench/net/FakeWebSocket.h): upgrades answered by URL, the server's side
// of each connection scripted by the test, delivered by the client's clock.

#include <quench/net/FakeWebSocket.h>

#include "net/WsBackend.h"

#include <algorithm>
#include <map>
#include <utility>

namespace quench::net {

struct FakeWebSocketServer::Impl {
    std::map<std::string, FakeWsAnswer> answers;
    std::function<void(FakeWsPeer&)> onOpen;
    std::function<void(FakeWsPeer&, const WsMessage&)> onMessage;
    std::function<void(FakeWsPeer&)> onClose;
    std::vector<std::unique_ptr<FakeWsPeer>> peers;
    double now = 0.0;

    const FakeWsAnswer* find(const std::string& url) const {
        if (auto it = answers.find(url); it != answers.end()) return &it->second;
        if (auto it = answers.find(url.substr(0, url.find('?'))); it != answers.end()) return &it->second;
        return nullptr;
    }
};

struct FakeWsPeer::Impl {
    struct Action {
        enum class Type : std::uint8_t { Message, Pong, Close, Fail };
        Type type = Type::Message;
        double due = 0.0;
        WsMessage message;
        int code = 0;
        std::string text; // the close reason or the failure's message
        ErrorKind error = ErrorKind::Failed;
    };

    FakeWebSocketServer::Impl* server = nullptr;
    std::string url;
    Headers headers;
    std::vector<std::string> protocols;
    std::vector<WsMessage> received;
    int pings = 0;
    std::optional<int> clientCode;
    std::string clientReason;
    bool connected = false;
    bool aborted = false;
    bool answerClose = true;
    bool answerPings = true;
    std::vector<Action> actions; // in the order scheduled; due times never decrease
    double lastDue = 0.0;

    void schedule(Action action, double delay) {
        const double base = actions.empty() ? server->now : std::max(lastDue, server->now);
        action.due = base + std::max(0.0, delay);
        lastDue = action.due;
        actions.push_back(std::move(action));
    }
};

FakeWsPeer::FakeWsPeer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
FakeWsPeer::~FakeWsPeer() = default;

const std::string& FakeWsPeer::url() const { return m_impl->url; }
const Headers& FakeWsPeer::headers() const { return m_impl->headers; }
const std::vector<std::string>& FakeWsPeer::protocols() const { return m_impl->protocols; }
const std::vector<WsMessage>& FakeWsPeer::received() const { return m_impl->received; }
int FakeWsPeer::pings() const { return m_impl->pings; }
std::optional<int> FakeWsPeer::clientCloseCode() const { return m_impl->clientCode; }
const std::string& FakeWsPeer::clientCloseReason() const { return m_impl->clientReason; }
bool FakeWsPeer::connected() const { return m_impl->connected; }
bool FakeWsPeer::aborted() const { return m_impl->aborted; }
void FakeWsPeer::setAnswerClose(bool answer) { m_impl->answerClose = answer; }
void FakeWsPeer::setAnswerPings(bool answer) { m_impl->answerPings = answer; }

void FakeWsPeer::sendText(std::string text, double delay) {
    m_impl->schedule({.type = Impl::Action::Type::Message, .message = {WsKind::Text, std::move(text)}}, delay);
}

void FakeWsPeer::sendBinary(std::string bytes, double delay) {
    m_impl->schedule({.type = Impl::Action::Type::Message, .message = {WsKind::Binary, std::move(bytes)}}, delay);
}

void FakeWsPeer::close(int code, std::string reason, double delay) {
    m_impl->schedule({.type = Impl::Action::Type::Close, .code = code, .text = std::move(reason)}, delay);
}

void FakeWsPeer::fail(ErrorKind error, std::string message, double delay) {
    m_impl->schedule({.type = Impl::Action::Type::Fail, .text = std::move(message), .error = error}, delay);
}

FakeWebSocketServer::FakeWebSocketServer() : m_impl(std::make_unique<Impl>()) {}
FakeWebSocketServer::~FakeWebSocketServer() = default;

void FakeWebSocketServer::answer(std::string url, FakeWsAnswer answer) {
    m_impl->answers[std::move(url)] = std::move(answer);
}
void FakeWebSocketServer::onOpen(std::function<void(FakeWsPeer&)> handler) { m_impl->onOpen = std::move(handler); }
void FakeWebSocketServer::onMessage(std::function<void(FakeWsPeer&, const WsMessage&)> handler) {
    m_impl->onMessage = std::move(handler);
}
void FakeWebSocketServer::onClose(std::function<void(FakeWsPeer&)> handler) { m_impl->onClose = std::move(handler); }

std::vector<FakeWsPeer*> FakeWebSocketServer::peers() const {
    std::vector<FakeWsPeer*> out;
    for (const auto& p : m_impl->peers) out.push_back(p.get());
    return out;
}

FakeWsPeer* FakeWebSocketServer::last() const { return m_impl->peers.empty() ? nullptr : m_impl->peers.back().get(); }

int FakeWebSocketServer::connected() const {
    return int(std::count_if(m_impl->peers.begin(), m_impl->peers.end(), [](const auto& p) { return p->connected(); }));
}

double FakeWebSocketServer::now() const { return m_impl->now; }

namespace detail {
namespace {

class FakeWsBackend final : public WsBackend {
public:
    explicit FakeWsBackend(FakeWebSocketServer& server) : m_server(server.impl()) {}
    ~FakeWsBackend() override { abort(); }

    const char* name() const override { return "fake"; }

    Result<void> connect(const WsConnect& request, bool, double now) override {
        abort();
        m_server.now = std::max(m_server.now, now);
        m_request = request;
        m_connecting = true;
        const FakeWsAnswer* answer = m_server.find(request.url);
        m_answer = answer ? *answer : FakeWsAnswer{.status = 404};
        m_due = now + m_answer.delay;
        return {};
    }

    void send(WsMessage message) override { m_outbox.push_back({Out::Type::Message, std::move(message), 0, {}}); }
    void ping() override { m_outbox.push_back({Out::Type::Ping, {}, 0, {}}); }
    void close(int code, const std::string& reason) override {
        m_outbox.push_back({Out::Type::Close, {}, code, reason});
    }

    void abort() override {
        m_connecting = false;
        m_outbox.clear();
        if (m_peer && m_peer->connected) {
            m_peer->connected = false;
            m_peer->aborted = true;
            m_peer->actions.clear();
            if (m_server.onClose) m_server.onClose(*m_peerObject);
        }
        m_peer = nullptr;
        m_peerObject = nullptr;
    }

    void poll(double now, std::vector<WsEvent>& events) override {
        m_server.now = std::max(m_server.now, now);
        if (m_connecting && !m_answer.never && now >= m_due) {
            m_connecting = false;
            if (m_answer.error != ErrorKind::None) {
                events.push_back({.type = WsEvent::Type::Failed, .error = m_answer.error, .text = m_answer.message});
            } else if (m_answer.status != 101) {
                events.push_back({.type = WsEvent::Type::Failed,
                                  .error = ErrorKind::Failed,
                                  .text = m_answer.status == 404 && m_answer.message.empty()
                                              ? std::string("no fake WebSocket server")
                                              : "the server refused the upgrade" +
                                                    (m_answer.message.empty() ? "" : ": " + m_answer.message),
                                  .status = m_answer.status});
            } else {
                auto impl = std::make_unique<FakeWsPeer::Impl>();
                impl->server = &m_server;
                impl->url = m_request.url;
                impl->headers = m_request.headers;
                impl->protocols = m_request.protocols;
                impl->connected = true;
                m_peer = impl.get();
                m_server.peers.push_back(std::make_unique<FakeWsPeer>(std::move(impl)));
                m_peerObject = m_server.peers.back().get();
                if (m_server.onOpen) m_server.onOpen(*m_peerObject);
                events.push_back({.type = WsEvent::Type::Open, .protocol = m_answer.protocol});
            }
        }
        if (!m_peer) return;

        // What the client sent reaches the server.
        std::vector<Out> outbox = std::move(m_outbox);
        m_outbox.clear();
        for (Out& out : outbox) {
            if (!m_peer || !m_peer->connected) break;
            switch (out.type) {
            case Out::Type::Message:
                m_peer->received.push_back(out.message);
                if (m_server.onMessage) m_server.onMessage(*m_peerObject, out.message);
                break;
            case Out::Type::Ping:
                m_peer->pings += 1;
                if (m_peer->answerPings) m_peer->schedule({.type = FakeWsPeer::Impl::Action::Type::Pong}, 0.0);
                break;
            case Out::Type::Close:
                m_peer->clientCode = out.code;
                m_peer->clientReason = out.reason;
                if (m_server.onClose) m_server.onClose(*m_peerObject);
                if (m_peer->answerClose) {
                    // The answer comes before anything still scheduled: a server stops sending once closed.
                    m_peer->actions.clear();
                    m_peer->schedule(
                        {.type = FakeWsPeer::Impl::Action::Type::Close, .code = out.code, .text = out.reason}, 0.0);
                }
                break;
            }
        }

        // The server's scheduled actions that are due.
        std::size_t done = 0;
        auto& actions = m_peer->actions;
        while (done < actions.size() && actions[done].due <= now && m_peer->connected) {
            FakeWsPeer::Impl::Action& a = actions[done++];
            switch (a.type) {
            case FakeWsPeer::Impl::Action::Type::Message:
                events.push_back({.type = WsEvent::Type::Message, .message = std::move(a.message)});
                break;
            case FakeWsPeer::Impl::Action::Type::Pong: events.push_back({.type = WsEvent::Type::Pong}); break;
            case FakeWsPeer::Impl::Action::Type::Close:
                m_peer->connected = false;
                events.push_back({.type = WsEvent::Type::Closed, .code = a.code, .reason = a.text});
                break;
            case FakeWsPeer::Impl::Action::Type::Fail:
                m_peer->connected = false;
                events.push_back({.type = WsEvent::Type::Failed, .error = a.error, .text = a.text});
                break;
            }
        }
        if (!m_peer->connected) {
            actions.clear();
            m_peer = nullptr;
            m_peerObject = nullptr;
        } else {
            actions.erase(actions.begin(), actions.begin() + std::ptrdiff_t(done));
        }
    }

private:
    struct Out {
        enum class Type : std::uint8_t { Message, Ping, Close };
        Type type = Type::Message;
        WsMessage message;
        int code = 0;
        std::string reason;
    };

    FakeWebSocketServer::Impl& m_server;
    WsConnect m_request;
    FakeWsAnswer m_answer;
    bool m_connecting = false;
    double m_due = 0.0;
    FakeWsPeer::Impl* m_peer = nullptr;
    FakeWsPeer* m_peerObject = nullptr;
    std::vector<Out> m_outbox;
};

class NoneWsBackend final : public WsBackend {
public:
    const char* name() const override { return "none"; }
    bool available() const override { return false; }
    Result<void> connect(const WsConnect&, bool, double) override {
        return Error{"no WebSocket backend on this platform"};
    }
    void send(WsMessage) override {}
    void ping() override {}
    void close(int, const std::string&) override {}
    void abort() override {}
    void poll(double, std::vector<WsEvent>&) override {}
};

} // namespace

std::unique_ptr<WsBackend> createFakeWsBackend(FakeWebSocketServer& server) {
    return std::make_unique<FakeWsBackend>(server);
}

std::unique_ptr<WsBackend> createNoneWsBackend() { return std::make_unique<NoneWsBackend>(); }

} // namespace detail
} // namespace quench::net
