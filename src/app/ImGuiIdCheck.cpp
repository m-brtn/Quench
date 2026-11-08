#include "ImGuiIdCheck.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <format>
#include <unordered_map>
#include <unordered_set>

namespace {

struct Conflict {
    ImGuiID id = 0;
    std::string window;
    int takes = 0; // without a label so far
};

struct IdCheck {
    int frame = -1;
    std::unordered_set<ImGuiID> seen;                // the items of this frame
    std::unordered_set<ImGuiID> found;               // every conflict of the run, reported once
    std::unordered_map<ImGuiID, std::string> labels; // of the items that were not clipped
    std::vector<Conflict> fresh;                     // found, not reported yet
};

IdCheck g_check;

// A conflict waits this many frames for its label: a clipped item (scrolled out of its window) has none.
constexpr int kLabelTakes = 120;

} // namespace

// ImGui's test engine hooks. ItemAdd comes before the clipping test, so items scrolled out of a window count too;
// windows register themselves without item data (once per Begin, which a window may call more than once a frame).
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect&, const ImGuiLastItemData* item) {
    if (item == nullptr || (item->ItemFlags & ImGuiItemFlags_AllowDuplicateId) != 0) return;
    if (ctx->FrameCount != g_check.frame) {
        g_check.frame = ctx->FrameCount;
        g_check.seen.clear();
    }
    if (g_check.seen.insert(id).second || !g_check.found.insert(id).second) return;
    g_check.fresh.push_back({.id = id, .window = ctx->CurrentWindow != nullptr ? ctx->CurrentWindow->Name : ""});
}

// Called by the widgets after ItemAdd unless they are clipped.
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    if (label != nullptr && !g_check.labels.contains(id)) g_check.labels.emplace(id, label);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }

namespace quench::app {

void enableImGuiIdCheck(ImGuiContext* context, bool on) { context->TestEngineHookItems = on; }

int imguiIdConflicts() { return int(g_check.found.size()); }

std::vector<std::string> takeImGuiIdConflicts() {
    std::vector<std::string> out;
    std::erase_if(g_check.fresh, [&](Conflict& c) {
        if (const auto it = g_check.labels.find(c.id); it != g_check.labels.end()) {
            out.push_back(std::format("\"{}\" in window \"{}\"", it->second, c.window));
            return true;
        }
        if (++c.takes < kLabelTakes) return false;
        out.push_back(std::format("ID {:#010x} in window \"{}\"", c.id, c.window));
        return true;
    });
    return out;
}

} // namespace quench::app
