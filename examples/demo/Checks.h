#pragma once

// The check mode's list: what still has to be looked at or listened to on a device (the devlog's checks waiting for
// the user), per section, each with the settings it starts from. Results are kept in the user directory as JSON
// (checks.json; --checks-file NAME), written after every mark.

#include <quench/core/Result.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace demo {

struct CheckItem {
    std::string_view id;      // "<section>.<what>"
    std::string_view section; // Section::id
    std::string_view title;
    std::string_view what;  // what to do and what is right
    std::string_view setup; // scenario commands run when the item is shown, one per line ("set tone 1", "hdr on")
};
std::span<const CheckItem> checkItems();

enum class CheckStatus { Pending, Ok, Problem, Skipped };
const char* checkStatusName(CheckStatus status);

struct CheckResult {
    std::string id;
    CheckStatus status = CheckStatus::Pending;
    std::string note;
    std::string backend; // the GPU backend and driver it was marked on
    std::string time;    // UTC, ISO 8601
};

// The run the results came from, written at the top of the file.
struct CheckRun {
    std::string app, version, build, backend, driver, device, platform;
};

// One result per item of checkItems(), in its order; results of items no longer listed are dropped.
quench::Result<std::vector<CheckResult>> parseCheckResults(const std::string& text);
std::string writeCheckResults(const CheckRun& run, std::span<const CheckResult> results);
std::vector<CheckResult> emptyCheckResults();
std::string utcNow(); // "2026-11-06T09:41:00Z"

} // namespace demo
