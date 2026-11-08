#pragma once

#include <string>
#include <vector>

struct ImGuiContext;

namespace quench::app {

// Two items with the same ID in one frame (the same label in one window, without "##" or PushID): ImGui reports them
// only while the mouse is over one of them ("conflicting ID"). This check sees every item of every frame, through
// ImGui's test engine hooks (quench_imgui is built with IMGUI_ENABLE_TEST_ENGINE; the hooks are in ImGuiIdCheck.cpp).
void enableImGuiIdCheck(ImGuiContext* context, bool on);

// The IDs found in conflict so far.
int imguiIdConflicts();

// The conflicts found since the last call, each once over the run: "label" in window "name" (or the ID, when the
// items stayed clipped).
std::vector<std::string> takeImGuiIdConflicts();

} // namespace quench::app
