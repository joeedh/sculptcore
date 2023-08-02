set(LIB
	util
	platform
	math
)

set(SRC
  PUBLIC window.h
  window.cc
)

sc_add_library(window "${SRC}" "${LIB}" OBJECT)
target_include_directories(window PUBLIC ../../extern/glfw/include)
