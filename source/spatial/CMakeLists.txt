set(LIB
	mesh
	util
	math
	gpu
	binding
)

set(SRC
  PUBLIC node.h
  PUBLIC spatial.h
  PUBLIC spatial_enums.h
  PUBLIC spatial_attrs.h
  PUBLIC bindings.h
  node.cc
  spatial.cc
  bindings.cc
  c-api/spatial_c_api.cc
)

sc_add_library(spatial "${SRC}" "${LIB}" OBJECT)
wasm_add_symbols("
  Mesh_buildSpatialTree
  SpatialTree_free
")
