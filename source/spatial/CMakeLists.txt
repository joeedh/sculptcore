set(LIB
	mesh
	util
	math
	draw
	gpu
)

set(SRC
  PUBLIC node.h
  PUBLIC spatial.h
  PUBLIC spatial_enums.h
  node.cc
  spatial.cc
  spatial_gpu.cc
)

sc_add_library(spatial "${SRC}" "${LIB}" OBJECT)
