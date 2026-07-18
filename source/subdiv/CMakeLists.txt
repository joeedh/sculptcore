set(LIB
    util
    math
    binding
    io
    mesh
    displace
    spatial
    vdm
)

set(SRC
  PUBLIC subdiv.h
  subdiv.cc
  PUBLIC grids.h
  grids.cc
  PUBLIC multires.h
  multires.cc
  PUBLIC bindings.h
  bindings.cc
  c-api/subdiv_c_api.cc
)

sc_add_library(subdiv "${SRC}" "${LIB}" STATIC)

wasm_add_symbols("
  Multires_new
  Multires_free
  Multires_setActiveLevel
  Multires_activeMesh
  Multires_activeTree
  Multires_levelSampleCount
  Multires_levelPositionsOut
  Multires_fromLevelPositions
  Multires_writeback
  Multires_captureToVdm
  Multires_downRefit
  Multires_serializeStore
  Multires_restoreStore
")
