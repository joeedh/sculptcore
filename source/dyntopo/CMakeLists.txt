set(LIB
    eigen
    util
    math
    binding
    mesh
    spatial
)

set(SRC
  PUBLIC bindings.h
  PUBLIC dyntopo.h
  bindings.cc
)

sc_add_library(dyntopo "${SRC}" "${LIB}" STATIC)
