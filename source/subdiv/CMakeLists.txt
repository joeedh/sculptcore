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
  PUBLIC grid_domain.h
  grid_domain.cc
  grid_draw_source.cc
  PUBLIC grid_tree.h
  grid_tree.cc
  PUBLIC grid_stroke_log.h
  grid_stroke_log.cc
  PUBLIC multires.h
  multires.cc
  PUBLIC multires_tuning.h
  multires_tuning.cc
  PUBLIC bindings.h
  bindings.cc
  c-api/grid_draw_c_api.cc
  c-api/subdiv_c_api.cc
)

sc_add_library(subdiv "${SRC}" "${LIB}" STATIC)

wasm_add_symbols("
  sc_external_draw_register_grids
  Multires_new
  Multires_free
  Multires_setActiveLevel
  Multires_setActiveLevelLazy
  Multires_levelVertCount
  Multires_tuningStats
  Multires_readDomainMask
  Multires_writeDomainMask
  Multires_activeMesh
  Multires_activeTree
  Multires_levelSampleCount
  Multires_levelPositionsOut
  Multires_fromLevelPositions
  Multires_seedLevelPositions
  Multires_writeback
  Multires_captureToVdm
  Multires_downRefit
  Multires_maxLevel
  Multires_addLevel
  Multires_removeTopLevel
  Multires_serializeStore
  Multires_restoreStore
")
