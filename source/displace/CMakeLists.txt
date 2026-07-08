set(LIB
    util
    math
    binding
    mesh
)

set(SRC
  PUBLIC compositor.h
  compositor.cc
  PUBLIC frames.h
  frames.cc
  c-api/displace_c_api.cc
)

sc_add_library(displace "${SRC}" "${LIB}" STATIC)

wasm_add_symbols("
  Mesh_layerSetWeight
  Mesh_layerSetEnabled
  Mesh_layerSetFrozen
  Mesh_layerRemove
  Mesh_setActiveEditLayer
  Mesh_layerFold
")
