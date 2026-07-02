set(LIB
    util
    math
    binding
    io
    mesh
)

set(SRC
  PUBLIC vdm_store.h
  vdm_store.cc
)

sc_add_library(vdm "${SRC}" "${LIB}" STATIC)
