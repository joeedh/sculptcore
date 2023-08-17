set(LIB
    PUBLIC math
    PUBLIC util
    PUBLIC io
    gpu
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
  attribute.cc
  idmap.cc
  mesh.cc
  mesh_shapes.cc
  mesh_types.cc
)

sc_add_library(mesh "${SRC}" "${LIB}" STATIC)

