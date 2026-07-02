set(LIB
    util
    math
    binding
    mesh
)

set(SRC
  PUBLIC compositor.h
  compositor.cc
  PUBLIC frames.h
  frames.cc
)

sc_add_library(displace "${SRC}" "${LIB}" STATIC)
