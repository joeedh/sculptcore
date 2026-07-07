set(LIB
    util
    math
    binding
    io
    mesh
    displace
    spatial
    meshlog
)

set(SRC
  PUBLIC vdm_store.h
  vdm_store.cc
  PUBLIC vdm_splat.h
  vdm_splat.cc
  PUBLIC vdm_gpu.h
  vdm_gpu.cc
  PUBLIC vdm_promote.h
  vdm_promote.cc
  PUBLIC vdm_undo.h
  PUBLIC bindings.h
  bindings.cc
  c-api/vdm_c_api.cc
)

sc_add_library(vdm "${SRC}" "${LIB}" STATIC)

wasm_add_symbols("
  VdmStore_new
  VdmStore_free
  Mesh_vdmSplatDab
  Mesh_vdmSplatDabLogged
  Vdm_lastSplatClamped
  VdmStore_serialize
  VdmStore_deserialize
  SpatialTree_fillDetailCarrier
  Mesh_updateFrames
")
