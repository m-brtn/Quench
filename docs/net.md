# Network: HTTPS requests

`quench_net` (since 2026-11-13, after 0.17.0) sends asynchronous HTTP requests: what a game needs to talk to a web
API, first of all an OpenAI-compatible one (OpenRouter, OpenAI, a local server) for an in-game assistant (chat
completions with tool calls as JSON, optionally streamed as server-sent events; speech-to-text, a few seconds of
recorded audio uploaded as JSON with base64 or as multipart; text-to-speech, audio bytes downloaded, possibly
streamed). It is the transport only: the assistant is a later module on top of it. Nothing in it blocks a frame or
waits through Asyncify.

Public headers: `include/quench/net/Http.h` (the client) and `include/quench/net/FakeNetwork.h` (the fake). The
target is headless (no SDL, no GPU); `quench_add_game()` links it into every game, so a game only includes it.

## Use

```cpp
#include <quench/net/Http.h>

net::Client m_net;                // a member; m_net.update() once a frame in Game::update

net::Request request;
request.method = "POST";
request.url = "https://openrouter.ai/api/v1/chat/completions";
request.headers = {{"Authorization", "Bearer " + playerKey}, {"Content-Type", "application/json"}};
request.setBody(json);
request.streamBody = true;        // the body to onChunk as it arrives
const Result<net::RequestId> id = m_net.send(std::move(request), {
    .onHeaders = [](const net::Response& head) { /* status and headers */ },
    .onChunk = [](std::span<const std::uint8_t> piece) { /* parse SSE lines */ },
    .onDone = [](const net::Response& r) { /* r.ok(), r.status, r.error, r.message */ },
});
```

- `Request`: `method`, `url`, `headers`, `body` (bytes; `setBody(text)`), `timeoutSeconds` (from the send to the end
  of the body; 30 by default, 0 for none), `maxResponseBytes` (16 MB by default), `streamBody`.
- `Response`: `status` (0 when no answer came), `headers` (`header(name)`, case-insensitive), `body` (empty when
  streamed), `bytes`, `seconds`, `error` (an `ErrorKind`) and `message`. `ok()` means the transport worked, any
  status; `success()` adds a 2xx status.
- `ErrorKind`: `InvalidRequest`, `NotAllowed` (the HTTPS rule, also for a redirect's target), `NoBackend` (reserved: today a platform without a backend fails `send` at once instead),
  `NoNetwork` (offline, the host not found, the connection refused or lost), `Timeout`, `Cancelled`, `Tls`,
  `TooLarge`, `Failed` (anything else; the browser's fetch cannot say more). `errorKindName()` names them.
- `Client::send(Request, Handlers)` returns a `RequestId`, or an error at once (and no handler is called) for a URL
  the rules refuse, a malformed request (a header value with a line break, a GET with a body) or a platform without
  a backend. `cancel(id)`, `cancelAll()`, `active(id)`, `activeCount()`, `backendName()` (`nsurlsession`, `fetch`,
  `fake`, `none`), `available()`.
- Several requests run at once; the destructor cancels what is still running without calling its handlers.

**Callbacks, not polling.** The handlers (`onHeaders`, `onChunk`, `onDone`) are called from `Client::update()` only,
on the thread that calls it (the main thread), in the order things arrived: a streamed body needs every piece in
order, and polling would make each game keep its own queue of pieces. `onDone` is called exactly once per sent
request, also after `cancel()` (with `Cancelled`, from the next `update()`, or from the running one when a handler
cancels). A handler may send and cancel; it must not destroy the client. This matches the asset manager and the
deferred files, whose work also finishes in a call the app makes once a frame on the main thread.

## Rules

- **HTTPS only**, except `http://localhost`, `http://127.0.0.1` and `http://[::1]` (a local server; any port).
  `ClientDesc::allowInsecure` lifts it, for tests against a server elsewhere only. Other schemes are refused, and so
  is a URL with `user:password@` (credentials go in a header). `net::checkUrl()` is the rule.
- **The log** (category `net`, one line per request at its end) shows the method, the host and path, the status, the
  bytes and the milliseconds (`net: POST openrouter.ai/api/v1/chat/completions: 200, 5123 bytes, 2400 ms`), never a
  header's value, a body or the query string; error messages from a backend have the URL's query string replaced
  by `...` before they are logged or put into `Response::message`. `net::logUrl()` is the form shown.
- **Redirects** are followed (at most 5). A redirect's target must pass the HTTPS rule too, and https never continues
  over plain http. To another origin (scheme, host or port), the secret headers stay behind
  (`net::isSecretHeader()`: Authorization, Proxy-Authorization, Cookie, Set-Cookie and names with key, token, secret,
  password or session in them: X-API-Key, api-key, X-Goog-Api-Key ...). 303, and 301 or 302 after a POST, continue
  as a GET without a body.
- **No cookies**: none are stored, none are sent from a store (a game may still set a `Cookie` header itself).
  Nothing is cached.
- **The size limit** is enforced while the body arrives: a declared `Content-Length` over it ends the request at its
  headers, and the piece that would pass it ends it without being delivered (`TooLarge`).

## Backends

One per build, chosen by CMake (`src/net/Backend.h` is the interface; a backend moves bytes, the client keeps the
rules, limits, timeouts and the log):

| Platform | Backend | How |
|---|---|---|
| macOS, iOS | `nsurlsession` (`src/net/AppleBackend.m`, `AppleBackend.cpp`) | An NSURLSession per client with an ephemeral configuration (no cookie storage, no cache, no credential storage, no waiting for connectivity), a data task per request. Its delegate runs on a serial background queue the system owns (like the audio device's thread, not a job): it only copies the headers and each piece of the body into a locked queue that `update()` takes, enforces the size limit and decides redirects. |
| Web (the `web` preset) | `fetch` (`src/net/WebBackend.cpp`) | The browser's `fetch` through `EM_JS`, with `credentials: "omit"` and `cache: "no-store"`, an `AbortController` per request (cancel, the timeout, the size limit) and a `ReadableStream` reader for the pieces. Its promises queue events in JavaScript; `update()` takes them, so the browser never calls into the program and nothing waits through Asyncify. |
| Linux, Windows, the Node.js test build (`wasm-node`) | `none` (`src/net/NonePlatform.cpp`) | `send` fails at once: "no network backend on this platform". A libcurl or WinHTTP backend replaces this one file. |
| Every build | `fake` (`src/net/FakeNetwork.cpp`) | `ClientDesc::fake`: see below. |

`ClientDesc::noBackend` gives the `none` backend anywhere (to test a game's behaviour without one).

Notes per platform:

- **Apple.** HTTPS needs nothing in an app's `Info.plist`: App Transport Security allows TLS 1.2 and later with valid
  certificates by default. NSURLSession is known to hold back the first 512 bytes of a body to sniff its type unless
  the server sends `X-Content-Type-Options: nosniff`; the test server sends it with `text/event-stream`, and the
  pieces came through one by one. Whether a provider's stream without that header is held back at its start was
  not tried (it would delay only the first half kilobyte). NSURLSession leaves `Authorization` out of every redirected request; the
  backend puts it back when the target has the original request's origin (as a browser does). Plain HTTP to
  `127.0.0.1` and `localhost` worked from the test executable (a command line program without an `Info.plist`, to
  which ATS does not apply). An app bundle is subject to ATS through its `Info.plist`; plain http to a local server
  from a bundle (a game, the iOS test app) was not tried. If a game needs it, ATS's key for local addresses is
  `NSAllowsLocalNetworking` in that game's own `Info.plist`; the engine's `Info.plist` template stays without it, so
  ATS is not weakened for games.
- **Web.** The browser decides more than the program: a request to another origin needs the server's CORS headers
  (whether a provider sends them for its API was not checked here; a local server may not), and only the CORS-safelisted and exposed
  response headers are visible. The browser follows redirects itself and, by the Fetch standard, drops
  `Authorization` on a redirect to another origin; the client checks the final URL against the HTTPS rule. A page on
  https cannot fetch plain http other than from `localhost` (mixed content), and Chrome's Private Network Access rules may
  block or ask before a public page reaches a local address (not tried). The browser's network errors are one opaque
  `TypeError`: offline is told apart (`NoNetwork`), CORS, TLS and a refused connection all become `Failed`.
  `net::pageOrigin()` is the page's origin (`http://127.0.0.1:8000`), empty elsewhere.

## The fake

`net::FakeNetwork` (public, in every build) answers by method and URL (`respond("POST", url, FakeResponse)`; an exact
URL, then one without its query string; `"*"` for any method; anything else 404): a status, headers, a body in
pieces each with a delay after the previous one (`FakeChunk`), a delay before the headers and before the end, an error
instead (`FakeResponse::failure`), a request that never answers (`silence()`, for timeouts and cancels) or a redirect
(`redirect(status, location)`, followed through the same policy). It records every request it received
(`requests()`, `last()`: the method, URL, headers and body as sent, also those of followed redirects, and whether it
was cancelled), and `open()` counts those in flight. Time is the client's clock: a test that gives
`ClientDesc::clock` a variable decides when each piece arrives.

```cpp
net::FakeNetwork fake;
double now = 0.0;
net::Client client(net::ClientDesc{.fake = &fake, .clock = [&] { return now; }});
fake.respond("GET", "https://api.example.com/v1/models", net::FakeResponse::text(200, R"({"data":[]})"));
```

## Tests

- `net.*` (label `net`, `tests/net/NetTests.cpp`, with the fake; also in the `headless`, `wasm-node` and iOS
  Simulator builds and the device test app): the URL rule, malformed requests, completion and headers, a POST's
  record, chunks in order by the fake's clock, several at once, cancel (also from a handler, and the destructor's),
  timeouts (before the headers and during the body), the size limit (streamed, exact, a declared length), the HTTPS
  rule at send, redirects (secrets dropped on another origin, https to http refused, a loop, 303), errors, the log's
  redaction (no key, cookie, query string or body in the log or the messages), sending from a handler, the `none`
  backend.
- `net_apple.*` (label `net`, macOS, `tests/net/AppleNetTests.cpp`): the NSURLSession backend against
  `tests/net/net_test_server.py`, a server of Python's standard library that CTest starts for each case on a free
  port of 127.0.0.1 (`--run`, the case's launcher): GET, a POST with its body echoed, request and response headers,
  404 and 500, a chunked answer of 5 pieces over a second (the first piece about a second before the end), a slow
  answer against a 0.5 s timeout, a cancel in the middle, redirects (the same origin keeps `Authorization` and the
  key, another origin, `localhost`, drops them, plain http elsewhere refused), the size limit (declared, chunked,
  exact), no cookies kept, three at once, a refused connection.
- `tools/check.sh step web`: hello's `scenarios/net_web.txt` in headless Chrome against `tools/web_check.mjs`'s
  `/net/` paths (a GET, a POST echoed, 4 chunks over 0.75 s delivered as they came, a 404, a cancel in the middle;
  `examples/hello/NetCheck.h` has its commands).

No automated test leaves the machine. A hand check on 2026-11-13 on the Mac: `https://example.com/` answered 200
over TLS through NSURLSession, and `https://expired.badssl.com/` ended with `Tls`.

## Cost

Each piece of a body is copied twice on its way (the backend's queue, then the response or the handler), and
`update()` takes a lock once (Apple) or calls JavaScript once per event (web). For the sizes of an assistant (a few
kilobytes of JSON, a few hundred kilobytes of audio) that is well under a millisecond of main-thread time a frame;
not measured. A request's body is copied when it is sent.

## Not there

WebSocket, HTTP/3 and protocol control, proxies set by the game (the system's proxy settings apply on Apple
platforms and in browsers), certificate pinning or own trust roots, uploads from a file and downloads to a file
(bodies are in memory), multipart building (a game builds the body), cookies, a response cache, request priorities,
progress of an upload, a backend for Linux, Windows or Android, and requests from the Node.js test build.
