// Apple backend of the app's thermal state (App.h, app::ThermalState): NSProcessInfo's thermalState, which macOS and
// iOS raise from nominal through fair and serious to critical as the device heats and the system lowers its clocks.

#if !__has_feature(objc_arc)
#error "compile with ARC (-fobjc-arc, set by quench_configure_target)"
#endif

#import <Foundation/Foundation.h>

// 0 nominal, 1 fair, 2 serious, 3 critical; -1 for a state this code does not know.
int quench_app_thermal_state(void) {
    switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal: return 0;
    case NSProcessInfoThermalStateFair: return 1;
    case NSProcessInfoThermalStateSerious: return 2;
    case NSProcessInfoThermalStateCritical: return 3;
    }
    return -1;
}
