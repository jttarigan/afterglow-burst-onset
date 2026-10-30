# Android: one shared library, libafterglow.so (JNI glue + the static libraries), placed where
# Gradle packages it (build/<preset>/jniLibs/arm64-v8a), plus the SPIR-V kernels and the research
# levels as APK assets (build/<preset>/assets). Adapted from NeonTide/platform/android/jni.
add_library(afterglow SHARED afterglow_jni.cpp)
target_link_libraries(afterglow PRIVATE ag_lab android nativewindow log)
target_link_options(afterglow PRIVATE -Wl,--no-undefined)
ag_set_warnings(afterglow)
set_target_properties(afterglow PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/jniLibs/${ANDROID_ABI}")
add_dependencies(afterglow ag_kernels)

set(_ag_assets "${CMAKE_BINARY_DIR}/assets")
file(GLOB _ag_levels "${CMAKE_SOURCE_DIR}/bench/levels/*.json")
add_custom_command(TARGET afterglow POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_ag_assets}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ag_assets}/kernels/spirv" "${_ag_assets}/levels"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${AG_KERNEL_DIR}/spirv" "${_ag_assets}/kernels/spirv"
  COMMAND "${CMAKE_COMMAND}" -E copy ${_ag_levels} "${_ag_assets}/levels"
  COMMENT "Staging SPIR-V kernels and research levels as APK assets"
  VERBATIM)
