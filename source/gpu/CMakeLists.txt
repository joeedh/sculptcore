set(LIB
    eigen
    util
    math
    PUBLIC glfw
    PUBLIC glew
)

set(SRC
  PUBLIC batch.h
  PUBLIC pipeline.h
  PUBLIC shader.h
  PUBLIC texture.h
  PUBLIC types.h
  PUBLIC vbo.h
  vbo.cc
)

sc_add_library(gpu "${SRC}" "${LIB}" STATIC)

