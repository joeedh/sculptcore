set(LIB
    eigen
    util
    math
)

set(SRC
  PUBLIC draw_abstract.h
  draw.cc
)

sc_add_library(draw "${SRC}" "${LIB}" STATIC)

