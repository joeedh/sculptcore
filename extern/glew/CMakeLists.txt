set(LIB
  PUBLIC "${CMAKE_SOURCE_DIR}/extern/glew/lib/win32/glew32.lib"
  PUBLIC "${CMAKE_SOURCE_DIR}/extern/glew/lib/win32/freeglut_static.lib"
)

set(SRC
  PUBLIC GL/glew.h
  PUBLIC gl/freeglut.h
  glew.cc
)

sc_add_library(glew "${SRC}" "${LIB}" OBJECT)
