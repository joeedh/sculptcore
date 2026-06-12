# Standalone quad-remesh CLI. A self-contained exe (statically links the module
# libs, like the test targets) so it can be rebuilt — `cmake --build <dir> --target
# remesh_cli` — without touching the running debug app's locked exe.

# sculptcore root, independent of CMAKE_SOURCE_DIR (cli is source/remesh/cli).
get_filename_component(_sc_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

find_package(Git QUIET)
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${_sc_root}
    OUTPUT_VARIABLE REMESH_CLI_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT REMESH_CLI_GIT_COMMIT)
  set(REMESH_CLI_GIT_COMMIT "unknown")
endif()

set(REMESH_CLI_ASSETS_DIR  "${_sc_root}/tests/assets")
set(REMESH_CLI_RESULTS_DIR "${_sc_root}/tests/remesher-results")

configure_file(remesh_cli_config.h.in remesh_cli_config.h @ONLY)

add_executable(remesh_cli remesh_cli.cc asset_quads.cc)
target_link_libraries(remesh_cli remesh mesh spatial util platform math)
# The configured config header (paths + git commit).
target_include_directories(remesh_cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
if(MSVC)
  target_compile_options(remesh_cli PRIVATE /std:c++20)
endif()
