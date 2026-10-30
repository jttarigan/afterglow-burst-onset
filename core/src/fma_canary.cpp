#include "ag/build_info.h"

#define AG_STR2(x) #x
#define AG_STR(x) AG_STR2(x)

namespace ag {

// Copied from NeonTide/core/src/fma_canary.cpp (commit 3dc7266); the canary itself is from
// NeonSwarm (NeonSwarm/core/src/api.cpp, commit 5663afd). With separate rounding,
// a*b rounds to 1+2^-22 and the sum is exactly 0; a fused multiply-add keeps the 2^-46 term and
// returns non-zero. The volatile loads stop constant folding; the expression itself may still be
// contracted, which is what we test.
CoreBuildInfo core_build_info() {
    CoreBuildInfo info;
    volatile float a = 1.0f + 1.1920929e-7f;     // 1 + 2^-23
    volatile float b = a;
    volatile float c = -(1.0f + 2.3841858e-7f);  // -(1 + 2^-22)
    const float r = a * b + c;
    info.fmaContracted = r != 0.0f;
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
    info.fastMath = true;
#endif
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && !defined(_DEBUG))
    info.optimized = true;
#endif
#if defined(NDEBUG)
    info.ndebug = true;
#endif
#if defined(__clang_version__)
    info.compiler = "clang " __clang_version__;
#elif defined(_MSC_FULL_VER)
    info.compiler = "MSVC " AG_STR(_MSC_FULL_VER);
#elif defined(__VERSION__)
    info.compiler = __VERSION__;
#else
    info.compiler = "unknown";
#endif
    return info;
}

} // namespace ag
