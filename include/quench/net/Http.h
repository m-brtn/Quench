#pragma once

#include <quench/core/Result.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Asynchronous HTTPS requests (docs/net.md). A Client sends requests through the platform's backend (NSURLSession on
// macOS and iOS, the browser's fetch on the web, none elsewhere yet) or through a FakeNetwork (quench/net/
// FakeNetwork.h) in tests. Nothing blocks: send() starts a request and returns, and the client's update(), called
// once a frame on the main thread, calls the request's handlers there: its headers, its body's chunks as they arrive
// (Request::streamBody) and its end, exactly once per sent request.
//
// Rules built in: HTTPS only, except http://localhost, http://127.0.0.1 and http://[::1] (ClientDesc::allowInsecure
// lifts it for tests); no credentials in a URL; the log shows the method, the host and path, the status, the bytes
// and the milliseconds, never a header's value or the query string; a redirect to another origin drops the
// Authorization, Cookie and key headers (isSecretHeader); no cookies are stored or sent from a store; the response
// size limit is enforced while the body arrives.
//
// A Client is used from the thread that created it (the main thread). It must not be destroyed inside one of its
// handlers.
namespace quench::net {

struct Header {
    std::string name;
    std::string value;
};
using Headers = std::vector<Header>;

// The value of the first header of that name (ASCII case-insensitive), or nullptr.
const std::string* findHeader(const Headers& headers, std::string_view name);

struct Request {
    std::string method = "GET";
    std::string url;
    Headers headers;
    std::vector<std::uint8_t> body;
    double timeoutSeconds = 30.0;             // from send() to the end of the body; 0: no limit
    std::size_t maxResponseBytes = 16u << 20; // a body over this fails the request (ErrorKind::TooLarge)
    // True: the body goes to Handlers::onChunk as it arrives and Response::body stays empty (server-sent events,
    // streamed audio). False: the whole body is in Response::body at the end.
    bool streamBody = false;

    void setBody(std::string_view text);
};

enum class ErrorKind : std::uint8_t {
    None,
    InvalidRequest, // a malformed URL, method or header
    NotAllowed,     // the HTTPS rule refused the URL or a redirect's target
    NoBackend,      // reserved: on a platform without a backend send() fails at once instead
    NoNetwork,      // offline, the host not found, the connection refused or lost
    Timeout,        // Request::timeoutSeconds passed, or the backend's own timeout
    Cancelled,      // Client::cancel
    Tls,            // the secure connection failed (certificate, protocol)
    TooLarge,       // the body is over Request::maxResponseBytes
    Failed,         // anything else the backend reports (the browser's fetch cannot say more)
};
const char* errorKindName(ErrorKind kind);

struct Response {
    int status = 0; // the HTTP status; 0 when no answer came
    Headers headers;
    std::vector<std::uint8_t> body; // empty for Request::streamBody
    std::size_t bytes = 0;          // body bytes received
    double seconds = 0.0;           // from send() to the end
    ErrorKind error = ErrorKind::None;
    std::string message; // why it failed, readable; without the URL's query string

    // The transport worked (any status, 404 included).
    bool ok() const { return error == ErrorKind::None; }
    // ok() and a 2xx status.
    bool success() const { return ok() && status >= 200 && status < 300; }
    const std::string* header(std::string_view name) const { return findHeader(headers, name); }
    std::string_view text() const { return {reinterpret_cast<const char*>(body.data()), body.size()}; }
};

using RequestId = std::uint64_t;

// Called on the main thread from Client::update() only.
struct Handlers {
    // The status and the headers, before the body (Response::body is empty, error None).
    std::function<void(const Response& head)> onHeaders;
    // A piece of the body as it arrived, in order (Request::streamBody only).
    std::function<void(std::span<const std::uint8_t> chunk)> onChunk;
    // The end: success, an HTTP error status or an error. Once per sent request, also after cancel().
    std::function<void(const Response& response)> onDone;
};

class FakeNetwork;

struct ClientDesc {
    FakeNetwork* fake = nullptr;   // send through this fake instead of the platform's backend (it outlives the client)
    bool noBackend = false;        // send through no backend: every send fails as on a platform without one
    bool allowInsecure = false;    // plain http to any host (tests against a server elsewhere)
    std::function<double()> clock; // seconds for timeouts and Response::seconds; default: a steady clock
};

namespace detail {
class Backend;
}

class Client {
public:
    explicit Client(const ClientDesc& desc = {});
    // Cancels what is still running, without calling its handlers.
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Starts a request. Fails at once (no handler is called) for a URL the rules refuse, a malformed request or a
    // platform without a backend; otherwise the handlers are called from update().
    Result<RequestId> send(Request request, Handlers handlers = {});
    // Stops a request: its onDone gets ErrorKind::Cancelled from the next update() (from the running one inside a
    // handler); nothing else of it is delivered. Unknown or finished ids are ignored.
    void cancel(RequestId id);
    void cancelAll();

    // Once a frame on the main thread: delivers what arrived, ends the requests past their timeout.
    void update();

    bool active(RequestId id) const;
    int activeCount() const;
    // "nsurlsession", "fetch", "fake" or "none".
    const char* backendName() const;
    // False for the "none" backend.
    bool available() const;

private:
    struct Entry;
    void finish(Entry& entry, ErrorKind error, std::string message);
    void deliver(Entry& entry);
    double now() const;

    ClientDesc m_desc;
    std::unique_ptr<detail::Backend> m_backend;
    std::vector<std::unique_ptr<Entry>> m_entries; // in send order
    std::vector<RequestId> m_finished;             // to deliver in the running or next update()
    RequestId m_nextId = 1;
    bool m_updating = false;
};

// The HTTPS rule: an absolute https:// URL, or http:// to localhost, 127.0.0.1 or [::1] (any http:// host with
// allowInsecure), without user:password@.
Result<void> checkUrl(std::string_view url, bool allowInsecure = false);
// "host[:port]/path" without the scheme, the query string and the fragment: what the log shows of a URL.
std::string logUrl(std::string_view url);
// Authorization, Proxy-Authorization, Cookie, Set-Cookie and names with "key", "token", "secret", "password" or
// "session" in them (X-API-Key, api-key, X-Goog-Api-Key, X-Auth-Token ...): dropped on a redirect to another origin.
bool isSecretHeader(std::string_view name);
// The origin of the web page the program runs in ("http://127.0.0.1:8000"), empty outside a browser.
std::string pageOrigin();

} // namespace quench::net
