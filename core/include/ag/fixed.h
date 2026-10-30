#pragma once
// Fixed-point and integer helpers (R1). Pure integer arithmetic; C++20 defines signed right
// shift as arithmetic and signed overflow-free conversions as two's complement.
// Copied from NeonTide/core/include/nt/fixed.h (commit 3dc7266).
#include "ag/types.h"

namespace ag {

/// x / 2^n rounded to nearest, ties away from zero.
constexpr i64 rshift_round(i64 x, u32 n) {
    if (n == 0u) return x;
    const i64 half = i64{1} << (n - 1u);
    return x >= 0 ? (x + half) >> n : -((-x + half) >> n);
}

/// floor(sqrt(x)) for x >= 0, bitwise with a fixed iteration count (no float, no libm).
constexpr u64 isqrt64(u64 x) {
    u64 res = 0;
    u64 bit = u64{1} << 62u;
    for (int i = 0; i < 32; ++i) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1u) + bit;
        } else {
            res >>= 1u;
        }
        bit >>= 2u;
    }
    return res;
}

constexpr i32 clamp_i32(i32 v, i32 lo, i32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr i32 abs_i32(i32 v) { return v < 0 ? -v : v; }
constexpr i32 max_i32(i32 a, i32 b) { return a > b ? a : b; }
constexpr i32 min_i32(i32 a, i32 b) { return a < b ? a : b; }

} // namespace ag
