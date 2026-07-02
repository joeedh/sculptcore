set(LIB
    util
    math
    binding
    mesh
)

set(SRC
  PUBLIC subdiv.h
  subdiv.cc
)

sc_add_library(subdiv "${SRC}" "${LIB}" STATIC)
