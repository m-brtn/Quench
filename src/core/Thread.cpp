#include <quench/core/Thread.h>

#include "core/Platform.h"

#if QUENCH_PLATFORM_APPLE
#include <pthread/qos.h>
#endif

namespace quench {

void raiseWorkerThreadPriority() {
#if QUENCH_PLATFORM_APPLE
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

bool hasWorkerThreadPriority() {
#if QUENCH_PLATFORM_APPLE
    return qos_class_self() == QOS_CLASS_USER_INTERACTIVE;
#else
    return true;
#endif
}

} // namespace quench
