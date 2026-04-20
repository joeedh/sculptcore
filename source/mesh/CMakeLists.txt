set(LIB
    PUBLIC math
    PUBLIC util
    PUBLIC binding
)

set(SRC
  PUBLIC attribute.h
  PUBLIC attribute_bool.h
  PUBLIC attribute_builtin.h
  PUBLIC attribute_enums.h
  PUBLIC mesh.h
  PUBLIC mesh_enums.h
  PUBLIC mesh_base.h
  PUBLIC mesh_types.h
  PUBLIC mesh_shapes.h
  PUBLIC idmap.h
  PUBLIC bindings.h

  c-api/mesh_c_api.h
  c-api/mesh_c_api.cc
  attribute.cc
  idmap.cc
  mesh.cc
  mesh_shapes.cc
  mesh_types.cc
  bindings.cc
)

sc_add_library(mesh "${SRC}" "${LIB}" STATIC)
wasm_add_symbols("
  memRelease
  memAlloc
  getAttrs
  getAttrName
  getAttr
  freeAttrRef
  copyAttrRef
  makeVertex
  freeMesh
  createMesh
  getStrData
  printMemBlocks
  _rawAlloc
  _rawRelease
  "
)
