set(LIB
	mesh
	util
	math
	draw
)

set(SRC
  PUBLIC spatial.h
  PUBLIC node.h
  spatial.cc
  node.cc
)

sc_add_library(spatial "${SRC}" "${LIB}" OBJECT)
