# Warnings-as-errors and the numeric rules (R1) for project code. Third-party code is SYSTEM.
# Copied from NeonTide/cmake/NtWarnings.cmake (commit 3dc7266), renamed nt_ -> ag_.
#
# Every project target gets -ffp-contract=off and -fno-fast-math, not only core/: a CPU float op
# that leaks onto the state path must still be IEEE-exact and unfused.
set(AG_NUMERIC_FLAGS -ffp-contract=off -fno-fast-math)

function(ag_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /fp:strict /EHsc /Zc:__cplusplus)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wnon-virtual-dtor -Wcast-align -Wunused -Wformat=2 -Wdouble-promotion
      -Werror
      ${AG_NUMERIC_FLAGS})
  endif()
endfunction()
