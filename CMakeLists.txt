cmake_minimum_required(VERSION 3.15)
project(sculptcore C CXX)

include("build_files/macros.cmake")

set(CMAKE_CXX_STANDARD_REQUIRED true)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_SOURCE_FILE_EXTENSIONS .cc)

add_subdirectory(extern)
add_subdirectory(source)

set(LIB
  PUBLIC util
)

add_library(sculptcore SHARED)
target_link_libraries(sculptcore ${LIB})
