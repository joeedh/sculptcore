set(LIB
    eigen
    util
    math
    binding
)

set(SRC
  PUBLIC batch.h
  PUBLIC pipeline.h
  PUBLIC shader.h
  PUBLIC texture.h
  PUBLIC types.h
  PUBLIC vbo.h
  PUBLIC bindings.h
  PUBLIC standard_attrs.h
  PUBLIC manager.h
  manager.cc
  vbo.cc
  shader.cc
)

sc_add_library(gpu "${SRC}" "${LIB}" STATIC)

