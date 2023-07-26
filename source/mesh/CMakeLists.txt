set(LIB
    eigen
    util
)

set(SRC
  PUBLIC attribute.h
  PUBLIC mesh.h
  PUBLIC mesh_enums.h
  attribute.cc
  mesh.cc
)

sc_add_library(mesh "${SRC}" "${LIB}" STATIC)

