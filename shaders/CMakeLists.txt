# shaders/: every GPU kernel, written once in Slang (R2).
set(AG_KERNEL_DIR "${CMAKE_BINARY_DIR}/kernels" CACHE INTERNAL "Compiled kernel output directory")
ag_compile_kernels(ag_kernels
  OUTPUT_DIR "${AG_KERNEL_DIR}"
  SOURCES
    ag_probe.slang
    ag_load.slang)
ag_write_kernel_build_info("${AG_GEN_DIR}/ag/KernelBuildInfo.gen.h")
