#pragma once

// Scenario commands and values for HTTP requests through quench::net (examples/hello/scenarios/net_web.txt, run in a
// browser by tools/check.sh step web against the server of tools/web_check.mjs). A path is sent to the page's origin
// (net::pageOrigin()); outside a browser it must be an absolute URL.
//   net_request NAME METHOD PATH [BODY]   a request; the body is kept to compare with the answer
//   net_stream NAME PATH                  a GET whose body arrives in chunks (Request::streamBody)
//   net_cancel NAME
// Values: net_state:NAME (0 running, 1 ended, 2 ended with an error), net_status:NAME, net_bytes:NAME (so far),
// net_chunks:NAME, net_error:NAME (net::ErrorKind as a number), net_echo:NAME (1: the answer's body is the body
// sent), net_spread:NAME (seconds from the first chunk to the end).

#include <quench/app/App.h>
#include <quench/app/Scenario.h>
#include <quench/net/Http.h>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

class NetCheck {
public:
    void update() { m_client.update(); }

    quench::app::ScenarioExec command(const quench::app::ScenarioCommand& c) {
        using quench::app::ScenarioExec;
        namespace net = quench::net;
        const bool request = c.op == "net_request", stream = c.op == "net_stream";
        if (c.op == "net_cancel" && c.args.size() == 1) {
            const auto it = m_requests.find(c.args[0]);
            if (it == m_requests.end()) return ScenarioExec::Failed;
            m_client.cancel(it->second.id);
            return ScenarioExec::Done;
        }
        if (!request && !stream) return ScenarioExec::Unknown;
        if ((request && (c.args.size() < 3 || c.args.size() > 4)) || (stream && c.args.size() != 2))
            return ScenarioExec::Failed;
        const std::string& path = request ? c.args[2] : c.args[1];
        net::Request r;
        r.method = request ? c.args[1] : "GET";
        r.url = path.starts_with('/') ? net::pageOrigin() + path : path;
        r.streamBody = stream;
        r.timeoutSeconds = 20.0;
        if (request && c.args.size() == 4) r.setBody(c.args[3]);
        State& s = m_requests[c.args[0]];
        s = State{.sent = std::string(r.body.begin(), r.body.end())};
        const quench::Result<net::RequestId> id =
            m_client.send(std::move(r), {
                                            .onChunk =
                                                [&s](std::span<const std::uint8_t> chunk) {
                                                    if (s.chunks++ == 0) s.firstChunk = now();
                                                    s.bytes += chunk.size();
                                                },
                                            .onDone =
                                                [&s](const net::Response& response) {
                                                    s.state = response.ok() ? 1 : 2;
                                                    s.status = response.status;
                                                    s.error = int(response.error);
                                                    if (!response.body.empty()) s.bytes = response.bytes;
                                                    s.echo = response.text() == s.sent;
                                                    s.spread = s.chunks > 0 ? now() - s.firstChunk : 0.0;
                                                },
                                        });
        if (!id) return ScenarioExec::Failed;
        s.id = *id;
        return ScenarioExec::Done;
    }

    std::optional<double> value(std::string_view name) const {
        const std::size_t colon = name.find(':');
        if (!name.starts_with("net_") || colon == std::string_view::npos) return std::nullopt;
        const auto it = m_requests.find(std::string(name.substr(colon + 1)));
        if (it == m_requests.end()) return std::nullopt;
        const State& s = it->second;
        const std::string_view what = name.substr(0, colon);
        if (what == "net_state") return double(s.state);
        if (what == "net_status") return double(s.status);
        if (what == "net_bytes") return double(s.bytes);
        if (what == "net_chunks") return double(s.chunks);
        if (what == "net_error") return double(s.error);
        if (what == "net_echo") return s.echo ? 1.0 : 0.0;
        if (what == "net_spread") return s.spread;
        return std::nullopt;
    }

private:
    struct State {
        quench::net::RequestId id = 0;
        std::string sent;
        int state = 0, status = 0, error = 0, chunks = 0;
        std::size_t bytes = 0;
        bool echo = false;
        double firstChunk = 0.0, spread = 0.0;
    };

    static double now() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    quench::net::Client m_client;
    std::map<std::string, State> m_requests; // std::map: the handlers keep references to their State
};
