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
  PUBLIC prop_curve.h
  PUBLIC bindings.h
  PUBLIC prop_coerce.h
  PUBLIC prop_types.h
  prop_inherit.cc
  prop_struct.cc
  props.cc
  curve_cache.cc
  bindings.cc
)

sc_add_library(props "${SRC}" "${LIB}" STATIC)

