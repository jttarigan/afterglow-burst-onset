#pragma once
// The lab clock (docs/03 §6, §9): one monotonic clock per platform for frame starts, probe times
// and GPU/present times. Apple: mach_absolute_time in ns, the same base as Metal's GPUStartTime
// and presentedTime (host time in seconds). Android and Linux: CLOCK_MONOTONIC, the base of
// Choreographer frame times; the CPU probe's own duration uses CLOCK_MONOTONIC_RAW (cpu_now_ns).
#include "ag/types.h"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

namespace ag::gfx {

#if defined(__APPLE__)
inline const mach_timebase_info_data_t& timebase() {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    return tb;
}
inline i64 now_ns() {
    const auto& tb = timebase();
    return static_cast<i64>(static_cast<__uint128_t>(mach_absolute_time()) * tb.numer / tb.denom);
}
/// CPU probe timing (docs/03 §6: mach_absolute_time on Apple).
inline i64 cpu_now_ns() { return now_ns(); }
/// Host time in seconds (CFTimeInterval from Metal) to the lab clock.
inline i64 host_seconds_to_ns(double s) { return static_cast<i64>(s * 1e9); }
#else
inline i64 now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return i64{ts.tv_sec} * 1000000000 + ts.tv_nsec;
}
/// CPU probe timing (docs/03 §6: CLOCK_MONOTONIC_RAW on Android).
inline i64 cpu_now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return i64{ts.tv_sec} * 1000000000 + ts.tv_nsec;
}
#endif

} // namespace ag::gfx
