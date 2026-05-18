set(LIB
    eigen
    util
    math
    props
    spatial
    mesh
)

set(SRC
  PUBLIC bindings.h
  PUBLIC meshlog_base.h
  PUBLIC meshlog.h
  bindings.cc

)

sc_add_library(meshlog "${SRC}" "${LIB}" STATIC)

