set(LIB
    util
    math
    binding
    io
    mesh
    displace
    spatial
)

set(SRC
  PUBLIC subdiv.h
  subdiv.cc
  PUBLIC grids.h
  grids.cc
  PUBLIC multires.h
  multires.cc
)

sc_add_library(subdiv "${SRC}" "${LIB}" STATIC)
