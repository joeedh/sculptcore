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

