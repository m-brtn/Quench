// quench_net's NSURLSession backend against the local test server (tests/net/net_test_server.py, which starts this
// executable with QUENCH_TEST_NET_URL=http://127.0.0.1:<port>): plain HTTP to the loopback address, the rule's one
// exception. macOS only; a case run without the server skips.

#include "Test.h"

#include <quench/net/Http.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace quench;
using namespace quench::net;

namespace {

std::string server() {
    const char* url = std::getenv("QUENCH_TEST_NET_URL");
    return url ? url : "";
}

#define NEED_SERVER()                                                                                                  \
    if (server().empty()) QUENCH_SKIP("no test server (run through tests/net/net_test_server.py --run)")

double seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Run {
    Response head;
    Response last;
    int headers = 0;
    int done = 0;
    std::vector<std::string> chunks;
    std::vector<double> chunkTimes;
    double doneTime = 0.0;
    RequestId id = 0;
};

// Sends and calls update() every 2 ms until onDone or `limit` seconds; `during` runs after each update.
template <class During>
bool run(Client& client, Request request, Run& out, double limit, During during) {
    const Result<RequestId> id =
        client.send(std::move(request), {
                                            .onHeaders =
                                                [&out](const Response& r) {
                                                    ++out.headers;
                                                    out.head = r;
                                                },
                                            .onChunk =
                                                [&out](std::span<const std::uint8_t> c) {
                                                    out.chunks.emplace_back(c.begin(), c.end());
                                                    out.chunkTimes.push_back(seconds());
                                                },
                                            .onDone =
                                                [&out](const Response& r) {
                                                    ++out.done;
                                                    out.last = r;
                                                    out.doneTime = seconds();
                                                },
                                        });
    if (!id) {
        out.last.error = ErrorKind::InvalidRequest;
        out.last.message = id.error();
        return false;
    }
    out.id = *id;
    const double end = seconds() + limit;
    while (out.done == 0 && seconds() < end) {
        client.update();
        during(out);
        if (out.done == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return out.done == 1;
}

bool run(Client& client, Request request, Run& out, double limit = 10.0) {
    return run(client, std::move(request), out, limit, [](Run&) {});
}

Request get(const std::string& path) {
    Request r;
    r.url = server() + path;
    return r;
}

} // namespace

QUENCH_TEST(apple_get) {
    NEED_SERVER();
    Client client;
    Run r;
    REQUIRE(run(client, get("/hello"), r), "done: %s", r.last.message.c_str());
    CHECK(r.last.success() && r.last.status == 200, "status %d, %s", r.last.status, r.last.message.c_str());
    CHECK(r.last.text() == "hello", "body '%.*s'", int(r.last.text().size()), r.last.text().data());
    CHECK(r.headers == 1 && r.head.status == 200, "headers first");
    const std::string* test = r.last.header("x-test");
    CHECK(test && *test == "yes", "a response header");
    CHECK(r.last.seconds > 0.0 && r.last.seconds < 5.0, "seconds %.3f", r.last.seconds);
}

QUENCH_TEST(apple_post_echo) {
    NEED_SERVER();
    Client client;
    Request request = get("/echo");
    request.method = "POST";
    request.headers = {{"Content-Type", "application/json"}, {"X-Custom", "custom value"}};
    std::string body = R"({"messages":[{"role":"user","content":"hi"}]})";
    for (int i = 0; i < 2000; ++i) body += ' ';
    request.setBody(body);
    Run r;
    REQUIRE(run(client, request, r), "done: %s", r.last.message.c_str());
    CHECK(r.last.success(), "%d %s", r.last.status, r.last.message.c_str());
    CHECK(std::string(r.last.text()) == body, "the body came back (%zu of %zu bytes)", r.last.body.size(), body.size());
    const std::string* method = r.last.header("X-Echo-Method");
    const std::string* custom = r.last.header("X-Echo-Custom");
    const std::string* type = r.last.header("Content-Type");
    CHECK(method && *method == "POST", "method");
    CHECK(custom && *custom == "custom value", "the request header reached the server");
    CHECK(type && *type == "application/json", "Content-Type");
}

QUENCH_TEST(apple_status_codes) {
    NEED_SERVER();
    Client client;
    for (int code : {404, 500}) {
        Run r;
        REQUIRE(run(client, get("/status/" + std::to_string(code)), r), "done");
        CHECK(r.last.ok() && !r.last.success() && r.last.status == code, "status %d for %d (%s)", r.last.status, code,
              r.last.message.c_str());
        CHECK(r.last.text() == "status " + std::to_string(code), "body");
    }
}

QUENCH_TEST(apple_chunks_as_they_arrive) {
    NEED_SERVER();
    Client client;
    Request request = get("/chunked?n=5&gap=0.25");
    request.streamBody = true;
    Run r;
    const double start = seconds();
    REQUIRE(run(client, request, r), "done: %s", r.last.message.c_str());
    CHECK(r.last.success(), "%s", r.last.message.c_str());
    std::string all;
    for (const std::string& c : r.chunks) all += c;
    CHECK(all.size() == 5 * 64 && r.last.bytes == 5 * 64, "320 bytes: %zu", all.size());
    CHECK(all.starts_with("data: 0 ") && all.find("data: 4 ") == 4 * 64, "in order");
    CHECK(r.chunks.size() >= 4, "several pieces: %zu", r.chunks.size());
    REQUIRE(!r.chunkTimes.empty(), "chunks");
    const double first = r.chunkTimes.front() - start, total = r.doneTime - start;
    CHECK(total >= 0.95, "the answer took about a second: %.3f s", total);
    CHECK(r.doneTime - r.chunkTimes.front() >= 0.75, "the first piece came %.3f s before the end (at %.3f of %.3f s)",
          r.doneTime - r.chunkTimes.front(), first, total);
    std::printf("  first piece at %.3f s, %zu pieces, the end at %.3f s\n", first, r.chunks.size(), total);
}

QUENCH_TEST(apple_timeout) {
    NEED_SERVER();
    Client client;
    Request request = get("/slow?seconds=3");
    request.timeoutSeconds = 0.5;
    Run r;
    const double start = seconds();
    REQUIRE(run(client, request, r, 5.0), "done");
    const double took = seconds() - start;
    CHECK(r.last.error == ErrorKind::Timeout, "Timeout: %s (%s)", errorKindName(r.last.error), r.last.message.c_str());
    CHECK(took >= 0.45 && took < 1.5, "ended at its timeout: %.3f s", took);
}

QUENCH_TEST(apple_cancel_in_the_middle) {
    NEED_SERVER();
    Client client;
    Request request = get("/stall");
    request.streamBody = true;
    Run r;
    bool cancelled = false;
    const double start = seconds();
    REQUIRE(run(client, request, r, 5.0,
                [&](Run& out) {
                    if (!cancelled && !out.chunks.empty()) {
                        client.cancel(out.id);
                        cancelled = true;
                    }
                }),
            "done");
    CHECK(cancelled && r.chunks.size() == 1 && r.chunks[0] == "first piece", "the first piece, then the cancel");
    CHECK(r.last.error == ErrorKind::Cancelled, "Cancelled: %s", errorKindName(r.last.error));
    CHECK(seconds() - start < 3.0, "without waiting for the server");
    // Nothing of it arrives afterwards.
    for (int i = 0; i < 20; ++i) {
        client.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(r.done == 1 && r.chunks.size() == 1, "nothing after the cancel");
}

QUENCH_TEST(apple_redirects) {
    NEED_SERVER();
    Client client;
    const Headers secrets = {{"Authorization", "Bearer local-test"}, {"X-API-Key", "local-test"}};
    // The same origin: the headers stay.
    Request same = get("/redirect?to=/secrets");
    same.headers = secrets;
    Run r;
    REQUIRE(run(client, same, r), "done");
    CHECK(r.last.success() && r.last.text() == "authorization=present;x-api-key=present;cookie=absent", "same: '%.*s'",
          int(r.last.text().size()), r.last.text().data());
    // Another origin (localhost instead of 127.0.0.1, the same server): Authorization and the key stay behind.
    const std::string port = server().substr(server().rfind(':') + 1);
    Request other = get("/redirect?to=http://localhost:" + port + "/secrets");
    other.headers = secrets;
    Run o;
    REQUIRE(run(client, other, o), "done");
    CHECK(o.last.success() && o.last.text() == "authorization=absent;x-api-key=absent;cookie=absent",
          "other: '%.*s' (%s)", int(o.last.text().size()), o.last.text().data(), o.last.message.c_str());
    // A redirect to plain http elsewhere is refused by the rule.
    Run refused;
    REQUIRE(run(client, get("/redirect?to=http://example.com/"), refused), "done");
    CHECK(refused.last.error == ErrorKind::NotAllowed, "refused: %s (%s)", errorKindName(refused.last.error),
          refused.last.message.c_str());
}

QUENCH_TEST(apple_size_limit) {
    NEED_SERVER();
    Client client;
    Request declared = get("/big?bytes=200000");
    declared.maxResponseBytes = 100000;
    Run d;
    REQUIRE(run(client, declared, d), "done");
    CHECK(d.last.error == ErrorKind::TooLarge, "a Content-Length over the limit: %s", errorKindName(d.last.error));

    Request streamed = get("/big?bytes=4000000&chunked=1");
    streamed.maxResponseBytes = 100000;
    streamed.streamBody = true;
    Run s;
    REQUIRE(run(client, streamed, s), "done");
    CHECK(s.last.error == ErrorKind::TooLarge, "a chunked body over the limit: %s", errorKindName(s.last.error));
    std::size_t delivered = 0;
    for (const std::string& c : s.chunks) delivered += c.size();
    CHECK(delivered <= 100000, "no more than the limit delivered: %zu", delivered);

    Request fits = get("/big?bytes=100000");
    fits.maxResponseBytes = 100000;
    Run f;
    REQUIRE(run(client, fits, f), "done");
    CHECK(f.last.success() && f.last.body.size() == 100000, "exactly the limit: %s", f.last.message.c_str());
}

QUENCH_TEST(apple_no_cookies) {
    NEED_SERVER();
    Client client;
    Run set;
    REQUIRE(run(client, get("/set-cookie"), set), "done");
    CHECK(set.last.success(), "set");
    Run check;
    REQUIRE(run(client, get("/secrets"), check), "done");
    CHECK(check.last.text() == "authorization=absent;x-api-key=absent;cookie=absent", "no cookie sent back: '%.*s'",
          int(check.last.text().size()), check.last.text().data());
}

QUENCH_TEST(apple_several_at_once) {
    NEED_SERVER();
    Client client;
    int done = 0;
    std::vector<std::string> order;
    auto handlers = [&](std::string name) {
        return Handlers{.onDone = [&, name](const Response& r) {
            ++done;
            if (r.success()) order.push_back(name);
        }};
    };
    REQUIRE(client.send(get("/slow?seconds=0.6"), handlers("slow")), "send");
    REQUIRE(client.send(get("/hello"), handlers("fast")), "send");
    REQUIRE(client.send(get("/chunked?n=3&gap=0.1"), handlers("chunked")), "send");
    CHECK(client.activeCount() == 3, "three at once");
    const double end = seconds() + 5.0;
    while (done < 3 && seconds() < end) {
        client.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(done == 3 && order.size() == 3, "all three: %d", done);
    CHECK(!order.empty() && order.back() == "slow", "the slow one last");
}

QUENCH_TEST(apple_connection_refused) {
    NEED_SERVER();
    Client client;
    Request request;
    request.url = "http://127.0.0.1:9/"; // the discard port: nothing listens there
    request.timeoutSeconds = 5.0;
    Run r;
    REQUIRE(run(client, request, r, 6.0), "done");
    CHECK(r.last.error == ErrorKind::NoNetwork, "NoNetwork: %s (%s)", errorKindName(r.last.error),
          r.last.message.c_str());
}
