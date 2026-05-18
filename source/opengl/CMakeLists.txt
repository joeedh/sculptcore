set(LIB
  util
  math
  gpu
  glew
)

set(SRC
  PUBLIC gl_context.h
  PUBLIC gl_backend.h
  PUBLIC gl_overlay.h
  PUBLIC gl_screenshot.h
  gl_context.cc
  gl_backend.cc
  gl_overlay.cc
  gl_screenshot.cc
)

sc_add_library(opengl "${SRC}" "${LIB}" STATIC)
target_include_directories(opengl PUBLIC "${CMAKE_SOURCE_DIR}/extern")
