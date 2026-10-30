# Kernel compilation (docs/03, R2). Adapted from NeonTide/cmake/NtSlang.cmake (commit 3dc7266).
#
# One .slang source per kernel, compiled once per target in a single math variant (D29):
#   SPIR-V 1.3 (Vulkan 1.1, Tab S7+), with an assembly listing next to it;
#   Metal: Slang emits MSL, which we compile ourselves with xcrun so the math flags are explicit
#   and logged (KernelBuildInfo.gen.h -> run.json).
# Research kernels (probe, load, heater) are integer-only, so the math mode cannot change their
# work; cosmetic float kernels (M3) may differ in value per device, never in amount (R1).
#
#   ag_compile_kernels(<target> OUTPUT_DIR <dir> SOURCES a.slang ... SHARED ...)
# Outputs per kernel <k>: <dir>/spirv/<k>.spv, <k>.spvasm; <dir>/metal/<k>.metal, .air, .metallib;
# <dir>/refl/<k>.{spirv,metal}.json; and ${AG_GEN_DIR}/ag/KernelBindings.gen.h.

set(AG_SLANG_SPIRV_PROFILE "spirv_1_3")
set(AG_SLANG_FLAGS -fp-mode fast -fvk-use-entrypoint-name -warnings-as-errors all -O2)
set(AG_METAL_STD "metal3.0" CACHE STRING "MSL language version passed to xcrun metal")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
  set(AG_METAL_SDK iphonesimulator)   # development builds for the iOS simulator (preset ios-sim)
  set(AG_METAL_MIN "-mios-simulator-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(AG_METAL_SDK iphoneos)
  set(AG_METAL_MIN "-mios-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
  set(AG_METAL_SDK macosx)
  set(AG_METAL_MIN "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
set(AG_METAL_FLAGS -fmetal-math-mode=fast -fmetal-math-fp32-functions=fast -Wno-unused-variable)

function(ag_compile_kernels target)
  cmake_parse_arguments(ARG "" "OUTPUT_DIR" "SOURCES;SHARED" ${ARGN})
  set(_outputs)
  set(_refl_spirv)
  set(_refl_metal)
  foreach(src IN LISTS ARG_SOURCES)
    get_filename_component(name "${src}" NAME_WE)
    get_filename_component(abs "${src}" ABSOLUTE)
    set(spv "${ARG_OUTPUT_DIR}/spirv/${name}.spv")
    set(spvasm "${ARG_OUTPUT_DIR}/spirv/${name}.spvasm")
    set(rspv "${ARG_OUTPUT_DIR}/refl/${name}.spirv.json")
    add_custom_command(
      OUTPUT "${spv}" "${spvasm}" "${rspv}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${ARG_OUTPUT_DIR}/spirv" "${ARG_OUTPUT_DIR}/refl"
      COMMAND "${AG_SLANGC}" "${abs}" -target spirv -profile ${AG_SLANG_SPIRV_PROFILE} ${AG_SLANG_FLAGS}
              -reflection-json "${rspv}" -o "${spv}"
      COMMAND "${AG_SLANGC}" "${abs}" -target spirv-asm -profile ${AG_SLANG_SPIRV_PROFILE} ${AG_SLANG_FLAGS} -o "${spvasm}"
      DEPENDS "${abs}" ${ARG_SHARED}
      COMMENT "slangc -> SPIR-V 1.3: ${name}"
      VERBATIM)
    list(APPEND _outputs "${spv}" "${spvasm}")
    list(APPEND _refl_spirv "${rspv}")
    if(APPLE)
      set(msl "${ARG_OUTPUT_DIR}/metal/${name}.metal")
      set(air "${ARG_OUTPUT_DIR}/metal/${name}.air")
      set(lib "${ARG_OUTPUT_DIR}/metal/${name}.metallib")
      set(rmtl "${ARG_OUTPUT_DIR}/refl/${name}.metal.json")
      add_custom_command(
        OUTPUT "${msl}" "${rmtl}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ARG_OUTPUT_DIR}/metal" "${ARG_OUTPUT_DIR}/refl"
        COMMAND "${AG_SLANGC}" "${abs}" -target metal ${AG_SLANG_FLAGS} -reflection-json "${rmtl}" -o "${msl}"
        DEPENDS "${abs}" ${ARG_SHARED}
        COMMENT "slangc -> MSL: ${name}"
        VERBATIM)
      add_custom_command(
        OUTPUT "${air}"
        COMMAND xcrun -sdk ${AG_METAL_SDK} metal -std=${AG_METAL_STD} ${AG_METAL_MIN} ${AG_METAL_FLAGS} -c "${msl}" -o "${air}"
        DEPENDS "${msl}"
        COMMENT "xcrun metal: ${name}"
        VERBATIM)
      add_custom_command(
        OUTPUT "${lib}"
        COMMAND xcrun -sdk ${AG_METAL_SDK} metallib "${air}" -o "${lib}"
        DEPENDS "${air}"
        COMMENT "xcrun metallib: ${name}"
        VERBATIM)
      list(APPEND _outputs "${lib}")
      list(APPEND _refl_metal "${rmtl}")
    endif()
  endforeach()

  set(bind_h "${AG_GEN_DIR}/ag/KernelBindings.gen.h")
  add_custom_command(
    OUTPUT "${bind_h}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_bindings.py" -o "${bind_h}"
            --spirv ${_refl_spirv} --metal ${_refl_metal}
    DEPENDS ${_refl_spirv} ${_refl_metal} "${CMAKE_SOURCE_DIR}/tools/gen_bindings.py"
    COMMENT "Generating kernel binding table from Slang reflection"
    VERBATIM)
  list(APPEND _outputs "${bind_h}")
  add_custom_target(${target} ALL DEPENDS ${_outputs})
endfunction()

# Kernel build facts for provenance (R7).
function(ag_write_kernel_build_info out)
  execute_process(COMMAND "${AG_SLANGC}" -version OUTPUT_VARIABLE slang_ver
                  ERROR_VARIABLE slang_ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
  set(metal_ver "n/a: no Metal on this target")
  set(metal_cmd "n/a: no Metal on this target")
  if(APPLE)
    execute_process(COMMAND xcrun -sdk ${AG_METAL_SDK} metal --version OUTPUT_VARIABLE metal_ver
                    ERROR_VARIABLE metal_ver OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX REPLACE "\n.*" "" metal_ver "${metal_ver}")
    string(REPLACE ";" " " mflags "${AG_METAL_FLAGS}")
    set(metal_cmd "xcrun -sdk ${AG_METAL_SDK} metal -std=${AG_METAL_STD} ${AG_METAL_MIN} ${mflags}")
  endif()
  string(REPLACE ";" " " sflags "${AG_SLANG_FLAGS}")
  file(WRITE "${out}.tmp"
"#pragma once
// Generated by cmake/AgSlang.cmake at configure time.
#define AG_SLANG_VERSION_PINNED \"${AG_SLANG_VERSION}\"
#define AG_SLANG_VERSION_REPORTED \"${slang_ver}\"
#define AG_SLANG_SPIRV_PROFILE \"${AG_SLANG_SPIRV_PROFILE}\"
#define AG_SLANG_FLAGS \"${sflags}\"
#define AG_METAL_COMPILER_VERSION \"${metal_ver}\"
#define AG_METAL_FLAGS \"${metal_cmd}\"
")
  file(COPY_FILE "${out}.tmp" "${out}" ONLY_IF_DIFFERENT)
endfunction()
