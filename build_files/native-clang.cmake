set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)

# Use sccache as a compiler launcher when present (auto-detected on PATH).
# Cross-worktree cache sharing goes through a small wrapper
# (tools/sccache-wrapper) that keeps the union of every live worktree's root in
# SCCACHE_BASEDIRS, so one sccache server caches for all worktrees at once. The
# wrapper binary is built into the shared sibling dir C:/dev/sccache-worktrees
# by tools/sccache-wrapper/setup.mjs (run from make.mjs configure native and
# new-worktree.mjs). If it isn't there (sculptcore built standalone, or setup
# never ran) we fall back to plain sccache. Opt out entirely with
# -DSCULPTCORE_NO_SCCACHE=ON, or override the wrapper path with
# -DSCULPTCORE_SCCACHE_LAUNCHER=<path>. configureEnv.mjs re-adds an on-PATH
# sccache to the build PATH on Windows (vcvars rebuilds PATH from scratch), so
# find_program/the wrapper can see it here.
if(NOT SCULPTCORE_NO_SCCACHE)
  # Shared dir is a sibling of the worktrees: <build_files>/../../../sccache-worktrees
  if(WIN32)
    set(_sccache_launcher_name "sccache-launcher.exe")
  else()
    set(_sccache_launcher_name "sccache-launcher")
  endif()
  if(DEFINED SCULPTCORE_SCCACHE_LAUNCHER)
    set(_sccache_launcher "${SCULPTCORE_SCCACHE_LAUNCHER}")
  else()
    get_filename_component(_sccache_launcher
      "${CMAKE_CURRENT_LIST_DIR}/../../../sccache-worktrees/${_sccache_launcher_name}" REALPATH)
  endif()

  if(EXISTS "${_sccache_launcher}")
    set(CMAKE_C_COMPILER_LAUNCHER   "${_sccache_launcher}" CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_sccache_launcher}" CACHE FILEPATH "" FORCE)
    message(STATUS "sculptcore: sccache cross-worktree launcher enabled (${_sccache_launcher})")
  else()
    find_program(SCCACHE_EXECUTABLE NAMES sccache)
    if(SCCACHE_EXECUTABLE)
      set(CMAKE_C_COMPILER_LAUNCHER   "${SCCACHE_EXECUTABLE}" CACHE FILEPATH "" FORCE)
      set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_EXECUTABLE}" CACHE FILEPATH "" FORCE)
      message(STATUS "sculptcore: sccache compiler launcher enabled (${SCCACHE_EXECUTABLE})")
    else()
      message(STATUS "sculptcore: sccache not found on PATH; building without compiler cache")
    endif()
  endif()
endif()
