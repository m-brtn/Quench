#pragma once

#include <quench/assistant/Live.h>

// The talk mode of the live check (Talk.cpp): the real microphone and output with a configured session (its key,
// instructions and tools set). Returns the exit code.
int runTalk(quench::assistant::LiveSession& session, bool gate, double seconds);
