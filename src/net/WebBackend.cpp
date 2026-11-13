// The web backend of quench_net (browser builds): the browser's fetch with an AbortController per request (cancel,
// the client's timeout, the size limit) and a ReadableStream reader for the body's pieces. Its promises queue events
// in JavaScript (Module.quenchNet); poll() takes them on the main thread during update(), so nothing calls into the
// program from a browser callback and nothing waits through Asyncify. Cookies are not sent or stored (credentials
// "omit"), nothing is cached ("no-store"); the browser follows redirects itself and drops Authorization on a
// redirect to another origin (the Fetch standard); the client checks the final URL against the HTTPS rule.
//
// No arrow functions or strict comparisons (=== and !==) in the JavaScript below: clang-format would put spaces into
// them (src/app/SdlFileSystem.cpp).

#include "net/Backend.h"

#include <emscripten/emscripten.h>

#include <cstdlib>
#include <string>

EM_JS_DEPS(quench_net, "$UTF8ToString,$stringToNewUTF8");

// Event types: 1 headers (status; bytes: the final URL, then "name\nvalue\n" lines), 2 a piece of the body, 3 the
// end, 4 a failure (status: 1 offline, 2 another failure, 3 over the size limit; bytes: the message).
EM_JS(void, quench_net_web_start,
      (int owner, double id, const char* method, const char* url, const char* headers, const void* body, int bodySize,
       double maxBytes),
      {
          var net = Module["quenchNet"];
          if (!net) {
              net = {owners : {}};
              Module["quenchNet"] = net;
          }
          var o = net.owners[owner];
          if (!o) {
              o = {queue : [], requests : {}};
              net.owners[owner] = o;
          }
          var encoder = new TextEncoder();
          function push(type, status, bytes) {
              o.queue.push({id : id, type : type, status : status, bytes : bytes || new Uint8Array(0)});
          }
          var controller = new AbortController();
          var entry = {controller : controller, done : false};
          o.requests[id] = entry;
          function end() {
              entry.done = true;
              delete o.requests[id];
          }
          function fail(kind, message) {
              if (entry.done) return;
              end();
              push(4, kind, encoder.encode(String(message)));
          }
          function offline() { return typeof navigator != "undefined" && navigator.onLine == false; }
          var init = {
              method : UTF8ToString(method),
              headers : new Headers(),
              credentials : "omit",
              cache : "no-store",
              redirect : "follow",
              signal : controller.signal
          };
          var list = UTF8ToString(headers).split("\n");
          var received = 0;
          try {
              for (var i = 0; i + 1 < list.length; i += 2) init.headers.append(list[i], list[i + 1]);
              if (bodySize > 0) init.body = HEAPU8.slice(body, body + bodySize);
              fetch(UTF8ToString(url), init)
                  .then(
                      function(response) {
                          if (entry.done) return;
                          var block = response.url + "\n";
                          response.headers.forEach(function(value, name) { block += name + "\n" + value + "\n"; });
                          push(1, response.status, encoder.encode(block));
                          if (!response.body) {
                              end();
                              push(3, 0, null);
                              return;
                          }
                          var reader = response.body.getReader();
                          function pump() {
                              reader.read().then(
                                  function(result) {
                                      if (entry.done) {
                                          reader.cancel().catch(function(){});
                                          return;
                                      }
                                      if (result.done) {
                                          end();
                                          push(3, 0, null);
                                          return;
                                      }
                                      received += result.value.length;
                                      if (received > maxBytes) {
                                          fail(3, "the body is over the limit of " + maxBytes + " bytes");
                                          controller.abort();
                                          return;
                                      }
                                      push(2, 0, result.value);
                                      pump();
                                  },
                                  function(error) { fail(offline() ? 1 : 2, "the body stopped: " + error); });
                          }
                          pump();
                      },
                      function(error) {
                          fail(offline() ? 1 : 2,
                               "the browser's fetch failed (offline, refused, CORS or TLS): " + error);
                      });
          } catch (e) { fail(2, e); }
      });

EM_JS(void, quench_net_web_cancel, (int owner, double id), {
    var net = Module["quenchNet"];
    var o = net ? net.owners[owner] : null;
    if (!o) return;
    var entry = o.requests[id];
    if (entry) {
        entry.done = true;
        delete o.requests[id];
        try {
            entry.controller.abort();
        } catch (e) {}
    }
    o.queue = o.queue.filter(function(e) { return e.id != id; });
});

EM_JS(void, quench_net_web_close, (int owner), {
    var net = Module["quenchNet"];
    var o = net ? net.owners[owner] : null;
    if (!o) return;
    for (var id in o.requests) {
        o.requests[id].done = true;
        try {
            o.requests[id].controller.abort();
        } catch (e) {}
    }
    delete net.owners[owner];
});

// 1 and the head event's fields, or 0 when nothing is queued.
EM_JS(int, quench_net_web_next, (int owner, double* id, int* type, int* status, int* size), {
    var net = Module["quenchNet"];
    var o = net ? net.owners[owner] : null;
    if (!o || o.queue.length == 0) return 0;
    var e = o.queue[0];
    HEAPF64[id >> 3] = e.id;
    HEAP32[type >> 2] = e.type;
    HEAP32[status >> 2] = e.status;
    HEAP32[size >> 2] = e.bytes.length;
    return 1;
});

// Copies the head event's bytes to `out` and removes it.
EM_JS(void, quench_net_web_take, (int owner, void* out), {
    var o = Module["quenchNet"].owners[owner];
    var e = o.queue.shift();
    if (e.bytes.length > 0) HEAPU8.set(e.bytes, out);
});

EM_JS(char*, quench_net_web_origin, (), {
    var origin = typeof location != "undefined" ? String(location.origin) : "";
    return stringToNewUTF8(origin);
});

namespace quench::net::detail {
namespace {

int g_nextOwner = 1;

class WebBackend final : public Backend {
public:
    WebBackend() : m_owner(g_nextOwner++) {}
    ~WebBackend() override { quench_net_web_close(m_owner); }

    const char* name() const override { return "fetch"; }

    Result<void> start(RequestId id, const Request& request, bool, double) override {
        const std::string headers = headerBlock(request.headers);
        quench_net_web_start(m_owner, double(id), request.method.c_str(), request.url.c_str(), headers.c_str(),
                             request.body.data(), int(request.body.size()), double(request.maxResponseBytes));
        return {};
    }

    void cancel(RequestId id) override { quench_net_web_cancel(m_owner, double(id)); }

    void poll(double, std::vector<Event>& events) override {
        double id = 0.0;
        int type = 0, status = 0, size = 0;
        while (quench_net_web_next(m_owner, &id, &type, &status, &size)) {
            std::vector<std::uint8_t> bytes(std::size_t(size > 0 ? size : 0));
            quench_net_web_take(m_owner, bytes.data());
            Event e;
            e.id = RequestId(id);
            if (type == 1) {
                const std::string_view block(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                const std::size_t line = block.find('\n');
                e.type = Event::Type::Headers;
                e.status = status;
                e.finalUrl = std::string(block.substr(0, line));
                e.headers = parseHeaderBlock(line == std::string_view::npos ? "" : block.substr(line + 1));
            } else if (type == 2) {
                e.type = Event::Type::Data;
                e.bytes = std::move(bytes);
            } else if (type == 3) {
                e.type = Event::Type::Done;
            } else {
                e.type = Event::Type::Failed;
                e.error = status == 1 ? ErrorKind::NoNetwork : status == 3 ? ErrorKind::TooLarge : ErrorKind::Failed;
                e.message.assign(bytes.begin(), bytes.end());
            }
            events.push_back(std::move(e));
        }
    }

private:
    int m_owner;
};

} // namespace

std::unique_ptr<Backend> createPlatformBackend() { return std::make_unique<WebBackend>(); }

std::string platformPageOrigin() {
    char* origin = quench_net_web_origin();
    std::string out = origin ? origin : "";
    std::free(origin);
    return out;
}

} // namespace quench::net::detail
