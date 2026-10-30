# Third-party code, fetched with FetchContent and pinned by version and SHA-256.
# Pins copied from NeonTide/cmake/NtDependencies.cmake (commit 3dc7266): doctest 2.5.3,
# Slang 2026.18.2 (binary release), metal-cpp_26.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

# ---------------------------------------------------------------- doctest
FetchContent_Declare(doctest
  URL      "https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz"
  URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR cmake_not_used)
set(_ag_fetch_list doctest)

# ---------------------------------------------------------------- Slang (host tool, pinned)
set(AG_SLANG_VERSION "2026.18.2")
if(CMAKE_HOST_APPLE AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(_ag_slang_asset "slang-${AG_SLANG_VERSION}-macos-aarch64.zip")
  set(_ag_slang_sha "cffb3a7eef12cabfc1e169271db4b7c9b982ed59aef6c3f138d02e392178bfbe")
else()
  message(FATAL_ERROR "Pin the Slang ${AG_SLANG_VERSION} asset hash for this host first.")
endif()
FetchContent_Declare(slang_release
  URL      "https://github.com/shader-slang/slang/releases/download/v${AG_SLANG_VERSION}/${_ag_slang_asset}"
  URL_HASH SHA256=${_ag_slang_sha}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR cmake_not_used)
list(APPEND _ag_fetch_list slang_release)

# ---------------------------------------------------------------- metal-cpp (Apple targets)
if(APPLE AND AG_GFX_BACKEND STREQUAL "metal")
  FetchContent_Declare(metalcpp
    URL      "https://developer.apple.com/metal/cpp/files/metal-cpp_26.zip"
    URL_HASH SHA256=4df3c078b9aadcb516212e9cb03004cbc5ce9a3e9c068fa3144d021db585a3a4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  list(APPEND _ag_fetch_list metalcpp)
endif()

# ---------------------------------------------------------------- Vulkan headers + MoltenVK (macOS dev only)
# Android uses the NDK's Vulkan headers. MoltenVK runs the Vulkan path on a Mac for development;
# its runs are never research data (it translates SPIR-V to Metal). Pins from NeonTide.
if(AG_GFX_BACKEND STREQUAL "vulkan" AND NOT ANDROID)
  FetchContent_Declare(vulkan_headers
    URL      "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz"
    URL_HASH SHA256=e87dce08116151f6b6d7de6b6faf41498e87e6cf848ff16fa3bd5402190ad4a3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR cmake_not_used)
  list(APPEND _ag_fetch_list vulkan_headers)
  if(APPLE)
    FetchContent_Declare(moltenvk
      URL      "https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar"
      URL_HASH SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SOURCE_SUBDIR cmake_not_used)
    list(APPEND _ag_fetch_list moltenvk)
  endif()
endif()

FetchContent_MakeAvailable(${_ag_fetch_list})

if(AG_GFX_BACKEND STREQUAL "vulkan")
  add_library(ag_vulkan_headers INTERFACE)
  if(NOT ANDROID)
    target_include_directories(ag_vulkan_headers SYSTEM INTERFACE "${vulkan_headers_SOURCE_DIR}/include")
  endif()
  if(APPLE)
    file(GLOB_RECURSE _ag_mvk "${moltenvk_SOURCE_DIR}/*/dynamic/dylib/macOS/libMoltenVK.dylib")
    list(GET _ag_mvk 0 _ag_mvk_dylib)
    target_compile_definitions(ag_vulkan_headers INTERFACE AG_VULKAN_LIBRARY_PATH="${_ag_mvk_dylib}")
    message(STATUS "Afterglow: Vulkan via MoltenVK (development only): ${_ag_mvk_dylib}")
  endif()
endif()

add_library(ag_doctest INTERFACE)
target_include_directories(ag_doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")

set(AG_SLANGC "${slang_release_SOURCE_DIR}/bin/slangc${CMAKE_EXECUTABLE_SUFFIX}" CACHE FILEPATH "Path to slangc")
if(NOT EXISTS "${AG_SLANGC}")
  message(FATAL_ERROR "slangc not found at '${AG_SLANGC}'")
endif()

if(APPLE AND AG_GFX_BACKEND STREQUAL "metal")
  if(EXISTS "${metalcpp_SOURCE_DIR}/metal-cpp/Metal/Metal.hpp")
    set(_ag_metalcpp_dir "${metalcpp_SOURCE_DIR}/metal-cpp")
  else()
    set(_ag_metalcpp_dir "${metalcpp_SOURCE_DIR}")
  endif()
  add_library(ag_metalcpp INTERFACE)
  target_include_directories(ag_metalcpp SYSTEM INTERFACE "${_ag_metalcpp_dir}")
  target_link_libraries(ag_metalcpp INTERFACE "-framework Metal" "-framework QuartzCore" "-framework Foundation")
endif()
