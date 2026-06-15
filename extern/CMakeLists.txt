if (WITH_ABSEIL)
add_subdirectory(abseil-cpp)
endif()

if (NOT BUILD_WASM)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(glfw)
  
  # GLFW is used Vulkan-only in this project; tell glfw3.h not to pull
  # in <GL/gl.h> for every consumer that links glfw. INTERFACE so the
  # define doesn't leak into GLFW's own TUs (its internal.h forbids it).
  target_compile_definitions(glfw INTERFACE GLFW_INCLUDE_NONE)

  # Vulkan is found at the root via find_package(Vulkan REQUIRED) so the
  # Vulkan::Vulkan imported target is visible to all subdirectories.

  include("${CMAKE_CURRENT_SOURCE_DIR}/imgui_build.cmake")

  # Native WebGPU backend impl (prebuilt). emdawnwebgpu covers the WASM side.
  add_subdirectory(wgpu_native)

  # --- Prebuilt OpenBLAS + SuiteSparse/CHOLMOD ----------------------------------
  # Built per platform/toolchain/config by tools/deps.mjs and staged in the
  # gitignored sparse clone extern/sculptcore-deps; `make.mjs configure native`
  # passes the combo dir as SCULPTCORE_DEPS_DIR. We expose one aggregate
  # `cholmod` INTERFACE target (globally visible to source/, added after extern/).
  if(DEFINED SCULPTCORE_DEPS_DIR AND EXISTS "${SCULPTCORE_DEPS_DIR}/suitesparse")
    set(_ss_inc "${SCULPTCORE_DEPS_DIR}/suitesparse/include/suitesparse")
    set(_ss_lib "${SCULPTCORE_DEPS_DIR}/suitesparse/lib")
    set(_ob_lib "${SCULPTCORE_DEPS_DIR}/openblas/lib")

    add_library(cholmod INTERFACE)
    target_include_directories(cholmod INTERFACE
      "${_ss_inc}"
      "${SCULPTCORE_DEPS_DIR}/openblas/include")
    target_compile_definitions(cholmod INTERFACE WITH_CHOLMOD EIGEN_CHOLMOD_SUPPORT)

    # Link in dependency order (cholmod -> its deps -> openblas last); glob each
    # so static-lib naming (lib*.a vs *.lib) is handled across platforms.
    foreach(_pkg cholmod ccolamd camd colamd amd suitesparseconfig)
      # SuiteSparse static libs install as <pkg>_static.{lib,a}; also accept the
      # plain name in case a future build drops the suffix.
      file(GLOB _hit
        "${_ss_lib}/lib${_pkg}_static.a" "${_ss_lib}/${_pkg}_static.lib"
        "${_ss_lib}/lib${_pkg}.a" "${_ss_lib}/${_pkg}.lib" "${_ss_lib}/lib${_pkg}.lib")
      if(NOT _hit)
        message(FATAL_ERROR "SuiteSparse prebuilt missing ${_pkg} under ${_ss_lib} (rerun: node make.mjs deps)")
      endif()
      list(GET _hit 0 _lib)
      target_link_libraries(cholmod INTERFACE "${_lib}")
    endforeach()

    file(GLOB _ob "${_ob_lib}/libopenblas*.a" "${_ob_lib}/openblas*.lib" "${_ob_lib}/libopenblas*.lib")
    if(NOT _ob)
      message(FATAL_ERROR "OpenBLAS prebuilt missing under ${_ob_lib} (rerun: node make.mjs deps)")
    endif()
    list(GET _ob 0 _ob_first)
    target_link_libraries(cholmod INTERFACE "${_ob_first}")
    if(NOT WIN32)
      target_link_libraries(cholmod INTERFACE pthread m ${CMAKE_DL_LIBS})
    endif()

    # The prebuilt CHOLMOD is compiled with OpenMP; consumers must supply the
    # OpenMP runtime (omp_*/__kmpc_* symbols) or every linked exe fails.
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
      target_link_libraries(cholmod INTERFACE OpenMP::OpenMP_CXX)
    else()
      message(WARNING "OpenMP not found; prebuilt CHOLMOD may fail to link "
                      "(undefined omp_*/__kmpc_* symbols)")
    endif()
  else()
    message(WARNING "SCULPTCORE_DEPS_DIR not set/found; CHOLMOD support disabled "
                    "(run: node make.mjs deps). Eigen SimplicialLDLT still works.")
  endif()
endif()
