set(LIB
    util
    math
    binding
    io
    mesh
)

set(SRC
  PUBLIC subdiv.h
  subdiv.cc
  PUBLIC grids.h
  grids.cc
)

sc_add_library(subdiv "${SRC}" "${LIB}" STATIC)
