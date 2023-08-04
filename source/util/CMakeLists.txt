set(LIB
    eigen
)

set(SRC
  PUBLIC string.h
  PUBLIC map.h
  PUBLIC boolvector.h
  PUBLIC set.h
  PUBLIC alloc.h
  PUBLIC vector.h
  PUBLIC compiler_util.h
  PUBLIC rand.h
  PUBLIC time.h
  PUBLIC task.h
  PUBLIC ordered_set.h
  alloc.cc
  util.cc
)

sc_add_library(util "${SRC}" "${LIB}" STATIC)

