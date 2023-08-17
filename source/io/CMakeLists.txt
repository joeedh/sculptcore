set(LIB
	util
	platform
	math
)

set(SRC
  PUBLIC serial.h
  serial.cc
)

sc_add_library(io "${SRC}" "${LIB}" OBJECT)

