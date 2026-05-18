if(WIN32)
  set(LIB
    PUBLIC "${CMAKE_SOURCE_DIR}/extern/glew/lib/win32/glew32s.lib"
  )
else()
  set(LIB "")
endif()

set(SRC
  PUBLIC GL/glew.h
  glew.cc
)

sc_add_library(glew "${SRC}" "${LIB}" OBJECT)

# glew32s.lib is the static build of GLEW; consumers must define GLEW_STATIC
# so the headers don't try to declspec-import.
target_compile_definitions(glew PUBLIC GLEW_STATIC)
