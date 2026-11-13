// quench::net::Client: the requests' state on the main thread, the rules, the limits, the timeouts and the log
// (quench/net/Http.h). The backends only move bytes (Backend.h).

#include <quench/net/Http.h>

#include "net/Backend.h"

#include <quench/core/Assert.h>
#include <quench/core/Log.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <utility>

namespace quench::net {
namespace {

constexpr const char* kLog = "net";

bool isToken(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        if (!ok) return false;
    }
    return true;
}

bool isHeaderValue(std::string_view text) {
    return std::none_of(text.begin(), text.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

Result<void> checkRequest(const Request& request, bool allowInsecure) {
    if (!isToken(request.method)) return Error{std::format("'{}' is not an HTTP method", request.method)};
    if (Result<void> url = checkUrl(request.url, allowInsecure); !url) return url;
    for (const Header& h : request.headers) {
        if (!isToken(h.name)) return Error{std::format("'{}' is not a header name", h.name)};
        // The value is not quoted: it may be a key.
        if (!isHeaderValue(h.value)) return Error{std::format("the value of the header {} has a line break", h.name)};
    }
    if ((request.method == "GET" || request.method == "HEAD") && !request.body.empty())
        return Error{std::format("a {} request cannot have a body", request.method)};
    if (request.timeoutSeconds < 0.0) return Error{"a negative timeout"};
    return {};
}

} // namespace

struct Client::Entry {
    RequestId id = 0;
    Request request;
    Handlers handlers;
    Response response;
    double start = 0.0;
    bool headers = false;
    bool finished = false;
};

Client::Client(const ClientDesc& desc) : m_desc(desc) {
    if (m_desc.fake)
        m_backend = detail::createFakeBackend(*m_desc.fake);
    else if (m_desc.noBackend)
        m_backend = detail::createNoneBackend();
    else
        m_backend = detail::createPlatformBackend();
    if (!m_desc.clock) {
        const auto origin = std::chrono::steady_clock::now();
        m_desc.clock = [origin] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
        };
    }
}

Client::~Client() {
    for (const auto& entry : m_entries)
        if (!entry->finished) m_backend->cancel(entry->id);
}

double Client::now() const { return m_desc.clock(); }

const char* Client::backendName() const { return m_backend->name(); }
bool Client::available() const { return m_backend->available(); }

Result<RequestId> Client::send(Request request, Handlers handlers) {
    const std::string where = std::format("{} {}", request.method, logUrl(request.url));
    if (!m_backend->available()) {
        log::warning(kLog, "{}: no network backend on this platform", where);
        return Error{"no network backend on this platform"};
    }
    if (Result<void> valid = checkRequest(request, m_desc.allowInsecure); !valid) {
        log::warning(kLog, "{}: refused: {}", where, valid.error());
        return Error{valid.error()};
    }
    auto entry = std::make_unique<Entry>();
    entry->id = m_nextId++;
    entry->start = now();
    if (Result<void> started = m_backend->start(entry->id, request, m_desc.allowInsecure, entry->start); !started) {
        const std::string message = detail::redactText(started.error(), request.url);
        log::warning(kLog, "{}: not started: {}", where, message);
        return Error{message};
    }
    entry->request = std::move(request);
    entry->handlers = std::move(handlers);
    const RequestId id = entry->id;
    m_entries.push_back(std::move(entry));
    return id;
}

void Client::cancel(RequestId id) {
    for (const auto& entry : m_entries) {
        if (entry->id != id || entry->finished) continue;
        m_backend->cancel(id);
        finish(*entry, ErrorKind::Cancelled, "cancelled");
        return;
    }
}

void Client::cancelAll() {
    for (std::size_t i = 0; i < m_entries.size(); ++i)
        if (!m_entries[i]->finished) cancel(m_entries[i]->id);
}

bool Client::active(RequestId id) const {
    return std::any_of(m_entries.begin(), m_entries.end(),
                       [id](const auto& entry) { return entry->id == id && !entry->finished; });
}

int Client::activeCount() const {
    return int(std::count_if(m_entries.begin(), m_entries.end(), [](const auto& entry) { return !entry->finished; }));
}

void Client::finish(Entry& entry, ErrorKind error, std::string message) {
    QUENCH_ASSERT(!entry.finished, "a request finished twice");
    entry.finished = true;
    entry.response.error = error;
    entry.response.message = detail::redactText(std::move(message), entry.request.url);
    entry.response.seconds = std::max(0.0, now() - entry.start);
    m_finished.push_back(entry.id);
}

void Client::deliver(Entry& entry) {
    const Response& r = entry.response;
    const std::string where = std::format("{} {}", entry.request.method, logUrl(entry.request.url));
    const long long ms = (long long)(r.seconds * 1000.0 + 0.5);
    if (r.ok())
        log::info(kLog, "{}: {}, {} bytes, {} ms", where, r.status, r.bytes, ms);
    else if (r.error == ErrorKind::Cancelled)
        log::info(kLog, "{}: cancelled after {} bytes, {} ms", where, r.bytes, ms);
    else
        log::warning(kLog, "{}: {} ({}), status {}, {} bytes, {} ms", where, errorKindName(r.error), r.message,
                     r.status, r.bytes, ms);
    if (entry.handlers.onDone) entry.handlers.onDone(r);
}

void Client::update() {
    QUENCH_ASSERT(!m_updating, "Client::update() called from one of its handlers");
    m_updating = true;
    const double t = now();
    std::vector<detail::Event> events;
    m_backend->poll(t, events);

    auto find = [this](RequestId id) -> Entry* {
        for (const auto& entry : m_entries)
            if (entry->id == id) return entry.get();
        return nullptr;
    };
    for (detail::Event& event : events) {
        Entry* entry = find(event.id);
        if (!entry || entry->finished) continue; // cancelled, timed out or over the limit meanwhile
        Response& r = entry->response;
        switch (event.type) {
        case detail::Event::Type::Headers: {
            if (!event.finalUrl.empty() && event.finalUrl != entry->request.url) {
                if (Result<detail::RedirectHeaders> allowed =
                        detail::checkRedirect(entry->request.url, event.finalUrl, m_desc.allowInsecure);
                    !allowed) {
                    m_backend->cancel(entry->id);
                    finish(*entry, ErrorKind::NotAllowed, allowed.error());
                    break;
                }
            }
            r.status = event.status;
            r.headers = std::move(event.headers);
            entry->headers = true;
            if (const std::string* length = r.header("Content-Length")) {
                unsigned long long bytes = 0;
                const auto [end, ec] = std::from_chars(length->data(), length->data() + length->size(), bytes);
                if (ec == std::errc() && bytes > entry->request.maxResponseBytes) {
                    m_backend->cancel(entry->id);
                    finish(*entry, ErrorKind::TooLarge,
                           std::format("the body of {} bytes is over the limit of {}", bytes,
                                       entry->request.maxResponseBytes));
                    break;
                }
            }
            if (entry->handlers.onHeaders) entry->handlers.onHeaders(r);
            break;
        }
        case detail::Event::Type::Data: {
            if (r.bytes + event.bytes.size() > entry->request.maxResponseBytes) {
                m_backend->cancel(entry->id);
                r.bytes += event.bytes.size();
                finish(*entry, ErrorKind::TooLarge,
                       std::format("the body is over the limit of {} bytes", entry->request.maxResponseBytes));
                break;
            }
            r.bytes += event.bytes.size();
            if (entry->request.streamBody) {
                if (entry->handlers.onChunk) entry->handlers.onChunk(event.bytes);
            } else {
                r.body.insert(r.body.end(), event.bytes.begin(), event.bytes.end());
            }
            break;
        }
        case detail::Event::Type::Done:
            if (!entry->headers) {
                finish(*entry, ErrorKind::Failed, "the answer ended before its headers");
                break;
            }
            finish(*entry, ErrorKind::None, {});
            break;
        case detail::Event::Type::Failed:
            m_backend->cancel(entry->id);
            finish(*entry, event.error, std::move(event.message));
            break;
        }
    }

    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        Entry& entry = *m_entries[i];
        if (entry.finished || entry.request.timeoutSeconds <= 0.0) continue;
        if (t - entry.start >= entry.request.timeoutSeconds) {
            m_backend->cancel(entry.id);
            finish(entry, ErrorKind::Timeout,
                   std::format("no complete answer within {} s", entry.request.timeoutSeconds));
        }
    }

    // The ends, in the order they happened; a handler may send, cancel (its end is delivered in this loop) or not.
    for (std::size_t i = 0; i < m_finished.size(); ++i) {
        if (Entry* entry = find(m_finished[i])) deliver(*entry);
    }
    std::erase_if(m_entries, [](const auto& entry) { return entry->finished; });
    m_finished.clear();
    m_updating = false;
}

} // namespace quench::net
