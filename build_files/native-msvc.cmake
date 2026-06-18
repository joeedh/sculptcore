# MSVC (cl.exe) toolchain for the native build, selected by WITH_NATIVE_MSVC in
# local-build-options.mjs (make.mjs points --toolchain here instead of
# native-clang.cmake). cl.exe / link.exe and the INCLUDE/LIB environment come
# from the vcvars64 setup that configureEnv.mjs applies before cmake runs.
#
# sccache is intentionally NOT wired here: caching MSVC objects needs /Z7
# (Embedded debug info), which RelWithDebInfo's default /Zi defeats — sccache
# would just mark every object non-cacheable. Keep the MSVC toolchain simple and
# uncached; the clang toolchain (native-clang.cmake) remains the cached default.
set(CMAKE_C_COMPILER cl)
set(CMAKE_CXX_COMPILER cl)
