# sbrush backend toolchain probes.
#
# Included from the root CMakeLists.txt whenever any non-CPP sbrush
# backend is enabled. Each enabled backend's external validator is
# located via find_program; missing tools are a hard configure error
# rather than a silent downgrade — CI can't tell a half-configured
# runner from a passing build otherwise. The exported cache variables
# are read by build_files/macros.cmake's sbrush_backend rules.

if (SBRUSH_BACKEND_WGSL OR SBRUSH_BACKEND_SPIRV)
  # Tint covers both backends in this slice: WGSL gets a syntax check, and
  # the SPIR-V backend uses `tint --format=spirv` as the lowering step
  # until a direct SPIR-V emitter lands (see brush_compute_dsl.md Wave 5).
  find_program(SBRUSH_WGSL_VALIDATOR
    NAMES tint
    DOC "Dawn standalone WGSL compiler (validator) — https://dawn.googlesource.com/dawn")
  if (NOT SBRUSH_WGSL_VALIDATOR)
    message(FATAL_ERROR
      "SBRUSH_BACKEND_WGSL/SPIRV=ON but 'tint' was not found on PATH. "
      "Install Dawn's tint (see ci/versions.env TINT_COMMIT) or unset the option.")
  endif()
  message(STATUS "sbrush: tint = ${SBRUSH_WGSL_VALIDATOR}")
endif()

if (SBRUSH_BACKEND_SPIRV)
  find_program(SBRUSH_SPIRV_VALIDATOR
    NAMES spirv-val
    DOC "Khronos SPIR-V validator from SPIRV-Tools")
  if (NOT SBRUSH_SPIRV_VALIDATOR)
    message(FATAL_ERROR
      "SBRUSH_BACKEND_SPIRV=ON but 'spirv-val' was not found on PATH. "
      "Install SPIRV-Tools or unset SBRUSH_BACKEND_SPIRV.")
  endif()
  message(STATUS "sbrush: SPIR-V validator = ${SBRUSH_SPIRV_VALIDATOR}")
endif()

# Wave 5 hooks live here once their emitters land:
#   if (SBRUSH_BACKEND_CUDA)  find_program(SBRUSH_CUDA_VALIDATOR  nvcc      ...)
#   if (SBRUSH_BACKEND_HIP)   find_program(SBRUSH_HIP_VALIDATOR   hipcc     ...)
#   if (SBRUSH_BACKEND_OPENCL) find_program(SBRUSH_OPENCL_VALIDATOR clspv  ...)
