// The platform backend where there is none yet (Linux, Windows, the Node.js test build): sends fail at once with "no
// network backend on this platform". A libcurl or WinHTTP backend replaces this file in CMakeLists.txt.

#include "net/Backend.h"

namespace quench::net::detail {

std::unique_ptr<Backend> createPlatformBackend() { return createNoneBackend(); }
std::string platformPageOrigin() { return {}; }

} // namespace quench::net::detail
