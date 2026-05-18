set(SRC
  app.cc
)

if(WIN32)
  set(GL opengl32.lib)
else()
  set(GL GL)
endif()

add_executable(app "${SRC}")
target_link_libraries(app util mesh math platform window gpu glfw glew ${GL})
target_include_directories(app PUBLIC
  ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/extern
)
if(MSVC)
  target_compile_options(app PUBLIC /std:c++20)
endif()
