// The Apple backend's C++ side (macOS and iOS): NSURLSession in AppleBackend.m calls the functions below from its
// delegate queue (a background thread the system owns); they only append to a locked queue that poll() takes on the
// main thread.

#include "net/Backend.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <utility>

extern "C" {
// AppleBackend.m
void* quench_net_apple_open(void* sink);
void quench_net_apple_close(void* session);
int quench_net_apple_start(void* session, std::uint64_t id, const char* method, const char* url,
                           const char* const* names, const char* const* values, std::size_t headerCount,
                           const void* body, std::size_t bodySize, double timeoutSeconds, std::uint64_t maxBytes,
                           int allowInsecure);
void quench_net_apple_cancel(void* session, std::uint64_t id);

// Called by AppleBackend.m (any thread).
void quench_net_apple_headers(void* sink, std::uint64_t id, int status, const char* headerBlock, const char* finalUrl);
void quench_net_apple_data(void* sink, std::uint64_t id, const void* data, std::size_t size);
void quench_net_apple_done(void* sink, std::uint64_t id);
void quench_net_apple_failed(void* sink, std::uint64_t id, int kind, long code, const char* message);
void quench_net_apple_sink_release(void* sink);
int quench_net_redirect_policy(const char* from, const char* to, int allowInsecure, char* reason, std::size_t size);
int quench_net_is_secret_header(const char* name);
}

namespace quench::net::detail {
namespace {

struct Sink {
    std::mutex mutex;
    std::vector<Event> events;

    void push(Event event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }
};

// NSURLErrorDomain codes (NSURLError.h) to the engine's kinds.
ErrorKind kindOfUrlError(long code) {
    switch (code) {
    case -999: return ErrorKind::Cancelled;   // NSURLErrorCancelled
    case -1001: return ErrorKind::Timeout;    // NSURLErrorTimedOut
    case -1003:                               // NSURLErrorCannotFindHost
    case -1004:                               // NSURLErrorCannotConnectToHost
    case -1005:                               // NSURLErrorNetworkConnectionLost
    case -1006:                               // NSURLErrorDNSLookupFailed
    case -1009:                               // NSURLErrorNotConnectedToInternet
    case -1018:                               // NSURLErrorInternationalRoamingOff
    case -1019:                               // NSURLErrorCallIsActive
    case -1020: return ErrorKind::NoNetwork;  // NSURLErrorDataNotAllowed
    case -1022: return ErrorKind::NotAllowed; // NSURLErrorAppTransportSecurityRequiresSecureConnection
    case -1103: return ErrorKind::TooLarge;   // NSURLErrorDataLengthExceedsMaximum
    case -1200:                               // NSURLErrorSecureConnectionFailed
    case -1201:                               // NSURLErrorServerCertificateHasBadDate
    case -1202:                               // NSURLErrorServerCertificateUntrusted
    case -1203:                               // NSURLErrorServerCertificateHasUnknownRoot
    case -1204:                               // NSURLErrorServerCertificateNotYetValid
    case -1205:                               // NSURLErrorClientCertificateRejected
    case -1206: return ErrorKind::Tls;        // NSURLErrorClientCertificateRequired
    default: return ErrorKind::Failed;
    }
}

class AppleBackend final : public Backend {
public:
    AppleBackend() : m_sink(std::make_shared<Sink>()) {
        // The session's delegate keeps this reference until the session is invalid (quench_net_apple_sink_release).
        m_session = quench_net_apple_open(new std::shared_ptr<Sink>(m_sink));
    }
    ~AppleBackend() override { quench_net_apple_close(m_session); }

    const char* name() const override { return "nsurlsession"; }

    Result<void> start(RequestId id, const Request& request, bool allowInsecure, double) override {
        std::vector<const char*> names, values;
        for (const Header& h : request.headers) {
            names.push_back(h.name.c_str());
            values.push_back(h.value.c_str());
        }
        const int status =
            quench_net_apple_start(m_session, id, request.method.c_str(), request.url.c_str(), names.data(),
                                   values.data(), names.size(), request.body.data(), request.body.size(),
                                   request.timeoutSeconds, request.maxResponseBytes, allowInsecure ? 1 : 0);
        if (status != 0) return Error{"NSURLSession refused the request (a URL Foundation cannot parse)"};
        return {};
    }

    void cancel(RequestId id) override { quench_net_apple_cancel(m_session, id); }

    void poll(double, std::vector<Event>& events) override {
        std::lock_guard lock(m_sink->mutex);
        if (events.empty()) {
            events.swap(m_sink->events);
        } else {
            for (Event& e : m_sink->events) events.push_back(std::move(e));
            m_sink->events.clear();
        }
    }

private:
    std::shared_ptr<Sink> m_sink;
    void* m_session = nullptr;
};

Sink& sinkOf(void* sink) { return **static_cast<std::shared_ptr<Sink>*>(sink); }

} // namespace

std::unique_ptr<Backend> createPlatformBackend() { return std::make_unique<AppleBackend>(); }
std::string platformPageOrigin() { return {}; }

} // namespace quench::net::detail

using namespace quench::net;
using quench::net::detail::Event;

extern "C" void quench_net_apple_headers(void* sink, std::uint64_t id, int status, const char* block,
                                         const char* finalUrl) {
    detail::sinkOf(sink).push({.type = Event::Type::Headers,
                               .id = id,
                               .status = status,
                               .headers = detail::parseHeaderBlock(block ? block : ""),
                               .finalUrl = finalUrl ? finalUrl : ""});
}

extern "C" void quench_net_apple_data(void* sink, std::uint64_t id, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    detail::sinkOf(sink).push({.type = Event::Type::Data, .id = id, .bytes = {bytes, bytes + size}});
}

extern "C" void quench_net_apple_done(void* sink, std::uint64_t id) {
    detail::sinkOf(sink).push({.type = Event::Type::Done, .id = id});
}

// kind: 0 an NSURLErrorDomain error (code), 1 another domain, 2 over the size limit, 3 a refused redirect.
extern "C" void quench_net_apple_failed(void* sink, std::uint64_t id, int kind, long code, const char* message) {
    ErrorKind error = ErrorKind::Failed;
    if (kind == 0) error = detail::kindOfUrlError(code);
    if (kind == 2) error = ErrorKind::TooLarge;
    if (kind == 3) error = ErrorKind::NotAllowed;
    std::string text = message ? message : "";
    if (kind <= 1) text = std::format("{} (code {})", text, code);
    detail::sinkOf(sink).push({.type = Event::Type::Failed, .id = id, .error = error, .message = std::move(text)});
}

extern "C" void quench_net_apple_sink_release(void* sink) { delete static_cast<std::shared_ptr<detail::Sink>*>(sink); }

// 0: refused (the reason in `reason`), 1: followed with every header, 2: followed without the secret headers.
extern "C" int quench_net_redirect_policy(const char* from, const char* to, int allowInsecure, char* reason,
                                          std::size_t size) {
    const quench::Result<detail::RedirectHeaders> policy = detail::checkRedirect(from, to, allowInsecure != 0);
    if (!policy) {
        if (reason && size > 0) {
            const std::size_t n = std::min(size - 1, policy.error().size());
            std::memcpy(reason, policy.error().data(), n);
            reason[n] = '\0';
        }
        return 0;
    }
    return *policy == detail::RedirectHeaders::Keep ? 1 : 2;
}

extern "C" int quench_net_is_secret_header(const char* name) { return isSecretHeader(name) ? 1 : 0; }
