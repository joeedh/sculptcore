set(LIB
    eigen
    util
)

set(SRC
  PUBLIC vector.h
  PUBLIC color.h
  color.cc
)

sc_add_library(math "${SRC}" "${LIB}" STATIC)

