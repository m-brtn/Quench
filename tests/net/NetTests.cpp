// quench_net with the fake network (every build, the Node.js one included): the rules, completion, chunks in order,
// headers, cancel, timeouts, the size limit, redirects and the log's redaction.

#include "Test.h"

#include "core/Platform.h"

#include <quench/core/Log.h>
#include <quench/net/FakeNetwork.h>
#include <quench/net/Http.h>

#include <string>
#include <vector>

using namespace quench;
using namespace quench::net;

namespace {

// A client on a fake network with a clock the case moves.
struct Rig {
    FakeNetwork fake;
    double time = 0.0;
    Client client{ClientDesc{.fake = &fake, .clock = [this] { return time; }}};

    void advance(double seconds) {
        time += seconds;
        client.update();
    }
};

// What the handlers saw.
struct Seen {
    int headers = 0;
    int done = 0;
    std::vector<std::string> chunks;
    std::vector<double> chunkTimes;
    Response head;
    Response last;

    Handlers handlers(const double* clock = nullptr) {
        return {
            .onHeaders =
                [this](const Response& r) {
                    ++headers;
                    head = r;
                },
            .onChunk =
                [this, clock](std::span<const std::uint8_t> c) {
                    chunks.emplace_back(c.begin(), c.end());
                    chunkTimes.push_back(clock ? *clock : 0.0);
                },
            .onDone =
                [this](const Response& r) {
                    ++done;
                    last = r;
                },
        };
    }
};

Request get(std::string url) {
    Request r;
    r.url = std::move(url);
    return r;
}

// The log lines written while it lives.
struct LogCapture {
    std::vector<std::string> lines;
    int id;
    LogCapture() {
        id = log::addSink([this](log::Level, std::string_view category, std::string_view message) {
            lines.push_back(std::string(category) + ": " + std::string(message));
        });
    }
    ~LogCapture() { log::removeSink(id); }
    std::string all() const {
        std::string out;
        for (const std::string& l : lines) out += l + "\n";
        return out;
    }
};

} // namespace

QUENCH_TEST(net_url_rules) {
    CHECK(checkUrl("https://api.example.com/v1/chat"), "https");
    CHECK(checkUrl("HTTPS://API.example.com"), "upper-case scheme");
    CHECK(checkUrl("http://localhost:8080/v1"), "localhost");
    CHECK(checkUrl("http://127.0.0.1/"), "127.0.0.1");
    CHECK(checkUrl("http://[::1]:11434/api"), "[::1]");
    CHECK(!checkUrl("http://example.com/"), "plain http elsewhere");
    CHECK(!checkUrl("http://localhost.example.com/"), "a host that only starts with localhost");
    CHECK(!checkUrl("http://127.0.0.2/"), "another loopback address is not on the list");
    CHECK(checkUrl("http://example.com/", true), "allowInsecure");
    CHECK(!checkUrl("ftp://example.com/", true), "another scheme, even with allowInsecure");
    CHECK(!checkUrl("https://user:pass@example.com/"), "credentials in the URL");
    CHECK(!checkUrl("example.com/path"), "not absolute");
    CHECK(!checkUrl("https://"), "no host");
    CHECK(!checkUrl("https://example.com:99999/"), "port out of range");
    CHECK(!checkUrl("https://exa mple.com/"), "a space");
    const Result<void> refused = checkUrl("http://example.com/?key=abc");
    CHECK(refused.error().find("https only") != std::string::npos, "%s", refused.error().c_str());
    CHECK(refused.error().find("abc") == std::string::npos, "the query is not in the message");

    CHECK(logUrl("https://api.example.com/v1/chat?key=abc#x") == "api.example.com/v1/chat", "%s",
          logUrl("https://api.example.com/v1/chat?key=abc#x").c_str());
    CHECK(logUrl("http://127.0.0.1:8000?x=1") == "127.0.0.1:8000/", "%s", logUrl("http://127.0.0.1:8000?x=1").c_str());
    CHECK(logUrl("https://user:secret@host/") == "host/", "%s", logUrl("https://user:secret@host/").c_str());

    for (const char* name : {"Authorization", "authorization", "Proxy-Authorization", "Cookie", "X-API-Key", "api-key",
                             "X-Goog-Api-Key", "X-Auth-Token", "X-Session-Id"})
        CHECK(isSecretHeader(name), "%s is secret", name);
    for (const char* name : {"Content-Type", "Accept", "User-Agent", "X-Title"})
        CHECK(!isSecretHeader(name), "%s is not secret", name);
}

QUENCH_TEST(net_invalid_requests) {
    Rig rig;
    Request bad = get("https://api.example.com/");
    bad.headers.push_back({"X-Custom", "a\r\nInjected: yes"});
    CHECK(!rig.client.send(bad), "a line break in a header value");
    bad = get("https://api.example.com/");
    bad.headers.push_back({"Bad Name", "x"});
    CHECK(!rig.client.send(bad), "a space in a header name");
    bad = get("https://api.example.com/");
    bad.setBody("x");
    CHECK(!rig.client.send(bad), "a GET with a body");
    bad = get("https://api.example.com/");
    bad.method = "PO ST";
    CHECK(!rig.client.send(bad), "a malformed method");
    CHECK(rig.fake.requests().empty(), "nothing reached the network");
}

QUENCH_TEST(net_fake_complete) {
    Rig rig;
    rig.fake.respond("GET", "https://api.example.com/v1/models",
                     FakeResponse::text(200, R"({"data":[]})", {{"Content-Type", "application/json"}}));
    Seen seen;
    Request request = get("https://api.example.com/v1/models");
    request.headers.push_back({"Authorization", "Bearer test-value"});
    const Result<RequestId> id = rig.client.send(request, seen.handlers());
    REQUIRE(id, "%s", id.error().c_str());
    CHECK(rig.client.active(*id) && rig.client.activeCount() == 1, "active after send");
    CHECK(seen.done == 0 && seen.headers == 0, "nothing is delivered inside send()");
    rig.client.update();
    CHECK(seen.headers == 1 && seen.done == 1, "headers %d, done %d", seen.headers, seen.done);
    CHECK(seen.head.status == 200 && seen.head.body.empty(), "the head has the status, not the body");
    CHECK(seen.last.success(), "success: %s", seen.last.message.c_str());
    CHECK(seen.last.text() == R"({"data":[]})", "body '%.*s'", int(seen.last.text().size()), seen.last.text().data());
    CHECK(seen.last.bytes == 11, "bytes %zu", seen.last.bytes);
    REQUIRE(seen.last.header("content-type"), "the Content-Type header, case-insensitively");
    CHECK(*seen.last.header("content-type") == "application/json", "Content-Type");
    CHECK(!rig.client.active(*id) && rig.client.activeCount() == 0, "not active after its end");
    REQUIRE(rig.fake.last(), "the fake recorded the request");
    const std::string* auth = findHeader(rig.fake.last()->request.headers, "Authorization");
    CHECK(auth && *auth == "Bearer test-value", "the request's header reached the fake");
    rig.client.update();
    CHECK(seen.done == 1, "onDone once");
    CHECK(rig.fake.open() == 0, "nothing open in the fake");
    CHECK(std::string(rig.client.backendName()) == "fake", "backend %s", rig.client.backendName());
}

QUENCH_TEST(net_fake_post_and_status) {
    Rig rig;
    rig.fake.respond("POST", "https://api.example.com/v1/chat/completions", FakeResponse::text(500, "boom"));
    Seen seen;
    Request request;
    request.method = "POST";
    request.url = "https://api.example.com/v1/chat/completions";
    request.headers.push_back({"Content-Type", "application/json"});
    request.setBody(R"({"model":"m"})");
    REQUIRE(rig.client.send(request, seen.handlers()), "send");
    rig.client.update();
    CHECK(seen.done == 1 && seen.last.ok() && !seen.last.success(), "a 500 is a transported answer, not a success");
    CHECK(seen.last.status == 500 && seen.last.text() == "boom", "status %d", seen.last.status);
    REQUIRE(rig.fake.last(), "recorded");
    const Request& sent = rig.fake.last()->request;
    CHECK(sent.method == "POST", "method %s", sent.method.c_str());
    CHECK(std::string(sent.body.begin(), sent.body.end()) == R"({"model":"m"})", "the body reached the fake");
    // An unknown URL: 404 from the fake.
    Seen missing;
    REQUIRE(rig.client.send(get("https://api.example.com/unknown"), missing.handlers()), "send");
    rig.client.update();
    CHECK(missing.last.status == 404, "unknown URL: %d", missing.last.status);
}

QUENCH_TEST(net_fake_chunks_in_order) {
    Rig rig;
    FakeResponse sse;
    sse.headers = {{"Content-Type", "text/event-stream"}};
    sse.headerDelay = 0.1;
    sse.body = {{0.1, "data: one\n\n"}, {0.2, "data: two\n\n"}, {0.0, "data: three\n\n"}, {0.3, "data: [DONE]\n\n"}};
    sse.endDelay = 0.1;
    rig.fake.respond("POST", "https://api.example.com/v1/chat/completions", sse);
    Seen seen;
    Request request;
    request.method = "POST";
    request.url = "https://api.example.com/v1/chat/completions";
    request.setBody("{}");
    request.streamBody = true;
    REQUIRE(rig.client.send(request, seen.handlers(&rig.time)), "send");
    rig.advance(0.05);
    CHECK(seen.headers == 0, "no headers before their delay");
    rig.advance(0.05); // 0.1
    CHECK(seen.headers == 1 && seen.chunks.empty(), "headers at 0.1 s, no chunk yet");
    rig.advance(0.1); // 0.2
    CHECK(seen.chunks.size() == 1, "one chunk at 0.2 s: %zu", seen.chunks.size());
    rig.advance(0.2); // 0.4: two and three
    CHECK(seen.chunks.size() == 3, "three chunks at 0.4 s: %zu", seen.chunks.size());
    CHECK(seen.done == 0, "not done before the last chunk");
    rig.advance(0.3); // 0.7
    CHECK(seen.chunks.size() == 4 && seen.done == 0, "the last chunk, the end still to come");
    rig.advance(0.1); // 0.8
    REQUIRE(seen.done == 1, "done at 0.8 s");
    const std::vector<std::string> expected = {"data: one\n\n", "data: two\n\n", "data: three\n\n", "data: [DONE]\n\n"};
    CHECK(seen.chunks == expected, "the chunks in order");
    CHECK(seen.last.body.empty() && seen.last.bytes == 49, "a streamed body is not kept: %zu bytes", seen.last.bytes);
    CHECK(seen.last.seconds > 0.79 && seen.last.seconds < 0.81, "seconds %.3f", seen.last.seconds);
}

QUENCH_TEST(net_several_at_once) {
    Rig rig;
    FakeResponse slow = FakeResponse::text(200, "slow");
    slow.headerDelay = 1.0;
    rig.fake.respond("GET", "https://a.example/slow", slow);
    rig.fake.respond("GET", "https://a.example/fast", FakeResponse::text(200, "fast"));
    std::vector<std::string> order;
    auto handlers = [&order](std::string name) {
        return Handlers{
            .onDone = [&order, name](const Response& r) { order.push_back(name + ":" + std::string(r.text())); }};
    };
    REQUIRE(rig.client.send(get("https://a.example/slow"), handlers("1")), "send");
    REQUIRE(rig.client.send(get("https://a.example/fast"), handlers("2")), "send");
    REQUIRE(rig.client.send(get("https://a.example/fast?x=1"), handlers("3")), "send (a query string: the bare URL)");
    CHECK(rig.client.activeCount() == 3, "three active");
    rig.advance(0.1);
    CHECK((order == std::vector<std::string>{"2:fast", "3:fast"}), "the fast ones first (%zu)", order.size());
    rig.advance(1.0);
    CHECK(order.size() == 3 && order[2] == "1:slow", "then the slow one");
}

QUENCH_TEST(net_cancel) {
    Rig rig;
    rig.fake.respond("GET", "https://a.example/never", FakeResponse::silence());
    Seen seen;
    const Result<RequestId> id = rig.client.send(get("https://a.example/never"), seen.handlers());
    REQUIRE(id, "send");
    rig.advance(5.0);
    CHECK(seen.done == 0 && rig.client.active(*id), "a silent request stays open");
    CHECK(rig.fake.open() == 1, "open in the fake");
    rig.client.cancel(*id);
    CHECK(seen.done == 0, "onDone is not called inside cancel()");
    CHECK(!rig.client.active(*id), "not active once cancelled");
    CHECK(rig.fake.open() == 0 && rig.fake.last()->cancelled, "the cancel reached the fake");
    rig.client.update();
    CHECK(seen.done == 1 && seen.last.error == ErrorKind::Cancelled, "Cancelled from the next update (%s)",
          errorKindName(seen.last.error));
    rig.client.cancel(*id); // finished: ignored
    rig.client.cancel(12345);
    rig.client.update();
    CHECK(seen.done == 1, "once");

    // Cancel from a chunk handler: nothing more of it, its end in the same update.
    FakeResponse pieces;
    pieces.body = {{0.0, "a"}, {0.0, "b"}, {0.0, "c"}};
    rig.fake.respond("GET", "https://a.example/pieces", pieces);
    Seen inside;
    RequestId running = 0;
    Request streamed = get("https://a.example/pieces");
    streamed.streamBody = true;
    Handlers h = inside.handlers();
    h.onChunk = [&](std::span<const std::uint8_t> c) {
        inside.chunks.emplace_back(c.begin(), c.end());
        rig.client.cancel(running);
    };
    const Result<RequestId> second = rig.client.send(streamed, h);
    REQUIRE(second, "send");
    running = *second;
    rig.client.update();
    CHECK(inside.chunks.size() == 1, "one chunk, then cancelled: %zu", inside.chunks.size());
    CHECK(inside.done == 1 && inside.last.error == ErrorKind::Cancelled, "its end in the same update");

    // The destructor cancels without handlers.
    int calls = 0;
    {
        Rig other;
        other.fake.respond("GET", "https://a.example/never", FakeResponse::silence());
        REQUIRE(other.client.send(get("https://a.example/never"), {.onDone = [&](const Response&) { ++calls; }}),
                "send");
    }
    CHECK(calls == 0, "no handler from the destructor");
}

QUENCH_TEST(net_timeout) {
    Rig rig;
    rig.fake.respond("GET", "https://a.example/never", FakeResponse::silence());
    Seen seen;
    Request request = get("https://a.example/never");
    request.timeoutSeconds = 2.0;
    REQUIRE(rig.client.send(request, seen.handlers()), "send");
    rig.advance(1.9);
    CHECK(seen.done == 0, "open before its timeout");
    rig.advance(0.2);
    REQUIRE(seen.done == 1, "ended at its timeout");
    CHECK(seen.last.error == ErrorKind::Timeout, "Timeout: %s", errorKindName(seen.last.error));
    CHECK(seen.last.message.find("2 s") != std::string::npos, "message '%s'", seen.last.message.c_str());
    CHECK(rig.fake.last()->cancelled && rig.fake.open() == 0, "the backend's request was cancelled");

    // A body that keeps coming past the timeout.
    FakeResponse dribble;
    for (int i = 0; i < 10; ++i) dribble.body.push_back({0.5, "x"});
    rig.fake.respond("GET", "https://a.example/dribble", dribble);
    Seen slow;
    request = get("https://a.example/dribble");
    request.timeoutSeconds = 1.2;
    REQUIRE(rig.client.send(request, slow.handlers()), "send");
    for (int i = 0; i < 10; ++i) rig.advance(0.25);
    CHECK(slow.done == 1 && slow.last.error == ErrorKind::Timeout && slow.last.status == 200,
          "timeout during the body, after the headers (%s)", errorKindName(slow.last.error));

    // No timeout: open as long as it takes.
    Seen patient;
    request = get("https://a.example/never");
    request.timeoutSeconds = 0.0;
    REQUIRE(rig.client.send(request, patient.handlers()), "send");
    rig.advance(1000.0);
    CHECK(patient.done == 0, "no timeout with 0");
}

QUENCH_TEST(net_size_limit) {
    Rig rig;
    FakeResponse big;
    for (int i = 0; i < 10; ++i) big.body.push_back({0.1, std::string(100, 'a')});
    rig.fake.respond("GET", "https://a.example/big", big);
    Seen seen;
    Request request = get("https://a.example/big");
    request.maxResponseBytes = 450;
    request.streamBody = true;
    REQUIRE(rig.client.send(request, seen.handlers()), "send");
    for (int i = 0; i < 20; ++i) rig.advance(0.1);
    CHECK(seen.done == 1 && seen.last.error == ErrorKind::TooLarge, "TooLarge: %s", errorKindName(seen.last.error));
    CHECK(seen.chunks.size() == 4, "the chunks under the limit were delivered, not the one over it: %zu",
          seen.chunks.size());
    CHECK(rig.fake.last()->cancelled, "stopped while receiving");

    // Exactly at the limit is fine; a Content-Length over it fails before the body.
    request.streamBody = false;
    request.maxResponseBytes = 1000;
    Seen exact;
    REQUIRE(rig.client.send(request, exact.handlers()), "send");
    for (int i = 0; i < 20; ++i) rig.advance(0.1);
    CHECK(exact.last.success() && exact.last.body.size() == 1000, "1000 of 1000 bytes: %s",
          errorKindName(exact.last.error));

    FakeResponse declared = FakeResponse::text(200, "", {{"Content-Length", "5000000"}});
    declared.body = {{1.0, "x"}};
    rig.fake.respond("GET", "https://a.example/declared", declared);
    Seen early;
    request = get("https://a.example/declared");
    request.maxResponseBytes = 1u << 20;
    REQUIRE(rig.client.send(request, early.handlers()), "send");
    rig.advance(0.0);
    CHECK(early.done == 1 && early.last.error == ErrorKind::TooLarge && early.headers == 0,
          "a declared length over the limit ends it at the headers (%s, %d)", errorKindName(early.last.error),
          early.headers);
}

QUENCH_TEST(net_https_rule_on_send) {
    Rig rig;
    const Result<RequestId> plain = rig.client.send(get("http://api.example.com/v1"));
    CHECK(!plain && plain.error().find("https only") != std::string::npos, "plain http refused: %s",
          plain.error().c_str());
    CHECK(rig.fake.requests().empty(), "a refused request does not reach the network");
    CHECK(rig.client.send(get("http://127.0.0.1:8080/v1")), "the loopback address");
    CHECK(rig.client.send(get("http://localhost:11434/api")), "localhost");

    FakeNetwork fake;
    Client insecure(ClientDesc{.fake = &fake, .allowInsecure = true});
    CHECK(insecure.send(get("http://api.example.com/v1")), "allowInsecure");
}

QUENCH_TEST(net_redirects) {
    Rig rig;
    rig.fake.respond("GET", "https://a.example/same", FakeResponse::redirect(302, "/target"));
    rig.fake.respond("GET", "https://a.example/target", FakeResponse::text(200, "same"));
    rig.fake.respond("GET", "https://a.example/other", FakeResponse::redirect(307, "https://b.example/target"));
    rig.fake.respond("GET", "https://b.example/target", FakeResponse::text(200, "other"));
    rig.fake.respond("GET", "https://a.example/down", FakeResponse::redirect(301, "http://localhost/x"));
    rig.fake.respond("GET", "https://a.example/loop", FakeResponse::redirect(302, "/loop"));
    rig.fake.respond("POST", "https://a.example/see", FakeResponse::redirect(303, "/target"));

    auto request = [](std::string url) {
        Request r = get(std::move(url));
        r.headers = {{"Authorization", "Bearer value"}, {"X-API-Key", "value"}, {"X-Title", "kept"}};
        return r;
    };
    Seen same;
    REQUIRE(rig.client.send(request("https://a.example/same"), same.handlers()), "send");
    rig.client.update();
    CHECK(same.last.text() == "same", "followed on the same origin");
    REQUIRE(rig.fake.requests().size() == 2, "two requests: %zu", rig.fake.requests().size());
    CHECK(findHeader(rig.fake.requests()[1].request.headers, "Authorization"), "Authorization kept on the same origin");

    rig.fake.clearRequests();
    Seen other;
    REQUIRE(rig.client.send(request("https://a.example/other"), other.handlers()), "send");
    rig.client.update();
    CHECK(other.last.text() == "other", "followed to another origin");
    REQUIRE(rig.fake.requests().size() == 2, "two requests");
    const Headers& moved = rig.fake.requests()[1].request.headers;
    CHECK(!findHeader(moved, "Authorization") && !findHeader(moved, "X-API-Key"), "secrets dropped on another origin");
    CHECK(findHeader(moved, "X-Title"), "other headers kept");
    CHECK(rig.fake.requests()[1].request.url == "https://b.example/target", "the new URL");

    Seen down;
    REQUIRE(rig.client.send(request("https://a.example/down"), down.handlers()), "send");
    rig.client.update();
    CHECK(down.last.error == ErrorKind::NotAllowed, "https to plain http refused: %s", down.last.message.c_str());

    Seen loop;
    REQUIRE(rig.client.send(request("https://a.example/loop"), loop.handlers()), "send");
    rig.client.update();
    CHECK(loop.last.error == ErrorKind::Failed && loop.last.message.find("redirects") != std::string::npos,
          "a redirect loop ends: %s", loop.last.message.c_str());

    rig.fake.clearRequests();
    Seen see;
    Request post = request("https://a.example/see");
    post.method = "POST";
    post.setBody("{}");
    REQUIRE(rig.client.send(post, see.handlers()), "send");
    rig.client.update();
    REQUIRE(rig.fake.requests().size() == 2, "two requests");
    CHECK(rig.fake.requests()[1].request.method == "GET" && rig.fake.requests()[1].request.body.empty(),
          "303 turns into a GET without a body");
}

QUENCH_TEST(net_errors) {
    Rig rig;
    rig.fake.respond(
        "GET", "https://a.example/offline",
        FakeResponse::failure(ErrorKind::NoNetwork, "The Internet connection appears to be offline.", 0.2));
    rig.fake.respond("GET", "https://a.example/tls", FakeResponse::failure(ErrorKind::Tls, "bad certificate"));
    Seen offline, tls;
    REQUIRE(rig.client.send(get("https://a.example/offline"), offline.handlers()), "send");
    REQUIRE(rig.client.send(get("https://a.example/tls"), tls.handlers()), "send");
    rig.advance(0.1);
    CHECK(tls.done == 1 && tls.last.error == ErrorKind::Tls && tls.last.status == 0, "Tls");
    CHECK(offline.done == 0, "after its delay");
    rig.advance(0.1);
    CHECK(offline.done == 1 && offline.last.error == ErrorKind::NoNetwork, "NoNetwork");
    CHECK(offline.last.message == "The Internet connection appears to be offline.", "message %s",
          offline.last.message.c_str());
    CHECK(std::string(errorKindName(ErrorKind::TooLarge)) == "too large", "names");
}

QUENCH_TEST(net_log_redaction) {
    Rig rig;
    rig.fake.respond("POST", "https://api.example.com/v1/chat", FakeResponse::text(200, "ok"));
    rig.fake.respond(
        "GET", "https://api.example.com/v1/fail",
        FakeResponse::failure(ErrorKind::Failed, "failed for https://api.example.com/v1/fail?key=qwerty42"));
    LogCapture capture;
    Request request;
    request.method = "POST";
    request.url = "https://api.example.com/v1/chat?key=qwerty42&user=someone";
    request.headers = {{"Authorization", "Bearer sk-abcdef123"}, {"X-API-Key", "zxcvb987"}, {"Cookie", "id=cookie55"}};
    request.setBody("body-text-77");
    Seen seen;
    REQUIRE(rig.client.send(request, seen.handlers()), "send");
    rig.advance(0.25);
    Seen failed;
    REQUIRE(rig.client.send(get("https://api.example.com/v1/fail?key=qwerty42"), failed.handlers()), "send");
    rig.client.update();
    CHECK(rig.client.send(get("http://api.example.com/v1/x?key=qwerty42")).ok() == false, "refused");
    const std::string all = capture.all();
    CHECK(all.find("net: POST api.example.com/v1/chat: 200, 2 bytes, 250 ms") != std::string::npos,
          "method, host and path, status, bytes, milliseconds:\n%s", all.c_str());
    CHECK(all.find("GET api.example.com/v1/fail: failed") != std::string::npos, "the failure:\n%s", all.c_str());
    for (const char* secret : {"sk-abcdef123", "zxcvb987", "cookie55", "qwerty42", "someone", "body-text-77"})
        CHECK(all.find(secret) == std::string::npos, "'%s' is not in the log:\n%s", secret, all.c_str());
    CHECK(failed.last.message.find("qwerty42") == std::string::npos, "nor in the response's message: %s",
          failed.last.message.c_str());
}

QUENCH_TEST(net_handlers_send_again) {
    Rig rig;
    rig.fake.respond("GET", "https://a.example/one", FakeResponse::text(200, "1"));
    rig.fake.respond("GET", "https://a.example/two", FakeResponse::text(200, "2"));
    std::string got;
    REQUIRE(rig.client.send(get("https://a.example/one"),
                            {.onDone =
                                 [&](const Response& r) {
                                     got += r.text();
                                     const Result<RequestId> next =
                                         rig.client.send(get("https://a.example/two"),
                                                         {.onDone = [&](const Response& r2) { got += r2.text(); }});
                                     CHECK(next, "send from a handler");
                                 }}),
            "send");
    rig.client.update();
    CHECK(got == "1" && rig.client.activeCount() == 1, "the second is running: '%s'", got.c_str());
    rig.client.update();
    CHECK(got == "12", "then done: '%s'", got.c_str());
}

QUENCH_TEST(net_no_backend) {
    Client none(ClientDesc{.noBackend = true});
    CHECK(!none.available() && std::string(none.backendName()) == "none", "the none backend");
    const Result<RequestId> sent = none.send(get("https://api.example.com/"));
    CHECK(!sent && sent.error() == "no network backend on this platform", "fails at once: %s", sent.error().c_str());
    none.update();
    CHECK(none.activeCount() == 0, "nothing active");

    // The platform's backend of this build.
    Client platform;
#if QUENCH_PLATFORM_APPLE
    CHECK(std::string(platform.backendName()) == "nsurlsession", "%s", platform.backendName());
#else
    CHECK(std::string(platform.backendName()) == "none" && !platform.available(), "%s", platform.backendName());
    CHECK(!platform.send(get("https://api.example.com/")), "no backend on this platform");
#endif
    CHECK(pageOrigin().empty(), "no page outside a browser");
}
