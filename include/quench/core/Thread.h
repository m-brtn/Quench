#pragma once

namespace quench {

// Gives the calling thread the scheduling class of work the frame thread waits for: user-interactive QoS on Apple
// platforms, where the default class of a new thread is lower and the wait would be a priority inversion. Call it at
// the start of every worker thread. Does nothing on other platforms yet.
void raiseWorkerThreadPriority();

// True when the calling thread has that class; always true where raiseWorkerThreadPriority() does nothing.
bool hasWorkerThreadPriority();

} // namespace quench
