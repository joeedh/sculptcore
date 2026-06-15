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
    ENV TINT_BIN
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

# CUDA / HIP backends share one validator: clang in offload mode. Rather
# than require a full CUDA/ROCm install (and a GPU to run on), the gate is
# purely syntactic — clang compiles the emitted .cu/.hip device-only and
# emits PTX/GCN assembly (`-S`). `-nogpuinc -nogpulib` keeps it offline: the
# emitted source is self-contained (see emit_cuda.cc's prelude), so no CUDA
# headers or device libs are needed, and nothing is ever linked. This catches
# every syntax/type error in the lowering without a device present.
if (SBRUSH_BACKEND_CUDA OR SBRUSH_BACKEND_HIP)
  find_program(SBRUSH_OFFLOAD_COMPILER
    NAMES clang
    DOC "clang with CUDA/HIP offload support (device-only syntactic gate)")
  if (NOT SBRUSH_OFFLOAD_COMPILER)
    message(FATAL_ERROR
      "SBRUSH_BACKEND_CUDA/HIP=ON but 'clang' was not found on PATH. "
      "Install clang (>= 14) or unset the option.")
  endif()
  message(STATUS "sbrush: CUDA/HIP offload compiler = ${SBRUSH_OFFLOAD_COMPILER}")
endif()

# OpenCL backend: sbrushc emits OpenCL C, clspv lowers it to SPIR-V, and the
# existing spirv-val gate validates the result. clspv ships in the published
# clspv-base image the devcontainer COPYs from (pinned by ci/versions.env
# CLSPV_COMMIT). spirv-val is shared with the SPIRV backend's probe above.
if (SBRUSH_BACKEND_OPENCL)
  find_program(SBRUSH_OPENCL_VALIDATOR
    NAMES clspv
    DOC "Google clspv OpenCL C -> SPIR-V compiler — https://github.com/google/clspv")
  if (NOT SBRUSH_OPENCL_VALIDATOR)
    message(FATAL_ERROR
      "SBRUSH_BACKEND_OPENCL=ON but 'clspv' was not found on PATH. "
      "Install clspv (see ci/versions.env CLSPV_COMMIT) or unset the option.")
  endif()
  message(STATUS "sbrush: clspv = ${SBRUSH_OPENCL_VALIDATOR}")
  if (NOT SBRUSH_SPIRV_VALIDATOR)
    find_program(SBRUSH_SPIRV_VALIDATOR NAMES spirv-val DOC "Khronos SPIR-V validator")
  endif()
endif()
