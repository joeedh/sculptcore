set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)

# Use sccache as a compiler launcher when present (auto-detected on PATH).
# Cross-worktree cache sharing relies on SCCACHE_BASEDIRS being set to each
# worktree's own root (the create-worktree skill / tools/new-worktree.mjs does
# this). Opt out with -DSCULPTCORE_NO_SCCACHE=ON. configureEnv.mjs re-adds an
# on-PATH sccache to the build PATH on Windows (vcvars rebuilds PATH from
# scratch), so find_program can see it here.
if(NOT SCULPTCORE_NO_SCCACHE)
  find_program(SCCACHE_EXECUTABLE NAMES sccache)
  if(SCCACHE_EXECUTABLE)
    set(CMAKE_C_COMPILER_LAUNCHER   "${SCCACHE_EXECUTABLE}" CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_EXECUTABLE}" CACHE FILEPATH "" FORCE)
    message(STATUS "sculptcore: sccache compiler launcher enabled (${SCCACHE_EXECUTABLE})")
  else()
    message(STATUS "sculptcore: sccache not found on PATH; building without compiler cache")
  endif()
endif()
