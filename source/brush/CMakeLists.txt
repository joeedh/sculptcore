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
  PUBLIC command.h
  PUBLIC brush_iter.h
  command.cc
  brush.cc
)

sc_add_library(brush "${SRC}" "${LIB}" STATIC)

