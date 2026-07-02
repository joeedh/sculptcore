set(LIB
    PUBLIC math
    PUBLIC util
    PUBLIC binding
    PUBLIC gpu
    PUBLIC io
)

set(SRC
  PUBLIC attribute.h
  PUBLIC attribute_bool.h
  PUBLIC attribute_builtin.h
  PUBLIC attribute_enums.h
  PUBLIC mesh.h
  PUBLIC mesh_enums.h
  PUBLIC mesh_base.h
  PUBLIC mesh_callbacks.h
  PUBLIC mesh_types.h
  PUBLIC mesh_topo_cache.h
  PUBLIC mesh_serialize.h
  PUBLIC mesh_shapes.h
  PUBLIC boundary.h
  PUBLIC mesh_path.h
  PUBLIC uvgen.h
  PUBLIC idmap.h
  PUBLIC bindings.h
  PUBLIC gpu/mesh_drawbatch.h
  gpu/mesh_drawbatch.cc

  c-api/mesh_c_api.h
  c-api/mesh_c_api.cc
  attribute.cc
  utils/select_derive.cc
  boundary.cc
  mesh_path.cc
  uvgen.cc
  idmap.cc
  mesh.cc
  mesh_topo_cache.cc
  mesh_serialize.cc
  mesh_shapes.cc
  mesh_types.cc
  bindings.cc
  # Native log sink (napi/napi_log.h). Lives here — the lowest lib that calls it
  # (edge_split.h, spatial.cc via spatial->mesh) — so every consumer links it.
  napi_log.cc
)

sc_add_library(mesh "${SRC}" "${LIB}" STATIC)
wasm_add_symbols("
  getAttrs
  getAttrName
  getAttr
  freeAttrRef
  copyAttrRef
  makeVertex
  freeMesh
  createMesh
  getStrData
  Mesh_createCube
  Mesh_makeGrid
  Mesh_makeCylinder
  Mesh_makeTorus
  Mesh_makeUVSphere
  Mesh_triangulate
  Mesh_ngonFaceCount
  serializeMesh
  serializeMeshRaw
  deserializeMesh
  freeMeshBuffer
  "
)
