#include "Test.h"

#include <quench/core/Result.h>

#include <memory>
#include <string>

using namespace quench;

namespace {

Result<int> half(int v) {
    if (v % 2 != 0) return Error{"odd value " + std::to_string(v)};
    return v / 2;
}

Result<void> check(bool ok) {
    if (!ok) return Error{"not ok"};
    return {};
}

} // namespace

QUENCH_TEST(result_value) {
    const Result<int> r = half(8);
    CHECK(r.ok() && bool(r), "ok() is false");
    CHECK(r.value() == 4 && *r == 4, "value %d", r.value());
    CHECK(r.error().empty(), "error on success: %s", r.error().c_str());
}

QUENCH_TEST(result_error) {
    const Result<int> r = half(7);
    CHECK(!r.ok() && !r, "ok() on error");
    CHECK(r.error() == "odd value 7", "error '%s'", r.error().c_str());
    CHECK(r.valueOr(-1) == -1, "valueOr");
}

QUENCH_TEST(result_move_only) {
    Result<std::unique_ptr<int>> r = std::make_unique<int>(5);
    CHECK(r.ok() && **r == 5, "unique_ptr value");
    const std::unique_ptr<int> taken = std::move(r).value();
    CHECK(taken && *taken == 5, "moved out");
}

QUENCH_TEST(result_void) {
    CHECK(check(true).ok(), "Result<void> success");
    const Result<void> r = check(false);
    CHECK(!r && r.error() == "not ok", "Result<void> error '%s'", r.error().c_str());
}
