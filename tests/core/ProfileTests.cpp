#include "Test.h"

#include <quench/core/Profile.h>

#include <string>

using namespace quench;

// The macros build and run with QUENCH_TRACY on and off; off, they do not evaluate their arguments.
QUENCH_TEST(profile_macros) {
    int evaluated = 0;
    {
        QUENCH_PROFILE_ZONE("test.zone");
        QUENCH_PROFILE_ZONE_TEXT("text");
    }
    {
        const std::string name = "test.dynamic";
        QUENCH_PROFILE_ZONE_DYNAMIC(name);
        QUENCH_PROFILE_ZONE_TEXT(name);
    }
    QUENCH_PROFILE_PLOT("test.int", 1);
    QUENCH_PROFILE_PLOT("test.float", 0.5f);
    QUENCH_PROFILE_PLOT("test.count", ++evaluated);
    QUENCH_PROFILE_MESSAGE("test message");
    QUENCH_PROFILE_THREAD_NAME("test");
    QUENCH_PROFILE_FRAME();
    CHECK(evaluated == (profile::kEnabled ? 1 : 0), "the plot's value evaluated %d times", evaluated);
}

// Tests run without a Tracy server: nothing is connected and nothing is recorded.
QUENCH_TEST(profile_without_server) {
    CHECK(!profile::connected(), "connected without a server");
    const profile::Zone zone = profile::beginZone("test.zone");
    CHECK(zone.active == 0, "a zone recorded without a server");
    profile::endZone(zone);
    profile::message("test message");
    const profile::GpuZone zones[] = {{"outer", 0, 1000, 0}, {"inner", 100, 900, 1}};
    profile::gpuZones(0, zones);
    profile::gpuZones(-1, {});
}
