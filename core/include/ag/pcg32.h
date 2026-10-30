#pragma once
// PCG32 (O'Neill, pcg-random.org: pcg32 = XSH RR 64/32, the reference "pcg32_random_r").
// Each consumer gets its own stream (seed, stream id), so adding a consumer never shifts
// another's sequence. Copied from NeonTide/core/include/nt/pcg32.h (commit 3dc7266).
#include "ag/types.h"

namespace ag {

class Pcg32 {
public:
    Pcg32() { seed(0x853C49E6748FEA9Bull, 0xDA3E39CB94B95BDBull); }
    Pcg32(u64 initState, u64 streamId) { seed(initState, streamId); }

    void seed(u64 initState, u64 streamId) {
        state_ = 0u;
        inc_ = (streamId << 1u) | 1u;
        next();
        state_ += initState;
        next();
    }

    u32 next() {
        const u64 old = state_;
        state_ = old * 6364136223846793005ull + inc_;
        const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot = static_cast<u32>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    /// Uniform in [0, bound) without modulo bias (the reference pcg32_boundedrand_r).
    u32 below(u32 bound) {
        if (bound <= 1u) return 0u;
        const u32 threshold = (0u - bound) % bound;
        for (;;) {
            const u32 r = next();
            if (r >= threshold) return r % bound;
        }
    }

    u64 state() const { return state_; }
    u64 inc() const { return inc_; }

private:
    u64 state_ = 0;
    u64 inc_ = 1;
};

} // namespace ag
