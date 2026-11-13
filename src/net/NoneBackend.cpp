// The "none" backend: every send fails at once (Client::send checks available()). Built in every build, for
// ClientDesc::noBackend; on platforms without a network backend yet (Linux, Windows, the Node.js test build) it is
// also the platform's (NonePlatform.cpp).

#include "net/Backend.h"

namespace quench::net::detail {
namespace {

class NoneBackend final : public Backend {
public:
    const char* name() const override { return "none"; }
    bool available() const override { return false; }
    Result<void> start(RequestId, const Request&, bool, double) override {
        return Error{"no network backend on this platform"};
    }
    void cancel(RequestId) override {}
    void poll(double, std::vector<Event>&) override {}
};

} // namespace

std::unique_ptr<Backend> createNoneBackend() { return std::make_unique<NoneBackend>(); }

} // namespace quench::net::detail
