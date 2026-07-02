set(LIB
    util
    math
    binding
    io
    mesh
    displace
    spatial
    meshlog
)

set(SRC
  PUBLIC vdm_store.h
  vdm_store.cc
  PUBLIC vdm_splat.h
  vdm_splat.cc
  PUBLIC vdm_undo.h
)

sc_add_library(vdm "${SRC}" "${LIB}" STATIC)
