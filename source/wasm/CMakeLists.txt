set(LIB
    PUBLIC math
    PUBLIC util
    PUBLIC binding
)

set(SRC
  PUBLIC wasmManager.h
  wasmManager.cc
)

sc_add_library(sculptcore_wasm "${SRC}" "${LIB}" STATIC)
wasm_add_symbols("
getBindingManager
")
