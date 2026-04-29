set(LIB
    PUBLIC math
    PUBLIC util
    PUBLIC binding
    PUBLIC mesh
    PUBLIC sculptcore_wasm
    PUBLIC gpu
    PUBLIC spatial
)

set(SRC
  bindings.cc
)

sc_add_library(sculptcore_core "${SRC}" "${LIB}" STATIC)
wasm_add_symbols("
initBindings
")
