set(LIB
    eigen
    util
    math
)

set(SRC
  PUBLIC props.h
  PUBLIC prop_enums.h
  PUBLIC prop_struct.h
  PUBLIC prop_types.h
  props.cc
)

sc_add_library(props "${SRC}" "${LIB}" STATIC)

