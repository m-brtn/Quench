#pragma once

#include <quench/net/Http.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A programmable network for tests of quench_net and of the modules built on it (docs/net.md, "The fake"): a
// Client made with ClientDesc::fake sends to it instead of the platform's backend, through the same rules, limits,
// timeouts and log. Canned responses are found by method and URL; time is the client's clock (ClientDesc::clock), so
// a test with a manual clock decides when each piece arrives.
namespace quench::net {

// A piece of a canned body: its bytes arrive `delay` seconds after the previous piece (or the headers).
struct FakeChunk {
    double delay = 0.0;
    std::string bytes;
};

struct FakeResponse {
    int status = 200;
    Headers headers;
    std::vector<FakeChunk> body;
    double headerDelay = 0.0;          // seconds from the send to the headers (or to the error)
    double endDelay = 0.0;             // seconds from the last piece to the end
    ErrorKind error = ErrorKind::None; // fail after headerDelay instead of answering
    std::string message;               // the error's message
    bool never = false;                // never answers (a request for timeouts and cancels)

    // A whole body at once.
    static FakeResponse text(int status, std::string body, Headers headers = {});
    static FakeResponse failure(ErrorKind error, std::string message, double delay = 0.0);
    static FakeResponse silence();
    // A 3xx with a Location (absolute or a path on the same origin); the fake follows it like a browser.
    static FakeResponse redirect(int status, std::string location);
};

// What the fake received: every request as sent, also those of followed redirects (with the headers a redirect left).
struct FakeRecord {
    Request request;
    bool cancelled = false;
};

class FakeNetwork {
public:
    FakeNetwork();
    ~FakeNetwork();
    FakeNetwork(const FakeNetwork&) = delete;
    FakeNetwork& operator=(const FakeNetwork&) = delete;

    // The answer to `method` and `url` (an exact URL, or one without its query string; method "*" for any). A later
    // call for the same pair replaces it. Requests without one get 404 with "no fake response".
    void respond(std::string method, std::string url, FakeResponse response);
    void clearResponses();

    const std::vector<FakeRecord>& requests() const;
    // The last request received, or nullptr.
    const FakeRecord* last() const;
    void clearRequests();
    // Requests started and not yet ended or cancelled.
    int open() const;

    struct Impl;
    Impl& impl() { return *m_impl; }

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace quench::net
