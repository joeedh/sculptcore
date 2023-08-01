set(LIB
    eigen
    math
    util
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
  attribute.cc
  mesh.cc
)

sc_add_library(mesh "${SRC}" "${LIB}" STATIC)

