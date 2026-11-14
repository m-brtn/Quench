// The WebSocket platform backend where there is none yet (the browser, Linux, Windows, the Node.js test build):
// connect() fails at once with "no WebSocket backend on this platform". The browser's WebSocket is left out on
// purpose: it cannot send an Authorization header, and OpenAI's Live API, the reason this exists, documents no other
// way to authenticate a browser's WebSocket (a browser reaches it by WebRTC instead; docs/live.md).

#include "net/WsBackend.h"

namespace quench::net::detail {

std::unique_ptr<WsBackend> createPlatformWsBackend() { return createNoneWsBackend(); }

} // namespace quench::net::detail
