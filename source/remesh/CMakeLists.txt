set(LIB
    eigen
    util
    math
    binding
    mesh
    spatial
    sculptcore_core
)

set(SRC
  PUBLIC remesh.h
  PUBLIC preremesh.h
  PUBLIC remesh_params.h
  PUBLIC remesh_report.h
  PUBLIC triage.h
  PUBLIC bindings.h
  PUBLIC field/curvature.h
  PUBLIC field/feature_tag.h
  PUBLIC field/cross_field.h
  PUBLIC field/constraints.h
  PUBLIC field/density.h
  PUBLIC field/singularity_adjust.h
  PUBLIC param/cut_graph.h
  PUBLIC param/seamless_param.h
  param/seamless_internal.h
  PUBLIC quantize/t_mesh.h
  PUBLIC quantize/quantize_ilp.h
  PUBLIC extract/quad_extract.h
  PUBLIC extract/reproject.h
  remesh.cc
  preremesh.cc
  triage.cc
  bindings.cc
  field/curvature.cc
  field/feature_tag.cc
  field/cross_field.cc
  field/constraints.cc
  field/density.cc
  field/singularity_adjust.cc
  param/cut_graph.cc
  param/seamless_param.cc
  param/seamless_internal.cc
  quantize/t_mesh.cc
  quantize/quantize_ilp.cc
  extract/quad_extract.cc
  extract/reproject.cc
  c-api/remesh_c_api.cc
)

# Prebuilt SuiteSparse/CHOLMOD (+ OpenBLAS), native-only; see
# sculptcore/extern/CMakeLists.txt. Enables Eigen/CholmodSupport in the solver.
if(TARGET cholmod)
  list(APPEND LIB cholmod)
endif()

sc_add_library(remesh "${SRC}" "${LIB}" STATIC)

# Global quad-remesh op, exposed like Mesh_triangulate. Hand-wrapped natively in
# source/napi/napi_runtime.cc; this list wires the WASM export.
wasm_add_symbols("
  Mesh_quadRemesh
  "
)

# Standalone remesh CLI: a native exe (std::filesystem + platform lib), not a
# WASM target. The debug app spawns it as a subprocess so it can be rebuilt
# (--target remesh_cli) without restarting the app.
if(NOT BUILD_WASM)
  add_subdirectory(cli)
endif()
