#pragma once
// Device and device-state facts for run.json (R7). Platform-specific implementation; copied from
// NeonTide/lab/src/sysinfo.h (commit 3dc7266), whose Apple part follows Lightbound/src/bench/SysInfo.cpp.
#include "ag/json.h"

#include <string>

namespace ag::lab {

/// Static facts: model, OS and build, CPU, RAM.
Json device_facts();
/// Momentary state: thermal state, battery level, charging, power source, Low Power Mode.
Json device_state();
/// "nominal", "fair", "serious", "critical", or "unknown".
std::string thermal_state();
/// Short machine label, e.g. "Mac17,3" or "iPhone13,2".
std::string device_model();
/// SoC name, e.g. "Apple M5", "A14" (from a table of the lab's devices), or "unknown: <model>".
std::string device_soc();

/// JSON set by the mobile host through the C ABI (M1); empty on desktop.
std::string platform_state_json();
void set_platform_state_json(std::string json);

} // namespace ag::lab
