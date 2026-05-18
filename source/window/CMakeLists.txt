set(LIB
  util
  platform
  math
  gpu
  glew
)

set(SRC
  PUBLIC window.h
  window.cc
)

sc_add_library(window "${SRC}" "${LIB}" OBJECT)
target_include_directories(window PUBLIC ${CMAKE_SOURCE_DIR}/extern/glfw/include)
