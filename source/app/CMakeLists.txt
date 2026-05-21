set(SRC
  app.cc
)

add_executable(app "${SRC}")
target_link_libraries(app util mesh math platform window gpu glfw)
target_include_directories(app PUBLIC
  ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/extern
)
if(MSVC)
  target_compile_options(app PUBLIC /std:c++20)
endif()
