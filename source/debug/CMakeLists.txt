set(LIB_SRC
  PUBLIC scene.h
  PUBLIC script.h
  PUBLIC state_dump.h
  PUBLIC camera.h
  scene.cc
  script.cc
  state_dump.cc
)

set(LIB
  util math platform
  mesh meshlog brush spatial gpu
  window vulkan
)

sc_add_library(debug_core "${LIB_SRC}" "${LIB}" STATIC)

target_link_libraries(debug_core glfw Vulkan::Vulkan)
target_include_directories(debug_core PUBLIC
  ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/extern
)

add_executable(debug_app debug_app.cc)
target_link_libraries(debug_app debug_core)
target_include_directories(debug_app PUBLIC
  ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/extern
)
if(MSVC)
  target_compile_options(debug_app PUBLIC /std:c++20)
endif()
