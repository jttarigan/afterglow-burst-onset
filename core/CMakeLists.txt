# core/: integer simulation, workload descriptor and presets, level files, forecaster, hashing.
# Pure C++, no platform headers, integer-only (R1). Compiled with -ffp-contract=off and without
# fast-math (AgWarnings.cmake); tools/check_core_numeric.py keeps float out of it.
add_library(ag_core STATIC
  src/fma_canary.cpp
  src/forecast.cpp
  src/json.cpp
  src/level.cpp
  src/sha256.cpp
  src/sim.cpp
  src/workload.cpp)
target_include_directories(ag_core PUBLIC include)
ag_set_warnings(ag_core)
