set(LIB
    eigen
)

set(SRC
  PUBLIC string.h
  map.h
  set.h
  util.cc
)

sc_add_library(util "${SRC}" "${LIB}" STATIC)

