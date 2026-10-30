#pragma once
// Facts about how the core was compiled, for provenance (R1, R7).
// Copied from NeonTide/core/include/nt/build_info.h (commit 3dc7266).
#include "ag/types.h"

namespace ag {

struct CoreBuildInfo {
    bool fmaContracted = false;   // runtime canary: true if a*b+c was fused (-ffp-contract=off ignored)
    bool fastMath = false;        // __FAST_MATH__ defined
    bool optimized = false;       // __OPTIMIZE__ defined
    bool ndebug = false;          // NDEBUG defined
    const char* compiler = "";
};

/// Compiled in core/src/fma_canary.cpp with the core's own flags. The only file in core/ that
/// uses floating point, and only to test the compiler (tools/check_core_numeric.py).
CoreBuildInfo core_build_info();

} // namespace ag
