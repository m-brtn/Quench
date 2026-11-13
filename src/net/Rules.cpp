// The URL rules, the redirect policy and the helpers the backends share (Backend.h).

#include "net/Backend.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

namespace quench::net {
namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }

std::string lowerCopy(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = lower(c);
    return out;
}

bool equalNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

// Visible ASCII only: a URL with spaces, controls or raw UTF-8 must be percent-encoded by the caller.
bool plainUrlText(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c > 0x20 && c < 0x7f; });
}

} // namespace

const std::string* findHeader(const Headers& headers, std::string_view name) {
    for (const Header& h : headers)
        if (equalNoCase(h.name, name)) return &h.value;
    return nullptr;
}

void Request::setBody(std::string_view text) { body.assign(text.begin(), text.end()); }

const char* errorKindName(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::None: return "none";
    case ErrorKind::InvalidRequest: return "invalid request";
    case ErrorKind::NotAllowed: return "not allowed";
    case ErrorKind::NoBackend: return "no backend";
    case ErrorKind::NoNetwork: return "no network";
    case ErrorKind::Timeout: return "timeout";
    case ErrorKind::Cancelled: return "cancelled";
    case ErrorKind::Tls: return "TLS failure";
    case ErrorKind::TooLarge: return "too large";
    case ErrorKind::Failed: return "failed";
    }
    return "unknown";
}

Result<void> checkUrl(std::string_view url, bool allowInsecure) {
    const std::optional<detail::Url> parsed = detail::parseUrl(url);
    if (!parsed) return Error{std::format("'{}' is not an absolute http or https URL", logUrl(url))};
    if (parsed->userInfo) return Error{"a URL with user:password@ is not allowed (send credentials in a header)"};
    if (parsed->scheme == "https") return {};
    if (parsed->scheme == "http") {
        if (allowInsecure || detail::isLoopbackHost(parsed->host)) return {};
        return Error{std::format("plain http to {} is not allowed: https only, except http://localhost, "
                                 "http://127.0.0.1 and http://[::1]",
                                 parsed->host)};
    }
    return Error{std::format("the scheme '{}' is not allowed (https only)", parsed->scheme)};
}

std::string logUrl(std::string_view url) {
    if (const std::optional<detail::Url> parsed = detail::parseUrl(url)) {
        std::string out = parsed->host;
        if (parsed->port >= 0) out += std::format(":{}", parsed->port);
        return out + parsed->path;
    }
    // Not a URL: everything up to a query string or fragment, at most 80 characters.
    std::string_view head = url.substr(0, url.find_first_of("?#"));
    if (const std::size_t at = head.find('@'); at != std::string_view::npos) head = head.substr(at + 1);
    return std::string(head.substr(0, 80));
}

bool isSecretHeader(std::string_view name) {
    const std::string n = lowerCopy(name);
    if (n == "authorization" || n == "proxy-authorization" || n == "cookie" || n == "set-cookie") return true;
    for (const char* part : {"key", "token", "secret", "password", "session"})
        if (n.find(part) != std::string::npos) return true;
    return false;
}

std::string pageOrigin() { return detail::platformPageOrigin(); }

namespace detail {

int Url::effectivePort() const {
    if (port >= 0) return port;
    return scheme == "https" ? 443 : 80;
}

std::string Url::origin() const {
    std::string out = scheme + "://" + host;
    if (port >= 0 && port != (scheme == "https" ? 443 : 80)) out += std::format(":{}", port);
    return out;
}

std::optional<Url> parseUrl(std::string_view text) {
    if (!plainUrlText(text)) return std::nullopt;
    const std::size_t schemeEnd = text.find("://");
    if (schemeEnd == std::string_view::npos || schemeEnd == 0) return std::nullopt;
    Url url;
    url.scheme = lowerCopy(text.substr(0, schemeEnd));
    for (char c : url.scheme)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
            return std::nullopt;
    std::string_view rest = text.substr(schemeEnd + 3);
    const std::size_t authorityEnd = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authorityEnd);
    rest = authorityEnd == std::string_view::npos ? std::string_view() : rest.substr(authorityEnd);
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        url.userInfo = true;
        authority = authority.substr(at + 1);
    }
    std::string_view host = authority, port;
    if (authority.starts_with('[')) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = authority.substr(0, close + 1);
        const std::string_view after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return std::nullopt;
            port = after.substr(1);
            if (port.empty()) return std::nullopt;
        }
    } else if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
        if (port.empty()) return std::nullopt;
    }
    if (host.empty() || host == "[]") return std::nullopt;
    url.host = lowerCopy(host);
    if (!port.empty()) {
        int value = 0;
        const auto [end, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
        if (ec != std::errc() || end != port.data() + port.size() || value < 1 || value > 65535) return std::nullopt;
        url.port = value;
    }
    const std::size_t fragment = rest.find('#');
    if (fragment != std::string_view::npos) rest = rest.substr(0, fragment);
    const std::size_t query = rest.find('?');
    url.path = std::string(rest.substr(0, query));
    if (query != std::string_view::npos) url.query = std::string(rest.substr(query + 1));
    if (url.path.empty()) url.path = "/";
    return url;
}

std::optional<std::string> resolveUrl(std::string_view base, std::string_view location) {
    if (location.empty()) return std::nullopt;
    if (location.find("://") != std::string_view::npos) {
        if (!parseUrl(location)) return std::nullopt;
        return std::string(location);
    }
    const std::optional<Url> b = parseUrl(base);
    if (!b) return std::nullopt;
    std::string out;
    if (location.starts_with("//")) {
        out = b->scheme + ":" + std::string(location);
    } else if (location.starts_with('/')) {
        out = b->origin() + std::string(location);
    } else {
        const std::size_t slash = b->path.rfind('/');
        out = b->origin() + b->path.substr(0, slash + 1) + std::string(location);
    }
    if (!parseUrl(out)) return std::nullopt;
    return out;
}

bool isLoopbackHost(std::string_view host) { return host == "localhost" || host == "127.0.0.1" || host == "[::1]"; }

Result<RedirectHeaders> checkRedirect(std::string_view from, std::string_view to, bool allowInsecure) {
    if (Result<void> allowed = checkUrl(to, allowInsecure); !allowed)
        return Error{std::format("a redirect to {} was refused: {}", logUrl(to), allowed.error())};
    const std::optional<Url> a = parseUrl(from), b = parseUrl(to);
    if (!a || !b) return Error{"a redirect between malformed URLs"};
    // An https page must not be continued over plain http, even to the loopback address.
    if (a->scheme == "https" && b->scheme == "http" && !allowInsecure)
        return Error{std::format("a redirect from https to plain http ({}) was refused", logUrl(to))};
    const bool sameOrigin = a->scheme == b->scheme && a->host == b->host && a->effectivePort() == b->effectivePort();
    return sameOrigin ? RedirectHeaders::Keep : RedirectHeaders::DropSecrets;
}

Result<Request> redirectRequest(const Request& from, int status, std::string_view location, bool allowInsecure) {
    const std::optional<std::string> target = resolveUrl(from.url, location);
    if (!target) return Error{std::format("a redirect from {} has no usable Location", logUrl(from.url))};
    const Result<RedirectHeaders> policy = checkRedirect(from.url, *target, allowInsecure);
    if (!policy) return Error{policy.error()};
    Request next = from;
    next.url = *target;
    if (status == 303 || ((status == 301 || status == 302) && from.method == "POST")) {
        next.method = "GET";
        next.body.clear();
        std::erase_if(next.headers, [](const Header& h) {
            return equalNoCase(h.name, "Content-Type") || equalNoCase(h.name, "Content-Length");
        });
    }
    if (*policy == RedirectHeaders::DropSecrets)
        std::erase_if(next.headers, [](const Header& h) { return isSecretHeader(h.name); });
    return next;
}

std::string redactText(std::string text, std::string_view url) {
    const std::optional<Url> parsed = parseUrl(url);
    if (!parsed || parsed->query.empty()) return text;
    const std::string& query = parsed->query;
    for (std::size_t at = text.find(query); at != std::string::npos; at = text.find(query, at + 3))
        text.replace(at, query.size(), "...");
    return text;
}

Headers parseHeaderBlock(std::string_view block) {
    Headers headers;
    while (!block.empty()) {
        const std::size_t nameEnd = block.find('\n');
        if (nameEnd == std::string_view::npos) break;
        const std::string_view name = block.substr(0, nameEnd);
        block = block.substr(nameEnd + 1);
        const std::size_t valueEnd = block.find('\n');
        const std::string_view value = block.substr(0, valueEnd);
        block = valueEnd == std::string_view::npos ? std::string_view() : block.substr(valueEnd + 1);
        if (!name.empty()) headers.push_back({std::string(name), std::string(value)});
    }
    return headers;
}

std::string headerBlock(const Headers& headers) {
    std::string out;
    for (const Header& h : headers) out += h.name + "\n" + h.value + "\n";
    return out;
}

} // namespace detail
} // namespace quench::net
