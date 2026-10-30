#pragma once
// CPU probe and CPU load (docs/03 §6, docs/04 E1): a dependent integer multiply-add chain, the
// same recurrence as the GPU kernels (shaders/ag_probe.slang). The empty asm keeps the chain
// serial, so the compiler can neither vectorise it nor fold it into a closed form.
#include "ag/types.h"

namespace ag::lab {

/// Fixed per build (R3): the CPU probe takes about 30 us on a warm big core.
inline constexpr u32 kCpuProbeIters = 24000;
/// Fixed per build (R3): the GPU probe, 4 groups x 64 threads (docs/03 §6). 600 iterations take
/// about 80 us on a warm Apple A14 GPU (6000 took 785 us, 2026-10-06 smoke run, D40); E0 checks it
/// stays under 2 % of the frame on every core device before the protocol freeze.
inline constexpr u32 kGpuProbeIters = 600;
inline constexpr u32 kGpuProbeGroups = 4;
/// Synthetic GPU load grid: 256 groups x 64 threads fills every core device's GPU.
inline constexpr u32 kGpuLoadGroups = 256;

inline u32 cpu_spin(u32 iterations, u32 seed) {
    u32 x = seed * 2654435761u + 12345u;
    u32 y = x ^ 0x9E3779B9u;
    for (u32 i = 0; i < iterations; ++i) {
        x = x * 1664525u + 1013904223u;
        y = y * 22695477u + x;
#if defined(__GNUC__) || defined(__clang__)
        asm volatile("" : "+r"(x), "+r"(y));
#endif
    }
    return x ^ y;
}

/// What GPU thread 'tid' of ag_probe / ag_load writes (the CPU check of the probe result).
constexpr u32 spin_reference(u32 tid, u32 seed, u32 iterations) {
    u32 x = tid * 2654435761u + seed;
    u32 y = x ^ 0x9E3779B9u;
    for (u32 i = 0; i < iterations; ++i) {
        x = x * 1664525u + 1013904223u;
        y = y * 22695477u + x;
    }
    return x ^ y;
}

/// Push constants of ag_probe and ag_load (SpinParams in the .slang sources; 16 bytes).
struct SpinParams {
    u32 iterations = 0;
    u32 seed = 0;
    u32 pad0 = 0;
    u32 pad1 = 0;
};
static_assert(sizeof(SpinParams) == 16, "must match SpinParams in shaders/ag_probe.slang");

} // namespace ag::lab
