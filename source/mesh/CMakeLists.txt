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
  PUBLIC attr_merge.h
  PUBLIC attr_weights.h
  PUBLIC deform_pool.h
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
  PUBLIC uv_reproject.h
  PUBLIC idmap.h
  PUBLIC bindings.h
  PUBLIC gpu/mesh_drawbatch.h
  gpu/mesh_drawbatch.cc

  c-api/mesh_c_api.h
  c-api/mesh_c_api.cc
  attribute.cc
  attr_merge.cc
  attr_weights.cc
  deform_pool.cc
  utils/select_derive.cc
  boundary.cc
  mesh_path.cc
  uvgen.cc
  uv_reproject.cc
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
  Mesh_fromArrays
  Mesh_arraySizes
  Mesh_toArrays
  Mesh_topoStamp
  Mesh_readVertFloatAttr
  Mesh_writeVertFloatAttr
  Mesh_readFaceIntAttr
  Mesh_writeFaceIntAttr
  Mesh_readVertFloat4Attr
  Mesh_writeVertFloat4Attr
  Mesh_writeCornerFloat2Attr
  Mesh_readAttr
  Mesh_writeAttr
  sc_mesh_weights_element_count
  sc_mesh_weights_get
  sc_mesh_weights_set
  sc_mesh_weight_group_count
  sc_mesh_weight_groups_get
  sc_mesh_weight_groups_set
  Mesh_edgeCount
  Mesh_writeEdgeFlagsByVerts
  Mesh_readEdgeFlags
  Mesh_recomputeBoundary
  Mesh_generateUVFromSeams
  serializeMesh
  serializeMeshRaw
  deserializeMesh
  freeMeshBuffer
  "
)
