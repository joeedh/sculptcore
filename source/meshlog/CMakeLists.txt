set(LIB
    eigen
    util
    math
    props
    spatial
    mesh
)

if (WITH_ABSEIL)
set(LIB ${LIB}
    absl::flat_hash_map
)
endif()

set(SRC
  PUBLIC bindings.h
  PUBLIC meshlog_base.h
  PUBLIC meshlog_chunk.h
  PUBLIC meshlog_reorder.h
  PUBLIC meshlog_row.h
  PUBLIC meshlog_topo.h
  PUBLIC meshlog_types.h
  PUBLIC meshlog.h
  bindings.cc
  meshlog_editops.cc
  meshlog_lifecycle.cc
  meshlog_preview.cc
  meshlog_selection.cc
  meshlog_undo.cc
)

sc_add_library(meshlog "${SRC}" "${LIB}" STATIC)

