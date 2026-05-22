set(LIB
    eigen
    util
    math
    props
    spatial
    meshlog
    platform
)

set(SRC
  PUBLIC brush.h
  PUBLIC test.h
  PUBLIC exec.h
  PUBLIC props.h
  PUBLIC brush_iterators.h
  PUBLIC brush_command.h
  PUBLIC brush_concepts.h
  PUBLIC stroke_spacing.h
  PUBLIC brushes/all.h
  brushes/draw.h
  brush_command.cc
  brush_executor.h
  brush_executor.cc
  brush.cc
  PUBLIC bindings.h
  bindings.cc
)

sc_add_library(brush "${SRC}" "${LIB}" STATIC)

