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
  PUBLIC meshlog.h
  bindings.cc

)

sc_add_library(meshlog "${SRC}" "${LIB}" STATIC)

