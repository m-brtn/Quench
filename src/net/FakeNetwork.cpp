// The fake backend (quench/net/FakeNetwork.h): canned responses by method and URL, delivered by the client's clock,
// and a record of what it received.

#include <quench/net/FakeNetwork.h>

#include "net/Backend.h"

#include <algorithm>
#include <format>
#include <map>
#include <utility>

namespace quench::net {

struct FakeNetwork::Impl {
    std::map<std::pair<std::string, std::string>, FakeResponse> responses;
    std::vector<FakeRecord> records;
    int open = 0;

    const FakeResponse* find(const std::string& method, const std::string& url) const {
        std::string bare = url.substr(0, url.find('?'));
        for (const std::string* m : {&method, &kAny}) {
            if (auto it = responses.find({*m, url}); it != responses.end()) return &it->second;
            if (auto it = responses.find({*m, bare}); it != responses.end()) return &it->second;
        }
        return nullptr;
    }
    static inline const std::string kAny = "*";
};

FakeResponse FakeResponse::text(int status, std::string body, Headers headers) {
    FakeResponse r;
    r.status = status;
    r.headers = std::move(headers);
    if (!body.empty()) r.body.push_back({0.0, std::move(body)});
    return r;
}

FakeResponse FakeResponse::failure(ErrorKind error, std::string message, double delay) {
    FakeResponse r;
    r.error = error;
    r.message = std::move(message);
    r.headerDelay = delay;
    return r;
}

FakeResponse FakeResponse::silence() {
    FakeResponse r;
    r.never = true;
    return r;
}

FakeResponse FakeResponse::redirect(int status, std::string location) {
    FakeResponse r;
    r.status = status;
    r.headers.push_back({"Location", std::move(location)});
    return r;
}

FakeNetwork::FakeNetwork() : m_impl(std::make_unique<Impl>()) {}
FakeNetwork::~FakeNetwork() = default;

void FakeNetwork::respond(std::string method, std::string url, FakeResponse response) {
    m_impl->responses[{std::move(method), std::move(url)}] = std::move(response);
}
void FakeNetwork::clearResponses() { m_impl->responses.clear(); }
const std::vector<FakeRecord>& FakeNetwork::requests() const { return m_impl->records; }
const FakeRecord* FakeNetwork::last() const { return m_impl->records.empty() ? nullptr : &m_impl->records.back(); }
void FakeNetwork::clearRequests() { m_impl->records.clear(); }
int FakeNetwork::open() const { return m_impl->open; }

namespace detail {
namespace {

// One request in flight: its timeline from `start` by the canned response.
struct Flight {
    RequestId id = 0;
    Request request;
    std::size_t record = 0; // index into FakeNetwork::Impl::records
    FakeResponse response;
    double start = 0.0;
    int redirects = 0;
    bool headersSent = false;
    std::size_t nextChunk = 0;
    double nextAt = 0.0; // when the next step is due
};

class FakeBackend final : public Backend {
public:
    explicit FakeBackend(FakeNetwork& fake) : m_fake(fake.impl()) {}
    ~FakeBackend() override {
        for (const Flight& f : m_flights) close(f, true);
    }

    const char* name() const override { return "fake"; }

    Result<void> start(RequestId id, const Request& request, bool allowInsecure, double now) override {
        m_allowInsecure = allowInsecure;
        Flight f;
        f.id = id;
        f.request = request;
        begin(f, now);
        ++m_fake.open;
        m_flights.push_back(std::move(f));
        return {};
    }

    void cancel(RequestId id) override {
        for (auto it = m_flights.begin(); it != m_flights.end(); ++it) {
            if (it->id != id) continue;
            close(*it, true);
            m_flights.erase(it);
            return;
        }
    }

    void poll(double now, std::vector<Event>& events) override {
        for (std::size_t i = 0; i < m_flights.size();) {
            if (step(m_flights[i], now, events)) {
                close(m_flights[i], false);
                m_flights.erase(m_flights.begin() + std::ptrdiff_t(i));
            } else {
                ++i;
            }
        }
    }

private:
    void begin(Flight& f, double now) {
        f.record = m_fake.records.size();
        m_fake.records.push_back({f.request, false});
        const FakeResponse* canned = m_fake.find(f.request.method, f.request.url);
        f.response = canned ? *canned
                            : FakeResponse::text(404, std::format("no fake response for {} {}", f.request.method,
                                                                  logUrl(f.request.url)));
        f.start = now;
        f.headersSent = false;
        f.nextChunk = 0;
        f.nextAt = now + f.response.headerDelay;
    }

    void close(const Flight& f, bool cancelled) {
        if (cancelled && f.record < m_fake.records.size()) m_fake.records[f.record].cancelled = true;
        --m_fake.open;
    }

    // Emits what is due; true when the request has ended.
    bool step(Flight& f, double now, std::vector<Event>& events) {
        const FakeResponse& r = f.response;
        if (r.never) return false;
        while (now >= f.nextAt) {
            if (!f.headersSent) {
                if (r.error != ErrorKind::None) {
                    events.push_back({.type = Event::Type::Failed, .id = f.id, .error = r.error, .message = r.message});
                    return true;
                }
                const std::string* location = findHeader(r.headers, "Location");
                if (r.status >= 300 && r.status < 400 && location) {
                    if (++f.redirects > kMaxRedirects) {
                        events.push_back({.type = Event::Type::Failed,
                                          .id = f.id,
                                          .error = ErrorKind::Failed,
                                          .message = std::format("more than {} redirects", kMaxRedirects)});
                        return true;
                    }
                    Result<Request> next = redirectRequest(f.request, r.status, *location, m_allowInsecure);
                    if (!next) {
                        events.push_back({.type = Event::Type::Failed,
                                          .id = f.id,
                                          .error = ErrorKind::NotAllowed,
                                          .message = next.error()});
                        return true;
                    }
                    f.request = std::move(*next);
                    begin(f, f.nextAt);
                    continue;
                }
                events.push_back({.type = Event::Type::Headers,
                                  .id = f.id,
                                  .status = r.status,
                                  .headers = r.headers,
                                  .finalUrl = f.request.url});
                f.headersSent = true;
                f.nextAt += r.body.empty() ? r.endDelay : r.body[0].delay;
                continue;
            }
            if (f.nextChunk < r.body.size()) {
                const std::string& bytes = r.body[f.nextChunk].bytes;
                events.push_back({.type = Event::Type::Data,
                                  .id = f.id,
                                  .bytes = std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
                ++f.nextChunk;
                f.nextAt += f.nextChunk < r.body.size() ? r.body[f.nextChunk].delay : r.endDelay;
                continue;
            }
            events.push_back({.type = Event::Type::Done, .id = f.id});
            return true;
        }
        return false;
    }

    FakeNetwork::Impl& m_fake;
    std::vector<Flight> m_flights;
    bool m_allowInsecure = false;
};

} // namespace

std::unique_ptr<Backend> createFakeBackend(FakeNetwork& fake) { return std::make_unique<FakeBackend>(fake); }

} // namespace detail
} // namespace quench::net
