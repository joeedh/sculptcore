cmake_minimum_required(VERSION 3.15)
project(sculptcore_napi_spike CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# cmake-js injects:
#   CMAKE_JS_INC  - node + napi C headers for the chosen runtime (electron)
#   CMAKE_JS_LIB  - the import lib (node.lib) for that runtime/version
#   CMAKE_JS_SRC  - win_delay_load_hook.cc; delay-loads host symbols on Windows
include_directories(${CMAKE_JS_INC})

add_library(${PROJECT_NAME} SHARED hello.cc ${CMAKE_JS_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" SUFFIX ".node")
target_link_libraries(${PROJECT_NAME} ${CMAKE_JS_LIB})

# node-addon-api (the C++ Napi:: wrapper) — resolve its header dir from the
# local install rather than hard-coding a path.
execute_process(
  COMMAND node -p "require('node-addon-api').include_dir"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NODE_ADDON_API_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
target_include_directories(${PROJECT_NAME} PRIVATE ${NODE_ADDON_API_DIR})

# Target a concrete N-API level (8 = Node 16 / Electron's floor here) so the
# addon is ABI-stable across Electron bumps without recompiling.
target_compile_definitions(${PROJECT_NAME} PRIVATE NAPI_VERSION=8)

# cmake-js passes "/DELAYLOAD:NODE.EXE" as a bare CMAKE_SHARED_LINKER_FLAG. That
# is link.exe syntax; our linker *driver* is clang++ (gcc-style), which treats a
# bare "/DELAYLOAD:NODE.EXE" as an input filename ("no such file or directory").
# Strip cmake-js's form and re-add it through CMake's LINKER: passthrough, which
# clang expands to "-Xlinker /DELAYLOAD:node.exe". Delay-loading node.exe is what
# lets the .node bind its node/napi imports to the actual host process
# (electron.exe, not node.exe) at load time — required for addons on Windows.
# This MSVC-vs-clang flag-syntax fix is the reusable takeaway for Workstream A.
string(REPLACE "/DELAYLOAD:NODE.EXE" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
target_link_options(${PROJECT_NAME} PRIVATE "LINKER:/DELAYLOAD:node.exe")
target_link_libraries(${PROJECT_NAME} delayimp.lib)
