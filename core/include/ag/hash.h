#pragma once
// State-hash primitives (docs/03 §3): NeonTide's D7 scheme, copied from
// NeonTide/core/include/nt/hash.h (commit 3dc7266). fmix32 is MurmurHash3's 32-bit finaliser; a
// word is mixed with a mixed (cell, field, seed) key. Lanes are combined with wrapping add, so
// the order in which entities are visited never changes the result.
#include "ag/types.h"

namespace ag {

inline constexpr u32 kHashSeedLo = 0x00000000u;
inline constexpr u32 kHashSeedHi = 0x5BD1E995u;

constexpr u32 fmix32(u32 h) {
    h ^= h >> 16u;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13u;
    h *= 0xC2B2AE35u;
    h ^= h >> 16u;
    return h;
}

constexpr u32 word_hash(u32 word, u32 cell, u32 field, u32 seed) {
    return fmix32(word ^ fmix32((cell * 0x9E3779B1u) ^ field ^ seed));
}

/// Two-lane accumulator. 'cell' is any stable index (grid cell, or player slot on the CPU).
struct Hash64 {
    u32 lo = 0;
    u32 hi = 0;
    constexpr void add(u32 word, u32 cell, u32 field) {
        lo += word_hash(word, cell, field, kHashSeedLo);
        hi += word_hash(word, cell, field, kHashSeedHi);
    }
    constexpr u64 value() const { return (u64{hi} << 32u) | lo; }
};

} // namespace ag
