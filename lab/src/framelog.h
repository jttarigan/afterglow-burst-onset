#pragma once
// frames.csv and device.csv (docs/03 §9): one schema on every platform, so the analysis scripts
// never branch per platform. All values are integers; a missing value is written as an empty field.
#include "runio.h"

#include <array>
#include <limits>
#include <string>

namespace ag::lab {

inline constexpr i64 kNA = std::numeric_limits<i64>::min();   // written as an empty field

// Column order is the schema. tools/analysis/agcsv.py holds the same list; a test compares them.
inline constexpr std::array<const char*, 30> kFrameColumns = {
    // indices
    "frame", "tick", "ticks_run",
    // CPU timing
    "t_frame_start_ns", "cpu_probe_ns", "cpu_sim_ns", "cpu_forecast_ns", "cpu_encode_ns",
    // GPU timing
    "gpu_probe_ns", "gpu_main_ns", "gpu_heater_ns",
    // presentation
    "present_target_ns", "present_actual_ns", "late",
    // workload
    "live_blasts", "detonations", "live_sparks", "smoke_on",
    // policy
    "policy_state", "heater_gpu_us", "heater_cpu_us", "hint_calls",
    // CPU placement (Android): the core at the CPU probe, and its frequency where readable (D45)
    "cpu_id", "cpu_freq_khz",
    // stepbench trial markers (D25): empty outside stepbench mode
    "sb_trial", "sb_phase", "sb_calm_ms", "sb_load_pct", "sb_target",
    // timestamp of the GPU probe start on the CPU clock where calibrated (idle-gap ladder, S6)
    "gpu_probe_start_ns",
};
inline constexpr const char* kFrameSchema = "afterglow.frames/2";   // /2 adds cpu_freq_khz (D45)

struct FrameRow {
    std::array<i64, kFrameColumns.size()> v;
    FrameRow() { v.fill(kNA); }
    i64& at(usize column) { return v[column]; }
};

/// Column index by name, or -1 (used once at start-up, never per frame).
int frame_column(const char* name);

inline constexpr std::array<const char*, 8> kDeviceColumns = {
    "t_ns", "thermal_state", "battery_pct", "charging", "current_ua", "voltage_mv", "brightness_pct", "low_power_mode",
};
inline constexpr const char* kDeviceSchema = "afterglow.device/1";

/// Formats one row; the string is reused, so writing a row does not allocate once warm.
void format_row(const i64* values, usize n, std::string& out);

class FrameLogWriter {
public:
    bool open(const std::string& path, const Json& meta);
    void write(const FrameRow& r);
    bool close() { return csv_.close(); }

private:
    CsvWriter csv_;
    std::string line_;
};

/// device.csv: sampled at 1 Hz. thermal_state is a string column (platform names, docs/04 §2).
struct DeviceRow {
    i64 tNs = kNA;
    std::string thermalState;
    i64 batteryPct = kNA, charging = kNA, currentUa = kNA, voltageMv = kNA, brightnessPct = kNA, lowPowerMode = kNA;
};

class DeviceLogWriter {
public:
    bool open(const std::string& path, const Json& meta);
    void write(const DeviceRow& r);
    bool close() { return csv_.close(); }

private:
    CsvWriter csv_;
    std::string line_;
};

} // namespace ag::lab
