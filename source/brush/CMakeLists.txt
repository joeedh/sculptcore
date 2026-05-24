set(LIB
    eigen
    util
    math
    props
    spatial
    meshlog
    platform
)

set(SRC
  PUBLIC brush.h
  PUBLIC test.h
  PUBLIC exec.h
  PUBLIC props.h
  PUBLIC brush_iterators.h
  PUBLIC brush_command.h
  PUBLIC brush_concepts.h
  PUBLIC stroke_spacing.h
  PUBLIC brushes/all.h
  brush_command.cc
  brush_executor.h
  brush_executor.cc
  brush.cc
  PUBLIC bindings.h
  bindings.cc
)

sc_add_library(brush "${SRC}" "${LIB}" STATIC)

# Generated sbrush kernels (created by `node make.mjs codegen`).
# The directory may not exist yet at configure time; that's fine —
# it becomes populated before the first build that needs it.
target_include_directories(brush PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/kernels/generated
)

# sbrush compiler — host-only tool. Skipped on WASM (cross-compile);
# WASM builds consume the .gen.h files produced by a prior native
# `node make.mjs codegen` run.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_CPP)
  add_subdirectory(compiler)
endif()

# WGSL backend: emit one .wgsl per .sbrush into the build dir, and
# (when SBRUSH_VALIDATE_ALL=ON) gate the build on `tint` accepting it.
# The .wgsl artifacts are not linked into libbrush — they exist for
# host-side WebGPU dispatch + CI validation only.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_WGSL)
  target_compile_definitions(brush PUBLIC SBRUSH_BACKEND_WGSL=1)
  file(GLOB SBRUSH_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.sbrush")
  set(WGSL_OUT_DIR "${CMAKE_BINARY_DIR}/sbrush_out/wgsl")
  file(MAKE_DIRECTORY "${WGSL_OUT_DIR}")
  set(WGSL_OUTPUTS)
  foreach(SRC ${SBRUSH_INPUTS})
    get_filename_component(STEM "${SRC}" NAME_WE)
    set(OUT "${WGSL_OUT_DIR}/${STEM}.wgsl")
    # Emit pass; the optional validator pass writes a stamp file so
    # downstream targets can depend on "WGSL produced AND tint-validated"
    # rather than re-running validation every build.
    set(STAMP "${OUT}.validated")
    if (SBRUSH_VALIDATE_ALL)
      add_custom_command(
        OUTPUT "${STAMP}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=wgsl --in=${SRC} --out=${OUT}
        COMMAND ${SBRUSH_WGSL_VALIDATOR} ${OUT} --format=wgsl -o ${OUT}.tint.wgsl
        COMMAND ${CMAKE_COMMAND} -E touch ${STAMP}
        DEPENDS sbrushc ${SRC}
        COMMENT "sbrush wgsl + tint: ${STEM}.wgsl"
        VERBATIM)
      list(APPEND WGSL_OUTPUTS "${STAMP}")
    else()
      add_custom_command(
        OUTPUT "${OUT}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=wgsl --in=${SRC} --out=${OUT}
        DEPENDS sbrushc ${SRC}
        COMMENT "sbrush wgsl: ${STEM}.wgsl"
        VERBATIM)
      list(APPEND WGSL_OUTPUTS "${OUT}")
    endif()
  endforeach()
  add_custom_target(sbrush-wgsl ALL DEPENDS ${WGSL_OUTPUTS})
  sbrush_backend(wgsl tint)
endif()

# SPIR-V backend: chain sbrushc(--backend=spirv) -> tint(--format=spirv)
# -> spirv-val. sbrushc's spirv backend emits WGSL (the tint input) per the
# design doc's "WGSL+Tint" lowering option; a future slice can swap the tint
# step for a direct emit_spirv.cc backend without touching this wiring. The
# .spv artifact is what a future Vulkan dispatcher would consume.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_SPIRV)
  target_compile_definitions(brush PUBLIC SBRUSH_BACKEND_SPIRV=1)
  file(GLOB SBRUSH_INPUTS_SPV "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.sbrush")
  set(SPV_OUT_DIR "${CMAKE_BINARY_DIR}/sbrush_out/spirv")
  file(MAKE_DIRECTORY "${SPV_OUT_DIR}")
  set(SPV_OUTPUTS)
  foreach(SRC ${SBRUSH_INPUTS_SPV})
    get_filename_component(STEM "${SRC}" NAME_WE)
    set(WGSL "${SPV_OUT_DIR}/${STEM}.wgsl")
    set(SPV  "${SPV_OUT_DIR}/${STEM}.spv")
    set(STAMP "${SPV}.validated")
    if (SBRUSH_VALIDATE_ALL)
      add_custom_command(
        OUTPUT "${STAMP}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=spirv --in=${SRC} --out=${WGSL}
        COMMAND ${SBRUSH_WGSL_VALIDATOR} ${WGSL} --format=spirv -o ${SPV}
        COMMAND ${SBRUSH_SPIRV_VALIDATOR} ${SPV}
        COMMAND ${CMAKE_COMMAND} -E touch ${STAMP}
        DEPENDS sbrushc ${SRC}
        COMMENT "sbrush spirv (via tint) + spirv-val: ${STEM}.spv"
        VERBATIM)
      list(APPEND SPV_OUTPUTS "${STAMP}")
    else()
      add_custom_command(
        OUTPUT "${SPV}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=spirv --in=${SRC} --out=${WGSL}
        COMMAND ${SBRUSH_WGSL_VALIDATOR} ${WGSL} --format=spirv -o ${SPV}
        DEPENDS sbrushc ${SRC}
        COMMENT "sbrush spirv (via tint): ${STEM}.spv"
        VERBATIM)
      list(APPEND SPV_OUTPUTS "${SPV}")
    endif()
  endforeach()
  add_custom_target(sbrush-spirv ALL DEPENDS ${SPV_OUTPUTS})
  sbrush_backend(spirv spirv-val)
endif()

