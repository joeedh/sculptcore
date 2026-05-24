# sbrush backend toolchain probes.
#
# Included from the root CMakeLists.txt whenever any non-CPP sbrush
# backend is enabled. Each enabled backend's external validator is
# located via find_program; missing tools are a hard configure error
# rather than a silent downgrade — CI can't tell a half-configured
# runner from a passing build otherwise. The exported cache variables
# are read by build_files/macros.cmake's sbrush_backend rules.

if (SBRUSH_BACKEND_WGSL)
  find_program(SBRUSH_WGSL_VALIDATOR
    NAMES tint
    DOC "Dawn standalone WGSL compiler (validator) — https://dawn.googlesource.com/dawn")
  if (NOT SBRUSH_WGSL_VALIDATOR)
    message(FATAL_ERROR
      "SBRUSH_BACKEND_WGSL=ON but 'tint' was not found on PATH. "
      "Install Dawn's tint (see ci/versions.env TINT_COMMIT) or unset SBRUSH_BACKEND_WGSL.")
  endif()
  message(STATUS "sbrush: WGSL validator = ${SBRUSH_WGSL_VALIDATOR}")
endif()

# Wave 5 hooks live here once their emitters land:
#   if (SBRUSH_BACKEND_SPIRV) find_program(SBRUSH_SPIRV_VALIDATOR spirv-val ...)
#   if (SBRUSH_BACKEND_CUDA)  find_program(SBRUSH_CUDA_VALIDATOR  nvcc      ...)
#   if (SBRUSH_BACKEND_HIP)   find_program(SBRUSH_HIP_VALIDATOR   hipcc     ...)
#   if (SBRUSH_BACKEND_OPENCL) find_program(SBRUSH_OPENCL_VALIDATOR clspv  ...)
