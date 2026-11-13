#pragma once

#include <quench/net/Http.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The backend interface of quench_net and the helpers the backends share. One platform backend is built per platform
// (CMakeLists.txt picks its file): AppleBackend.cpp with AppleBackend.m (NSURLSession), WebBackend.cpp (fetch) or
// NoneBackend.cpp. A new one (libcurl, WinHTTP) is one more file that defines createPlatformBackend() and
// platformPageOrigin(). The fake (FakeNetwork.cpp) is built everywhere.
namespace quench::net::detail {

struct Event {
    enum class Type : std::uint8_t { Headers, Data, Done, Failed };
    Type type = Type::Done;
    RequestId id = 0;
    int status = 0;                      // Headers
    Headers headers;                     // Headers
    std::string finalUrl;                // Headers: the URL that answered, after redirects (empty: the request's)
    std::vector<std::uint8_t> bytes;     // Data
    ErrorKind error = ErrorKind::Failed; // Failed
    std::string message;                 // Failed
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;
    virtual bool available() const { return true; }
    // Starts a checked request (the URL passed checkUrl). Events come from poll(); an error here fails the send.
    virtual Result<void> start(RequestId id, const Request& request, bool allowInsecure, double now) = 0;
    // Stops a request; its later events, if any, are ignored by the client.
    virtual void cancel(RequestId id) = 0;
    // Appends what arrived since the last call, in order per request. Main thread.
    virtual void poll(double now, std::vector<Event>& events) = 0;
};

std::unique_ptr<Backend> createPlatformBackend();
std::unique_ptr<Backend> createNoneBackend();
std::unique_ptr<Backend> createFakeBackend(FakeNetwork& fake);
std::string platformPageOrigin();

// ---------------------------------------------------------------- URLs and rules (Rules.cpp)

struct Url {
    std::string scheme; // lower case
    std::string host;   // lower case; IPv6 in brackets ("[::1]")
    int port = -1;      // -1: the scheme's default
    std::string path;   // from '/' to before '?' or '#'; "/" when empty
    std::string query;  // after '?', before '#'
    bool userInfo = false;

    int effectivePort() const;
    // scheme://host[:port], the port only when it is not the scheme's default.
    std::string origin() const;
};
std::optional<Url> parseUrl(std::string_view url);
// `location` (absolute, or a path) resolved against `base`.
std::optional<std::string> resolveUrl(std::string_view base, std::string_view location);
bool isLoopbackHost(std::string_view host);

// Policy for a redirect from `from` to `to`: refused (an error), or followed with the secret headers dropped when the
// origin changes.
enum class RedirectHeaders : std::uint8_t { Keep, DropSecrets };
Result<RedirectHeaders> checkRedirect(std::string_view from, std::string_view to, bool allowInsecure);
// The request a redirect makes: the URL, the method (303, and 301 or 302 after a POST, turn into a GET without a
// body) and the headers (checkRedirect).
Result<Request> redirectRequest(const Request& from, int status, std::string_view location, bool allowInsecure);

// `text` with every occurrence of the URL's query string replaced by "?...", so that it can be logged.
std::string redactText(std::string text, std::string_view url);
// "name\nvalue\n..." (the backends' header blocks) to headers, and back.
Headers parseHeaderBlock(std::string_view block);
std::string headerBlock(const Headers& headers);

inline constexpr int kMaxRedirects = 5;

} // namespace quench::net::detail
