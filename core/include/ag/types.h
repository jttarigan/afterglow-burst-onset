#pragma once
// Integer aliases. core/ is integer-only (R1): no float or double on the simulation path.
// tools/check_core_numeric.py enforces this for every file under core/ except fma_canary.cpp.
// Copied from NeonTide/core/include/nt/types.h (commit 3dc7266).
#include <cstddef>
#include <cstdint>

namespace ag {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using usize = std::size_t;

} // namespace ag
