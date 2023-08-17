set(LIB
    eigen
    util
    math
    props
)

set(SRC
  PUBLIC brush.h
  PUBLIC test.h
  PUBLIC exec.h
  PUBLIC props.h
  brush.cc
)

sc_add_library(brush "${SRC}" "${LIB}" STATIC)

