#pragma once

// What the Apple backends of quench_net share (AppleBackend.cpp defines it).

#include <quench/net/Http.h>

namespace quench::net::detail {

// An NSURLErrorDomain code (NSURLError.h) as the engine's kind.
ErrorKind kindOfUrlError(long code);

} // namespace quench::net::detail
