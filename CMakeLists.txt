cmake_minimum_required(VERSION 3.25)
project(afterglow VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global settings (layout follows NeonTide/CMakeLists.txt, see docs/DECISIONS.md D19)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0" CACHE STRING "" FORCE)
  else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "" FORCE)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
  # Mobile: no CLI and no tests (the platform shells link the libraries).
  set(AG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(AG_BUILD_CLI OFF)
else()
  set(AG_BUILD_CLI ON)
endif()
option(AG_BUILD_TESTS "Build unit tests" ON)
if(APPLE)
  set(_ag_default_backend metal)
else()
  set(_ag_default_backend vulkan)
endif()
set(AG_GFX_BACKEND "${_ag_default_backend}" CACHE STRING "GPU backend: metal | vulkan")
set_property(CACHE AG_GFX_BACKEND PROPERTY STRINGS metal vulkan)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(AgWarnings)
include(AgDependencies)
include(AgSlang)

# Git commit + dirty flag, regenerated on every build (R7: a dirty tree invalidates runs).
set(AG_GEN_DIR "${CMAKE_BINARY_DIR}/gen")
add_custom_target(ag_gitinfo
  COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${AG_GEN_DIR}/ag/GitInfo.h
          -P "${CMAKE_SOURCE_DIR}/cmake/AgGitInfo.cmake"
  BYPRODUCTS "${AG_GEN_DIR}/ag/GitInfo.h"
  COMMENT "Recording git commit and dirty flag")

message(STATUS "Afterglow: build=${CMAKE_BUILD_TYPE} backend=${AG_GFX_BACKEND}")

add_subdirectory(shaders)
add_subdirectory(core)
add_subdirectory(gfx)
add_subdirectory(lab)
if(ANDROID)
  add_subdirectory(platform/android/jni)
endif()
if(AG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
